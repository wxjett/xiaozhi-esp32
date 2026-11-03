"""Exercise the real MOSS light driver with gated tasks and simulated GPIO/PWM."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from scripts.tests.test_moss_motor import STUBS as MOTOR_STUBS


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/moss"
STUBS = dict(MOTOR_STUBS)
STUBS["driver/ledc.h"] = r'''
#pragma once
#include "gpio.h"
constexpr int LEDC_LOW_SPEED_MODE=0, LEDC_TIMER_1=1, LEDC_CHANNEL_1=1;
constexpr int LEDC_TIMER_13_BIT=13, LEDC_AUTO_CLK=0, LEDC_INTR_DISABLE=0;
struct ledc_timer_config_t { int speed_mode,duty_resolution,timer_num,freq_hz,clk_cfg; };
struct ledc_channel_config_t { int gpio_num,speed_mode,channel,intr_type,timer_sel; unsigned duty; };
esp_err_t ledc_timer_config(const ledc_timer_config_t*);
esp_err_t ledc_channel_config(const ledc_channel_config_t*);
esp_err_t ledc_set_duty(int,int,unsigned);
esp_err_t ledc_update_duty(int,int);
esp_err_t ledc_stop(int,int,int);
'''

HARNESS = r'''
#include "moss_lights.h"
#include "driver/ledc.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
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
bool fail_task=false,fail_semaphore=false,fail_config=false,fail_timer=false,fail_channel=false;
std::atomic<int> fail_pin=-1;
std::atomic<bool> fail_set=false,fail_update=false;
std::mutex output_mutex;
int levels[49]{}; unsigned duty=0,pending_duty=0;
const char* esp_err_to_name(esp_err_t e) { return e==ESP_OK?"ESP_OK":"ESP_FAIL"; }
esp_err_t gpio_set_level(int pin,int value) {
 if(fail_pin==pin){fail_pin=-1;return ESP_FAIL;}
 std::lock_guard<std::mutex> lock(output_mutex);levels[pin]=value;return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t*) { return fail_config?ESP_FAIL:ESP_OK; }
esp_err_t ledc_timer_config(const ledc_timer_config_t* t) {
 assert(t->timer_num==1 && t->duty_resolution==13 && t->freq_hz==5000);
 return fail_timer?ESP_FAIL:ESP_OK;
}
esp_err_t ledc_channel_config(const ledc_channel_config_t* c) {
 assert(c->gpio_num==12 && c->channel==1 && c->timer_sel==1 && c->duty==0);
 std::lock_guard<std::mutex> lock(output_mutex);duty=0;return fail_channel?ESP_FAIL:ESP_OK;
}
esp_err_t ledc_set_duty(int,int,unsigned d) {
 assert(d<=8191);if(fail_set.exchange(false))return ESP_FAIL;
 std::lock_guard<std::mutex> lock(output_mutex);pending_duty=d;return ESP_OK;
}
esp_err_t ledc_update_duty(int,int) {
 if(fail_update.exchange(false))return ESP_FAIL;
 std::lock_guard<std::mutex> lock(output_mutex);duty=pending_duty;return ESP_OK;
}
esp_err_t ledc_stop(int,int,int level) {
 assert(level==0);std::lock_guard<std::mutex> lock(output_mutex);duty=0;return ESP_OK;
}
SemaphoreHandle_t xSemaphoreCreateBinary() {return fail_semaphore?nullptr:new FakeSemaphore;}
int xSemaphoreGive(SemaphoreHandle_t s) {
 std::lock_guard<std::mutex> lock(s->mutex);s->given=true;s->cv.notify_all();return 1;
}
int xSemaphoreTake(SemaphoreHandle_t s,TickType_t) {
 std::unique_lock<std::mutex> lock(s->mutex);
 assert(s->cv.wait_for(lock,std::chrono::seconds(2),[&]{return s->given;}));return 1;
}
void vSemaphoreDelete(SemaphoreHandle_t s){delete s;}
int xTaskCreate(void(*fn)(void*),const char*,unsigned,void* arg,unsigned,TaskHandle_t* out){
 if(fail_task){*out=nullptr;return 0;}
 tasks.push_back(std::make_unique<FakeTask>());auto t=tasks.back().get();*out=t;
 t->thread=std::thread([=]{current=t;fn(arg);std::lock_guard<std::mutex> lock(t->mutex);
                         t->finished=true;t->cv.notify_all();});return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t t){
 std::lock_guard<std::mutex> lock(t->mutex);++t->notifications;t->cv.notify_all();
}
unsigned ulTaskNotifyTake(int,TickType_t ticks){
 assert(ticks==10);auto t=current;std::unique_lock<std::mutex> lock(t->mutex);
 t->waiting=true;t->cv.notify_all();t->cv.wait(lock,[&]{return t->permits||t->notifications;});
 t->waiting=false;if(t->permits)--t->permits;auto n=t->notifications;t->notifications=0;return n;
}
void vTaskDelete(TaskHandle_t){}
void WaitReady(){
 auto t=tasks.back().get();std::unique_lock<std::mutex> lock(t->mutex);
 assert(t->cv.wait_for(lock,std::chrono::seconds(2),[&]{return t->waiting;}));
}
void Pulse(int count=1){
 auto t=tasks.back().get();
 for(int i=0;i<count;++i){std::unique_lock<std::mutex> lock(t->mutex);
  assert(t->cv.wait_for(lock,std::chrono::seconds(2),[&]{return t->waiting;}));
  t->waiting=false;++t->permits;t->cv.notify_all();
  assert(t->cv.wait_for(lock,std::chrono::seconds(2),[&]{return t->waiting||t->finished;}));
  assert(!t->finished);
 }
}
void Join(){for(auto& t:tasks)if(t->thread.joinable())t->thread.join();}
MossLights::Config Config(){return {{8,3,9,10,11},12};}
unsigned Pattern(){
 std::lock_guard<std::mutex> lock(output_mutex);unsigned p=0;
 for(auto pin:Config().bar_pins)p=(p<<1)|levels[pin];return p;
}
unsigned Duty(){std::lock_guard<std::mutex> lock(output_mutex);return duty;}
using Mode=MossLights::EyeMode;
int main(){
 {
  MossLights lights;assert(lights.Initialize(Config()));WaitReady();
  assert(Pattern()==0 && Duty()==0 && lights.GetStatus().ready);
  assert(lights.SetFlowing(true));assert(lights.SetEyeMode(Mode::Breathing));
  assert(tasks.size()==1);assert(lights.SetFlowing(true));assert(lights.SetEyeMode(Mode::Breathing));
  const unsigned patterns[]={17,10,12,22,5};const int ticks[]={11,10,9,10,13};
  for(int i=0;i<5;++i){assert(Pattern()==patterns[i]);Pulse(ticks[i]-1);
                      assert(Pattern()==patterns[i]);Pulse();}
  assert(Pattern()==17 && Duty()==2600); // 53 ticks = 26 PWM updates.
  assert(lights.SetEyeMode(Mode::Paused));auto held=Duty();Pulse(20);assert(Duty()==held);
  assert(lights.ResumeBreathing());assert(!lights.ResumeBreathing());Pulse(2);assert(Duty()>held);
  assert(lights.SetEyeMode(Mode::On));assert(Duty()==8191);Pulse(40);assert(Duty()==8191);
  assert(lights.SetEyeMode(Mode::Off));Pulse(40);assert(Duty()==0);
  assert(!lights.SetEyeMode(Mode::Paused));assert(!lights.ResumeBreathing());
  assert(lights.GetStatus().flowing);assert(lights.SetFlowing(false));Pulse(100);assert(Pattern()==0);
 }
 Join();assert(Pattern()==0 && Duty()==0);
 std::puts("PASS: original flow timing, concurrent effects, pause/resume, exclusive modes, stop");
 {
  MossLights lights;assert(lights.Initialize(Config()));WaitReady();
  assert(lights.SetEyeMode(Mode::Breathing));Pulse(164);assert(Duty()==8191);
  Pulse(2);assert(Duty()==8091);Pulse(162);assert(Duty()==0);Pulse(2);assert(Duty()==100);
  assert(lights.SetFlowing(true));fail_pin=8;Pulse(11);
  auto s=lights.GetStatus();assert(!s.flowing && s.bar_error==ESP_FAIL && Pattern()==0);
  assert(s.eye_mode==Mode::Breathing);assert(lights.SetFlowing(true));
  fail_update=true;Pulse(2);s=lights.GetStatus();
  assert(s.eye_mode==Mode::Off && s.eye_error==ESP_FAIL && Duty()==0 && s.flowing);
  assert(lights.SetEyeMode(Mode::On));assert(lights.GetStatus().eye_error==ESP_OK);
  fail_set=true;assert(!lights.SetEyeMode(Mode::Breathing));Pulse(20);assert(Duty()==0);
  assert(lights.SetEyeMode(Mode::Breathing));assert(!lights.SetEyeMode(static_cast<Mode>(99)));
 }
 Join();assert(Pattern()==0 && Duty()==0);
 std::puts("PASS: full PWM range, direction reversal, fault isolation and recovery, teardown");
 for(int failure=0;failure<6;++failure){
  fail_config=failure==0;fail_timer=failure==1;fail_channel=failure==2;
  fail_semaphore=failure==3;fail_task=failure==4;if(failure==5)fail_pin=8;
  MossLights lights;assert(!lights.Initialize(Config()));assert(!lights.GetStatus().ready);
  assert(!lights.SetFlowing(true));assert(!lights.SetEyeMode(Mode::On));
 }
 fail_config=fail_timer=fail_channel=fail_semaphore=fail_task=false;
 {MossLights lights;auto c=Config();c.eye_pin=8;assert(!lights.Initialize(c));
  c=Config();c.bar_pins[4]=8;assert(!lights.Initialize(c));
  c=Config();c.eye_pin=-1;assert(!lights.Initialize(c));}
 std::puts("PASS: invalid pins and GPIO/PWM/semaphore/task initialization failures");
}
'''


class MossLightsTests(unittest.TestCase):
    def test_real_driver(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("Host C++23 compiler not available")
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            for name, source in STUBS.items():
                path = temp / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(source, encoding="utf-8")
            harness = temp / "lights_test.cc"
            harness.write_text(HARNESS, encoding="utf-8")
            executable = temp / ("lights_test.exe" if os.name == "nt" else "lights_test")
            build = subprocess.run(
                [compiler, "-std=c++23", "-pthread", "-I", str(temp), "-I", str(BOARD),
                 str(harness), str(BOARD / "moss_lights.cc"), "-o", str(executable)],
                capture_output=True, text=True, timeout=60,
            )
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.count("PASS:"), 3, result.stdout)


if __name__ == "__main__":
    unittest.main()
