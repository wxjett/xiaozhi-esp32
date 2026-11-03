# MOSS

ESP32-S3 Wi-Fi 板型，基于 WiFi 面包板实现，接线对应嘉立创EDA工程
`moss_pcb_old → INMP441版本 → P1`，其中 OLED 引脚以已确认的实物接线为准。

## 引脚

| 模块 | 信号 | GPIO |
| --- | --- | --- |
| INMP441 | SD | 4 |
| INMP441 | SCK | 5 |
| INMP441 | WS | 6 |
| MAX98357A | DIN | 7 |
| MAX98357A | BCLK | 15 |
| MAX98357A | LRC | 16 |
| OLED | SDA | 1 |
| OLED | SCL | 2 |
| BOOT | 按键，低电平有效 | 0 |

原理图中三个模块均使用 3V3/GND，INMP441 的 L/R 接 GND；
MAX98357A 的 SD 接 3V3、GAIN 接 GND，均不由软件 GPIO 控制。

OLED 暂按当前工程配置使用 SSD1306 128×64，I2C 地址为 `0x3C`，
镜像方向沿用面包板配置。用户已确认屏幕能够点亮显示。

BOOT 在启动阶段点击进入配网，其余状态点击切换对话。
暂不初始化电机、眼灯、灯条、红外、触摸按键、音量按键或示例灯控制。

## 编译

初始化 ESP-IDF 环境后，在仓库根目录执行：

```sh
python scripts/build.py moss --name moss --language zh-CN
```

使用项目 ESP32-S3 默认的 16MB Flash、`partitions/v2/16m.csv` 分区表和
八线 PSRAM 配置；烧录前确认主控模组与之匹配。
固件板型与变体名称均为 `moss`，具有独立的 OTA 身份。

手动配置时，在 `Xiaozhi Assistant → Board Type` 选择 MOSS，
并将 OLED 选为 SSD1306 128×64。

编译通过后仍需在实物验证录音、唤醒、播放、OLED 显示和 BOOT 配网。

## MOSS 显示

屏幕初始化后先播放已确认的 128×64 点阵开机动画，再进入日志界面。
总时长固定为 6 秒：保留前 5 秒原动画流程，结尾额外停留 1 秒展示完整画面。
`moss_display.h` 的 `kBootDurationMs = 6000` 从首次刷新启动帧后计时；
使用 LVGL 定时器切换页面，不阻塞联网和音频启动。启动期间收到的消息会缓存。

主界面用 `U:`、`M:`、`>` 区分用户、助手和系统消息，自动换行并向上滚动。
最多保存 16 条记录，每条最多 512 字节（过长内容按 UTF-8 字符边界截断并加省略号）。
公共业务清空当前字幕时保留终端历史；移除临时消息时只删除最近一条匹配内容。
顶部为固定的 16px 状态栏：左侧显示实际 Wi-Fi 图标，右侧每 200ms 刷新设备状态、
等待计时与光标。下方 48px 日志区使用 12px 行高，可显示 4 行，长消息自动换行。
通知短暂替换状态文字，并保存在日志中；不打印虚构的思考过程，也不显示大表情。
当前没有电池采样，暂不显示电量图标。

MOSS 使用独立紧凑字库 `assets/console_font.h`，由上游现有 16px 行高字库缩放生成，
不修改公共显示类或其他板型。对话补充字形也会缩放，以免中文撑高日志行。
重新生成字库时执行（需要 Pillow 和已安装的 xiaozhi-fonts 组件）：

```sh
python main/boards/moss/assets/generate_console_font.py
```

确认后的点阵 GIF 保存在 `assets/boot.gif`，已生成的 `assets/boot_animation.h` 直接随固件编译，
不依赖外部资源目录。替换图片后，在仓库根目录运行（需要 Pillow）：

```sh
python main/boards/moss/assets/generate_boot_animation.py
```

生成器逐帧保留 GIF 的像素和时序，检查分辨率为 128×64、总时长为 6000ms。
当前共 115 帧，内置像素数据约 116KiB，不需要运行时解码 GIF 或 MP4。
播放按实际经过时间选择帧，显示较慢时跳过过期帧，避免动画越播越长。
底部小字受分辨率限制可能无法辨认。替换动画后需重新生成资源、编译并烧录。
