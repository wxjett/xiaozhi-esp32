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
