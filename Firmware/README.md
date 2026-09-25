# Firmware — ESP32-S3 固件

目标硬件：Waveshare ESP32-S3-Touch-AMOLED-2.16（SKU 33969）
框架：ESP-IDF v5.5+ + LVGL v9

固件铁律：
- 零 WZ 解析（素材由服务端下发）
- 零 Hermes / 零 HA 代码（协议无此概念）
- 设备永远是 HTTP client
- 动画只播服务端下发布局表（WZ 真实 action，禁自创）

引脚表见 ../docs/ai/waveshare-wiki-ESP32-S3-Touch-AMOLED-2.16.md
已知坑：GPIO46=PA_CTRL 功放使能（不开无声）；GPIO14/15 共享 I2C 五器件。
