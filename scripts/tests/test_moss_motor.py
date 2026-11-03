"""Run the real MOSS motor driver with simulated GPIO and a gated worker task."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MOTOR = ROOT / "main/boards/moss"

STUBS = {
    "driver/gpio.h": r'''
#pragma once
#include <cstdint>
using gpio_num_t = int;
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1;
const char* esp_err_to_name(esp_err_t);
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) ((pin)>=0 && (pin)<49)
constexpr int GPIO_MODE_OUTPUT=1, GPIO_PULLUP_DISABLE=0, GPIO_PULLDOWN_DISABLE=0, GPIO_INTR_DISABLE=0;
struct gpio_config_t { uint64_t pin_bit_mask; int mode,pull_up_en,pull_down_en,intr_type; };
esp_err_t gpio_set_level(gpio_num_t,int);
esp_err_t gpio_config(const gpio_config_t*);
''',
    "freertos/FreeRTOS.h": r'''
#pragma once
using TickType_t = unsigned;
constexpr int pdPASS=1,pdTRUE=1;
constexpr unsigned portMAX_DELAY=~0u;
#define pdMS_TO_TICKS(ms) (ms)
''',
    "freertos/task.h": r'''
#pragma once
#include "FreeRTOS.h"
struct FakeTask;
using TaskHandle_t = FakeTask*;
int xTaskCreate(void(*)(void*),const char*,unsigned,void*,unsigned,TaskHandle_t*);
void xTaskNotifyGive(TaskHandle_t);
unsigned ulTaskNotifyTake(int,TickType_t);
void vTaskDelete(TaskHandle_t);
''',
    "freertos/semphr.h": r'''
#pragma once
#include "FreeRTOS.h"
struct FakeSemaphore;
using SemaphoreHandle_t = FakeSemaphore*;
SemaphoreHandle_t xSemaphoreCreateBinary();
int xSemaphoreTake(SemaphoreHandle_t,TickType_t);
int xSemaphoreGive(SemaphoreHandle_t);
void vSemaphoreDelete(SemaphoreHandle_t);
''',
    "esp_log.h": "#pragma once\n#define ESP_LOGI(...)\n#define ESP_LOGE(...)\n",
}

HARNESS = r'''
#include "moss_motor.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

struct FakeTask {
 std::mutex mutex; std::condition_variable cv; std::thread thread;
 unsigned permits=0,notifications=0; bool waiting=false,finished=false;
};
struct FakeSemaphore { std::mutex mutex; std::condition_variable cv; bool given=false; };
thread_local FakeTask* current=nullptr;
std::vector<std::unique_ptr<FakeTask>> tasks;
std::atomic<bool> fail_task=false,fail_semaphore=false,fail_config=false;
std::atomic<int> fail_pin=-1;
std::mutex gpio_mutex; int levels[49]{}; int high_writes=0;
const char* esp_err_to_name(esp_err_t e) { return e==ESP_OK ? "ESP_OK" : "ESP_FAIL"; }
esp_err_t gpio_set_level(int pin,int value) {
 if(fail_pin==pin) { fail_pin=-1; return ESP_FAIL; }
 std::lock_guard<std::mutex> lock(gpio_mutex);
 levels[pin]=value; high_writes+=value!=0; return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t*) { return fail_config.exchange(false)?ESP_FAIL:ESP_OK; }
SemaphoreHandle_t xSemaphoreCreateBinary() { return fail_semaphore.exchange(false)?nullptr:new FakeSemaphore; }
int xSemaphoreGive(SemaphoreHandle_t s) {
 std::lock_guard<std::mutex> lock(s->mutex); s->given=true; s->cv.notify_all(); return 1;
}
int xSemaphoreTake(SemaphoreHandle_t s,TickType_t) {
 std::unique_lock<std::mutex> lock(s->mutex);
 assert(s->cv.wait_for(lock,std::chrono::seconds(2),[&]{return s->given;}));return 1;
}
void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }
int xTaskCreate(void(*fn)(void*),const char*,unsigned,void* arg,unsigned,TaskHandle_t* out) {
 if(fail_task.exchange(false)) { *out=nullptr;return 0; }
 tasks.push_back(std::make_unique<FakeTask>());auto t=tasks.back().get();*out=t;
 t->thread=std::thread([=]{current=t;fn(arg);std::lock_guard<std::mutex> lock(t->mutex);
                         t->finished=true;t->cv.notify_all();});return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t t) {
 std::lock_guard<std::mutex> lock(t->mutex);++t->notifications;t->cv.notify_all();
}
unsigned ulTaskNotifyTake(int,TickType_t ticks) {
 assert(ticks==10);auto t=current;std::unique_lock<std::mutex> lock(t->mutex);
 t->waiting=true;t->cv.notify_all();
 t->cv.wait(lock,[&]{return t->permits||t->notifications;});t->waiting=false;
 if(t->permits)--t->permits;auto notifications=t->notifications;t->notifications=0;return notifications;
}
void vTaskDelete(TaskHandle_t) {}
void WaitReady() {
 auto t=tasks.back().get();std::unique_lock<std::mutex> lock(t->mutex);
 assert(t->cv.wait_for(lock,std::chrono::seconds(2),[&]{return t->waiting;}));
}
void Pulse() {
 auto t=tasks.back().get();std::unique_lock<std::mutex> lock(t->mutex);
 assert(t->cv.wait_for(lock,std::chrono::seconds(2),[&]{return t->waiting;}));
 t->waiting=false;++t->permits;t->cv.notify_all();
 assert(t->cv.wait_for(lock,std::chrono::seconds(2),[&]{return t->waiting||t->finished;}));
 assert(!t->finished);
}
void JoinTasks() { for(auto& t:tasks)if(t->thread.joinable())t->thread.join(); }
MossMotor::Config Config(int per_rev=360) {
 return {{{{13,14,21,46},{42,41,40,39}}},per_rev,10,90};
}
int Phase(const std::array<int,4>& pins) {
 std::lock_guard<std::mutex> lock(gpio_mutex);int value=0;
 for(auto pin:pins)value=(value<<1)|levels[pin];return value;
}
using Axis=MossMotor::Axis;
int main() {
 {
  MossMotor motor;auto config=Config();assert(motor.Initialize(config));WaitReady();
  assert(high_writes==0);assert(!motor.GetStatus().axes[0].moving);
  assert(motor.MoveRelative(Axis::Pitch,8));assert(motor.MoveRelative(Axis::Yaw,-8));
  const int forward[]={8,12,4,6,2,3,1,9},reverse[]={9,1,3,2,6,4,12,8};
  for(int i=0;i<8;++i){Pulse();assert(Phase(config.pins[0])==forward[i]);assert(Phase(config.pins[1])==reverse[i]);}
  auto status=motor.GetStatus();assert(status.axes[0].energized && status.axes[0].remaining_half_steps==0);
  Pulse();status=motor.GetStatus();assert(!status.axes[0].moving&&!status.axes[1].moving);
  assert(status.axes[0].executed_half_steps==8 && status.axes[1].executed_half_steps==-8);
  assert(Phase(config.pins[0])==0 && Phase(config.pins[1])==0);
 }
 JoinTasks();std::puts("PASS: zero-output startup, eight-phase direction, two axes, final phase dwell");
 {
  MossMotor motor;assert(motor.Initialize(Config()));WaitReady();
  for(int i=0;i<4;++i)assert(motor.MoveRelative(Axis::Pitch,3));
  assert(!motor.MoveRelative(Axis::Pitch,1));assert(motor.MoveRelative(Axis::Yaw,5));
  Pulse();auto before=motor.GetStatus();assert(before.axes[0].queued_commands==3);
  assert(motor.Stop(Axis::Pitch));
  for(int i=0;i<5;++i)Pulse();auto after=motor.GetStatus();
  assert(!after.axes[0].moving && after.axes[0].queued_commands==0);
  assert(after.axes[0].executed_half_steps==before.axes[0].executed_half_steps);
  assert(after.axes[1].executed_half_steps==5);assert(Phase(Config().pins[0])==0);
  assert(motor.MoveRelative(Axis::Pitch,1));assert(motor.MoveRelative(Axis::Pitch,-1));
  Pulse();assert(motor.GetStatus().axes[0].executed_half_steps==before.axes[0].executed_half_steps+1);
  Pulse();assert(motor.GetStatus().axes[0].executed_half_steps==before.axes[0].executed_half_steps);
  Pulse();assert(!motor.GetStatus().axes[0].moving);
 }
 JoinTasks();std::puts("PASS: bounded FIFO, stop clears only selected axis, no stale output, queued reversal");
 {
  MossMotor motor;assert(motor.Initialize(Config(4096)));WaitReady();
  assert(!motor.MoveRelative(Axis::Pitch,91));assert(!motor.MoveRelative(Axis::Pitch,std::numeric_limits<int>::min()));
  assert(!motor.MoveRelative(static_cast<Axis>(9),1));assert(motor.MoveRelative(Axis::Pitch,0));
  assert(motor.GetStatus().axes[0].queued_commands==0);assert(motor.MoveRelative(Axis::Pitch,90));
  for(int i=0;i<1024;++i)Pulse();auto status=motor.GetStatus();
  assert(status.axes[0].executed_half_steps==1024 && status.axes[0].remaining_half_steps==0);
  Pulse();assert(!motor.GetStatus().axes[0].moving);
 }
 JoinTasks();std::puts("PASS: legacy 4096 half-steps conversion, zero command, range and enum validation");
 {
  MossMotor motor;assert(motor.Initialize(Config()));WaitReady();
  assert(motor.MoveRelative(Axis::Pitch,3));fail_pin=13;Pulse();
  auto status=motor.GetStatus();assert(status.axes[0].last_error==ESP_FAIL);
  assert(status.axes[0].executed_half_steps==0 && status.axes[0].queued_commands==0);
  assert(!motor.MoveRelative(Axis::Pitch,1));assert(motor.Stop(Axis::Pitch));
  assert(motor.MoveRelative(Axis::Pitch,1));Pulse();assert(motor.GetStatus().axes[0].executed_half_steps==1);
 }
 JoinTasks();std::puts("PASS: GPIO error clears pending motion, stop recovers, shutdown releases coils");
 for(int failure=0;failure<3;++failure) {
  MossMotor motor;if(failure==0)fail_task=true;if(failure==1)fail_semaphore=true;if(failure==2)fail_config=true;
  assert(!motor.Initialize(Config()));assert(!motor.GetStatus().ready);assert(!motor.MoveRelative(Axis::Pitch,1));
 }
 {MossMotor motor;auto config=Config();config.pins[1][0]=13;assert(!motor.Initialize(config));}
 assert(Phase(Config().pins[0])==0 && Phase(Config().pins[1])==0);
 std::puts("PASS: task/semaphore/GPIO initialization failures and duplicate pins");
}
'''


@unittest.skipUnless(shutil.which("c++"), "requires a host C++23 compiler")
class MossMotorTests(unittest.TestCase):
    def test_driver_with_simulated_hardware(self):
        with tempfile.TemporaryDirectory(prefix="moss-motor-") as directory:
            temp = Path(directory)
            for name, text in STUBS.items():
                target = temp / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text, encoding="utf-8")
            source = temp / "test.cc"
            source.write_text(HARNESS, encoding="utf-8")
            binary = temp / ("motor-test.exe" if os.name == "nt" else "motor-test")
            subprocess.run([shutil.which("c++"), "-std=c++23", "-pthread", "-O2", "-I", str(temp),
                            "-I", str(MOTOR), str(source), str(MOTOR / "moss_motor.cc"),
                            "-o", str(binary)], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count("PASS:"), 5, result.stdout)


if __name__ == "__main__":
    unittest.main()
