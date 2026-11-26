# Bring-up progress

Living check-in log for the next week and a half. Newest rows first. A component is not done until it passes three times in a row and someone other than the author has seen it.

Branch: `test/esp32s3-camera-bringup`

| Date | Component | Tester | Result | Measured | Notes |
| --- | --- | --- | --- | --- | --- |
| 2026-11-25 | ESP32-S3 camera bring-up (repo) | — | PASS | — | Branch renamed to `test/esp32s3-camera-bringup`. Camera firmware, host tools, Arduino alternate, and docs committed. Camera hardware passes exist; still need two more witnessed runs to close the component. |
| 2026-11-24 | ESP32-S3 camera (ESP-IDF) | agent | PASS | GC0308 PID 0x009b; 5 JPEG frames CRC/SOI/EOI OK | Freenove FNK0085 on COM5. Firmware: `firmware/esp32s3_cam`. Host: `tools/capture_frame.py`. Sensor has no hardware JPEG; firmware uses RGB565 + software JPEG. Factory 8 MB dump kept locally under `backups/` (gitignored). On-device firmware as of this date. |
| 2026-11-24 | ESP32-S3 camera (Arduino) | agent | PASS | PSRAM 8192 KB; 30/30 @ 320x240, 20.0 fps | Sketch: `test/hardware/esp32_camera`. Later overwritten on-device by ESP-IDF image. Useful as a simpler reflash path. |
| 2026-11-24 | VL53L0X single | — | BLOCKED | — | Sketch at `test/hardware/tof_single` written but not built/flashed. Rewire first: SDA→D14, SCL→D15, XSHUT→D10 (was swapped / on D7). Nucleo seen as COM4 + `NODE_F401RE`. |

## Firmware currently on boards

| Board | Firmware | As of |
| --- | --- | --- |
| ESP32-S3 FNK0085 | `firmware/esp32s3_cam` (ESP-IDF via PlatformIO) | 2026-11-24 |
| Nucleo F401RE | stock / unknown | 2026-11-25 |

## Required to close the camera bring-up

Camera has two agent PASSes (Arduino + ESP-IDF). Per the project rule, close it only after:

- [ ] Run 1: `python tools\capture_frame.py --port COM5 --baud 921600 --out captures --count 3` — log date, frame count, any failures
- [ ] Run 2: same command, different time/lighting if possible
- [ ] Run 3: same command, witnessed by a second person (name in the Tester column)
- [ ] Note sensor still reports GC0308; spot-check one JPEG opens and looks sane
- [ ] Optional: record flicker/banding under fluorescent lights (known; not a fail by itself)

## Open items (after camera)

- [ ] Rewire VL53L0X (SDA D14, SCL D15, XSHUT D10); leave GPIO1 unconnected
- [ ] Build + flash `test/hardware/tof_single`; ruler check ±5% at 100 mm and 300 mm
- [ ] Decide Nucleo production toolchain (Mbed CE vs STM32Cube HAL) — see archive plan
