# rc-slam

Embedded bring-up for a maze-robot stack: Nucleo F401RE as the main controller, VL53L0X distance sensors, and a Freenove ESP32-S3 camera as a stretch goal.

## Hardware on the desk

| Board | Notes |
| --- | --- |
| Freenove FNK0085 ESP32-S3 (N8R8) | Camera + 8 MB flash + 8 MB PSRAM. USB serial via onboard CH343 (typically `COM5`). Sensor: **GC0308** (no hardware JPEG). |
| Nucleo F401RE | Main MCU. ST-Link VCP typically `COM4`; mass-storage label `NODE_F401RE`. |
| VL53L0X | Single-sensor bring-up next. Planned wiring: VIN→3V3, GND→GND, SDA→D14, SCL→D15, XSHUT→D10. |

## Layout

```text
firmware/esp32s3_cam/          ESP-IDF camera firmware (serial frame protocol)
test/hardware/esp32_camera/    Arduino alternate camera bring-up
test/hardware/tof_single/      Nucleo single VL53L0X bring-up (not flashed yet)
tools/capture_frame.py         Host capture for the ESP-IDF firmware
docs/PROGRESS.md               Living bring-up log (next ~1.5 weeks)
docs/archive/                  Longer planning notes (pin map, BOM, schedule)
```

## Host setup

```powershell
python -m venv venv
.\venv\Scripts\Activate.ps1
pip install -r requirements.txt
```

Firmware builds use [PlatformIO](https://platformio.org/), not pip.

## Camera (ESP-IDF path)

Board should be running `firmware/esp32s3_cam`. From the repo root:

```powershell
python tools\capture_frame.py --port COM5 --baud 921600 --out captures --count 5
```

Arduino alternate (reflash first):

```powershell
cd test\hardware\esp32_camera
python -m platformio run -t upload --upload-port COM5
python capture.py --port COM5 --out captures\shot.jpg
```

## Next up

1. Finish VL53L0X wiring (SDA/SCL/XSHUT as above).
2. Build and flash `test/hardware/tof_single` to the Nucleo; compare readings to a ruler at 100 mm and 300 mm.
3. Log each attempt in [docs/PROGRESS.md](docs/PROGRESS.md).

Full team planning notes (toolchain, pin map, bring-up order) live under [docs/archive/maze-robot-team-plan.md](docs/archive/maze-robot-team-plan.md).
