# 1.85B / 216 双板烧录操作手册

> 板卡串口会互换，**每次烧录前必须读 MAC 确认板卡**。
> 当前已知端口映射（会变）：21101 = 1.85B(78:0e:04)、21201 = 216(60:da:c0)

---

## 1. 确认板卡身份

```bash
# 读 MAC：78:0e:04 = 1.85B，60:da:c0 = 216
esptool.py --port /dev/cu.usbmodem21101 --chip esp32s3 read_mac
# 输出 MAC: 44:bd:8d:78:0e:04 → 这是 185B
```

## 2. 1.85B 板（ST77916 360×360 圆屏）

### 2.1 构建

```bash
cd /Users/a502/IdeaProjects/minipet-esp32/Firmware/board185b
source ~/esp/esp-idf/export.sh > /dev/null 2>&1
idf.py build
```

⚠️ 不要用 `-B build-185b`（会创建嵌套目录+错误 flash size）。
项目标准构建目录 = `board185b/build/`。

### 2.2 烧录固件

```bash
cd build
esptool.py --port /dev/cu.usbmodem21101 --chip esp32s3 --baud 921600 write_flash @flash_args
```

### 2.3 仅烧素材（不重烧固件）

```bash
# 素材来源：Server/tools/rebuild-flash-assets.sh --server http://192.168.3.46:38090 \
#   --device dev-693ea4 --port /dev/cu.usbmodem21101
# 或手动：
esptool.py --port /dev/cu.usbmodem21101 --chip esp32s3 --baud 921600 \
  write_flash 0x620000 /tmp/minipet-assets.XXXXXX/assets.bin
```

### 2.4 擦除 NVS（清配网/E14 计数/strike）

```bash
esptool.py --port /dev/cu.usbmodem21101 --chip esp32s3 erase_region 0x9000 0xF000
```

## 3. 216 板（CO5300 480×480 AMOLED）

### 3.1 构建

```bash
cd /Users/a502/IdeaProjects/minipet-esp32/Firmware/board216
source ~/esp/esp-idf/export.sh > /dev/null 2>&1
idf.py build
```

⚠️ board216/ 是另一会话的工作树——**构建前先 git status 确认没有未提交的半成品**。

### 3.2 烧录

```bash
cd build
esptool.py --port /dev/cu.usbmodem21601 --chip esp32s3 --baud 921600 write_flash @flash_args
```

⚠️ **216 板是另一会话在用的板子——烧录前必须获得用户许可**。

### 3.3 仅烧素材

```bash
esptool.py --port /dev/cu.usbmodem21601 --chip esp32s3 --baud 921600 \
  write_flash 0x620000 /tmp/minipet-assets.XXXXXX/assets.bin
```

素材来源同上（rebuild-flash-assets.sh，改 --port）。

## 4. 抓串口日志（不触发复位的写法）

```python
# /Users/a502/mp-work/capture_boot.py
import serial, time, sys
port = sys.argv[1]; dur = int(sys.argv[2]); out = sys.argv[3]
ser = serial.Serial(port, 115200, timeout=0.2, dsrdtr=False)
ser.dtr = False; ser.rts = False   # 不触发复位
end = time.time() + dur
buf = []
while time.time() < end:
    d = ser.read(4096)
    if d: buf.append(d)
open(out, 'wb').write(b''.join(buf))
```

```bash
python3 /Users/a502/mp-work/capture_boot.py /dev/cu.usbmodem21101 30 /Users/a502/mp-work/boot_log.txt
```

⚠️ 如果需要重启触发新日志：
```bash
esptool.py --port /dev/cu.usbmodem21101 --chip esp32s3 --after hard_reset run
# 然后立刻启动 capture（注意 capture 脚本本身也会触发复位）
```

## 5. 素材重导出（重新生成 assets.bin）

```bash
cd /Users/a502/IdeaProjects/minipet-esp32
export DOTNET_ROOT=/Volumes/SSD/C#
# 480 档（216 板用）：
Server/tools/rebuild-flash-assets.sh --server http://192.168.3.46:38090 \
  --device dev-693ea4 --port /dev/cu.usbmodem21601
# 360 档（185B 用）：
export FACTORY_PROFILE=lcd185b
Server/tools/rebuild-flash-assets.sh --server http://192.168.3.46:38090 \
  --device dev-693ea4 --port /dev/cu.usbmodem21101
```

## 6. 构建/烧录 1.85B 专用的坑

| 坑 | 说明 |
|---|---|
| `-B build-185b` 嵌套 | 在 board185b/ 里再 `-B build-185b` 会创建嵌套目录+错误 flash size。**在 board185b/ 里用 `idf.py build`（默认 build/）** |
| sdkconfig | board185b 有自己的 sdkconfig（含 MP_ASSET_HEAP_GATE_KB=6 等）。**不要**用根目录的 sdkconfig |
| flash size | 板=16MB。如果构建报"doesn't fit in 2MB"→ 检查 sdkconfig 里 CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y |
| 串口竞争 | 另一会话可能在用同一 COM 口——报"multiple access" = 等几秒重试 |
| 素材版本 | vbCYqT=480 相机裁剪（娃娃正确但地图比例 216 视角）；KfNTSS=FitWholeMap（整图但上半地形不对，待修条带坐标） |
