# Maze Robot — Team Project Plan (Nucleo F401RE)

> Archived from root `Tab 1.md`. Kept as reference for pin map, BOM, and bring-up criteria. Day-to-day status lives in [`docs/PROGRESS.md`](../PROGRESS.md); the portfolio-facing overview is the root README.

Nov 24, 2026 · @bigpenis123

## Toolchain: use Mbed CE with CMake

Mbed reached end of life four months ago, so Mbed Studio is no longer a safe primary toolchain for this project. Arm announced that [Mbed OS and the Mbed Platform reach end of life in July 2026](https://www.arm.com/products/iot/mbed-os), at which point the website is archived and projects can no longer be built in Arm's online tools. Arm had already [halted active maintenance and CI on the codebase](https://os.mbed.com/blog/entry/Important-Update-on-Mbed/), including security support, and the repository is now archived on GitHub.

The practical consequence from [Arm's own FAQ](https://forums.mbed.com/t/important-update-on-mbed-end-of-life/23644): you cannot build an Mbed project inside Keil Studio Cloud or Mbed Studio, though building with GCC through the Mbed CLI may still work. The same post points to **Mbed CE**, a community fork under active development, which treats offline compilation as the normal way of building.

### The recommendation

Use **Mbed CE + CMake + arm-none-eabi-gcc** in whichever editor the team prefers. This keeps the Mbed API you already know from your other class, builds offline with no dependence on archived Arm services, and because it is a plain CMake project, CMake Tools and clangd (or equivalent) give you full symbol navigation and completion against the real headers. Nothing about the project is locked to one editor.

### How Mbed Studio and a normal editor coexist

An Mbed CE project is a CMake project and a git repository. Any editor can open the same folder:

- **Day-to-day editing** happens in a normal code editor. CMake configures and builds; clangd (or the editor's language server) reads `compile_commands.json` for completion against the real Mbed headers.
- **Mbed Studio** stays available for anyone on the team who prefers its board detection and one-click flash, as long as their existing install still builds. Treat it as a convenience, not the source of truth.
- **The build must be reproducible from the command line.** If `cmake --preset release && cmake --build build/release` works in a terminal, it works everywhere, and your CI can run it. Any step that only exists inside a GUI is a step that breaks when one teammate's install differs.

If your course requires Mbed Studio specifically, confirm with your instructor before the first meeting, since it changes the repository layout.

### If Mbed CE gives you trouble

Fall back to **STM32Cube HAL + CMake**, which ST supports directly. You lose the Mbed convenience layer and write more setup code, but you gain a toolchain that will still be maintained after this semester. Decide this in the first week; switching later is expensive.

| Option | Build offline | Editor / IDE support | Maintained | Setup cost |
| --- | --- | --- | --- | --- |
| Mbed CE + CMake | Yes | Full, via clangd / CMake | Community fork | Medium |
| STM32Cube HAL + CMake | Yes | Full, via clangd / CMake | Yes, by ST | High |
| Mbed Studio alone | Existing installs only | Mbed Studio only | No, EOL July 2026 | Low |
| PlatformIO + Arduino | Yes | Full | Yes | Lowest |

## Debug output: make it non-blocking, don't remove it

The F401RE is not your bottleneck, so trading away serial output is solving the wrong problem. The chip runs an 84 MHz Cortex-M4F with 512 KB of flash and 96 KB of SRAM. A wall-following controller uses a few hundred bytes of state and a handful of floating-point operations per cycle. You will not run out of either.

What will actually limit your loop rate is the VL53L0X. Each ranging measurement takes roughly 20-33 ms depending on the timing budget you configure, and three sensors read one after another on a shared I2C bus puts a full scan somewhere near 60-100 ms. That caps your control loop at 10-16 Hz, and it is I/O-bound waiting on the sensor, not compute-bound waiting on the CPU. Measure this on your own hardware before you optimize anything.

### What serial output actually costs

The cost is real but specific, and all three parts are fixable without giving up visibility:

1. **Blocking transmission.** A `printf` that busy-waits on the UART at 115200 baud spends about 87 microseconds per byte. A 60-character debug line blocks your loop for roughly 5 ms. In a 20 ms control cycle that is a quarter of your budget, spent doing nothing.
2. **Formatting cost.** Full `printf` with `%f` pulls newlib's floating-point formatter into flash and costs hundreds of microseconds per call. Integer-only formatting is dramatically cheaper.
3. **Flash footprint.** The formatter itself is tens of kilobytes. On 512 KB this does not matter, but it is the part people usually notice first.

### What to do instead

- **Raise the baud rate to 921600.** The ST-Link virtual COM port handles it, and the per-byte cost drops to about 11 microseconds. This is a one-line change and the single biggest win.
- **Put the UART behind a DMA ring buffer.** Log calls copy into a buffer and return immediately; DMA drains it in the background. Costs about 1-2 KB of your 96 KB SRAM and removes the blocking cost entirely. This is the change that actually answers your concern.
- **Use compile-time log levels.** Wrap output in `LOG_ERR`, `LOG_INFO`, `LOG_DBG` macros gated on a `LOG_LEVEL` define. Debug builds print everything, competition builds compile the calls to nothing, and you never delete code to get speed back.
- **Avoid `%f` in the hot loop.** Print distances as integer millimetres and headings as integer tenths of a degree. You lose nothing readable and skip the float formatter.
- **Never log inside an interrupt handler.** Set a flag and log from the main loop.

### If you still want more headroom

SWO trace through the ST-Link costs far less CPU time than UART and is worth knowing about. Two cautions before you plan around it: on Nucleo-64 boards the SWO pin (PB\_3) is often not connected to the ST-Link by default and may need a solder bridge closed, so check your board's user manual first. The pin map below leaves PB\_3 free for exactly this reason.

| Transport | Approx. cost per 60-char line | Setup effort | Keep for competition |
| --- | --- | --- | --- |
| Blocking UART, 115200 | \~5 ms | None | No |
| Blocking UART, 921600 | \~650 us | One line | Acceptable |
| DMA ring buffer, 921600 | Under 10 us of CPU | A few hours | Yes |
| SWO / ITM trace | Near zero | Board-dependent | Yes, if the pin works |

The honest summary: spend an afternoon on the DMA ring buffer in week one and you will never have to choose between debugging and performance for the rest of the project.

## Parts, power, and the pin map

The pin map below is the contract between the three of you. Once it is agreed, it lives in `include/board_pins.h` and nobody changes a pin without telling the other two, because a silent pin change is the hardest bug on this project to find.

### Bill of materials

| Part | Qty | Role | Bus / interface |
| --- | --- | --- | --- |
| Nucleo F401RE | 1 | Main controller, 84 MHz, 96 KB SRAM | - |
| VL53L0X ToF module | 3 | Front, left, right wall ranging | I2C1 |
| GY-521 (MPU-6050) | 1 | Yaw rate for 90-degree turns | I2C1 |
| L298N motor driver | 2 | Drive motors (one is spare) | GPIO + PWM |
| 2WD chassis kit | 1 | Frame, motors, wheels | - |
| LM2596 buck converter | 2 | Battery to regulated 5 V | - |
| 6-cell AA battery holder | 1 | Main power, approx. 9 V | - |
| SG90 micro servo | 4 | Optional sensor pan (one used) | PWM |
| ESP32-S3 camera | 1 | Out of scope for v1 | - |

Treat the ESP32-S3 as a stretch goal. It is a second microcontroller with its own toolchain, and adding it before the robot drives reliably will cost you a week you do not have.

### Power architecture

Keep two supply rails and tie them together only at ground. Motors inject noise and voltage sag that will reset the Nucleo if they share a rail with it.

1. **Motor rail** runs straight from the 6xAA pack (approx. 9 V) into the L298N motor supply terminal.
2. **Logic rail** runs from the same pack through the LM2596, set to 5 V before you connect anything, into the Nucleo's 5 V pin.
3. **Set the LM2596 output with a multimeter while it is disconnected from the board.** These arrive at arbitrary voltages and a 12 V output into the 5 V pin kills the Nucleo.
4. **Sensors run from the Nucleo's 3.3 V pin**, not 5 V, so their I2C lines idle at a level the MCU expects.
5. **One common ground** joins the battery negative, the LM2596 ground, the L298N ground, and the Nucleo ground. Without it nothing works and the failure looks random.

The L298N drops roughly 2 V across its output stage, so your motors see about 7 V from a 9 V pack. Budget for that when you tune speeds.

### Pin map

Mbed pin names are given alongside the Arduino header labels silkscreened on the board.

| Function | Header | Mbed pin | Peripheral | Notes |
| --- | --- | --- | --- | --- |
| I2C SDA | D14 | PB\_9 | I2C1 | Shared by all 4 sensors |
| I2C SCL | D15 | PB\_8 | I2C1 | Pull-ups are on the breakouts |
| Left motor PWM (ENA) | D9 | PC\_7 | TIM3\_CH2 |  |
| Left motor IN1 | D7 | PA\_8 | GPIO |  |
| Left motor IN2 | D8 | PA\_9 | GPIO |  |
| Right motor PWM (ENB) | D5 | PB\_4 | TIM3\_CH1 | Same timer as left |
| Right motor IN3 | D4 | PB\_5 | GPIO |  |
| Right motor IN4 | D2 | PA\_10 | GPIO |  |
| XSHUT front | D10 | PB\_6 | GPIO | Drives I2C address assignment |
| XSHUT left | D11 | PA\_7 | GPIO |  |
| XSHUT right | D12 | PA\_6 | GPIO |  |
| Servo PWM | D6 | PB\_10 | TIM2\_CH3 | Optional sensor pan |
| Battery sense | A0 | PA\_0 | ADC1\_IN0 | Through a divider |
| Debug UART | - | PA\_2 / PA\_3 | USART2 | Reserved for ST-Link VCP |
| Onboard LED | D13 | PA\_5 | GPIO | Reserved, do not reuse |
| SWO trace | D3 | PB\_3 | - | Left free deliberately |

Three things to watch. PA\_5 drives the onboard LD2 LED, so do not assign it. PA\_2 and PA\_3 carry the ST-Link virtual COM port, so leaving them alone is what keeps your debug output working. And if you later add wheel encoders, TIM4's channels land on PB\_6 and PB\_7, which collides with the front XSHUT line above, so move that XSHUT to a free pin at the same time.

## Repository structure

One repository, one CMake build, every driver in its own file pair. The layout below separates code that touches hardware from code that does not, which is what makes the logic testable on your laptop instead of only on the robot.

```markdown
maze-robot/
  CMakeLists.txt            top-level build
  CMakePresets.json         debug / release / host-test presets
  README.md                 how to build and flash, in 10 lines
  mbed-os.lib               pins Mbed CE to one commit
  include/
    board_pins.h            THE pin map, single source of truth
    config.h                tunables: speeds, thresholds, timing budgets
    log.h                   LOG_ERR / LOG_INFO / LOG_DBG macros
  src/
    main.cpp                wiring only, no logic
    drivers/
      motor.h / motor.cpp
      tof_array.h / tof_array.cpp
      imu.h / imu.cpp
      servo_pan.h / servo_pan.cpp
      battery.h / battery.cpp
      log_uart.cpp           DMA ring buffer behind log.h
    control/
      wall_follow.h / .cpp    pure logic, no hardware calls
      turn_controller.h / .cpp
  test/
    host/                   unit tests, run on your laptop
    hardware/               one bring-up program per component
  cad/
    *.FCStd                 FreeCAD sources
    stl/                    printable exports
  docs/
    test-log.md             the filled-in bring-up results
    wiring.md               photos and a wiring table
  .github/workflows/ci.yml  builds every push
```

### Rules for the layout

- **`board_pins.h` is the only place a pin number appears.** If you find `PB_9` typed anywhere else, that is a bug waiting to happen.
- **`control/` must not include any Mbed header.** It takes distances in and returns motor commands out. That restriction is what lets you run it as a host test with fake sensor values and catch logic errors without touching the robot.
- **One component, one file pair.** `tof_array.cpp` owns all three sensors because their address assignment is a single interleaved sequence; splitting it per sensor would be worse.
- **`test/hardware/` holds one small program per component**, each buildable on its own. These are what you run during bring-up and what you re-run when something breaks later.
- **`config.h` holds every tunable number.** Wall-follow setpoint, turn speed, timing budget, loop period. Tuning should never mean editing logic.

### Git hygiene

Commit the `CMakePresets.json` and the Mbed CE revision pin; do not commit `build/`, `.vscode/`, or Mbed Studio's generated folders. Add a `.gitignore` in the first commit, before anyone builds, because untangling a repository that has 200 MB of build output in its history is an afternoon none of you will enjoy.

## Firmware architecture

Three layers, and one rule that matters more than the rest: nothing in `control/` may include a hardware header. That restriction is what lets you run the maze logic as a host test with fake sensor values, which is where you will catch most of your bugs.

&#91;embedded content: firmware layers · 4 bands, host-test boundary marked\]

Calls only ever go downward. A driver never calls control logic, and control logic never reaches past the function signatures below.

### Driver interfaces

Agree on these signatures before anyone writes an implementation, so the three tracks can work in parallel against headers that already exist.

```markdown
// motor.h
void  motor_init(void);
void  motor_set(int16_t left_pct, int16_t right_pct);  // -100..100
void  motor_stop(void);

// tof_array.h
typedef enum { TOF_FRONT, TOF_LEFT, TOF_RIGHT, TOF_COUNT } tof_id_t;
bool     tof_init(void);              // assigns the 3 I2C addresses
bool     tof_read_all(uint16_t mm[TOF_COUNT]);
uint32_t tof_last_scan_us(void);      // measure before tuning the loop

// imu.h
bool  imu_init(void);
void  imu_calibrate(uint32_t ms);     // robot must be still
void  imu_update(float dt_s);
float imu_yaw_deg(void);
void  imu_zero_yaw(void);

// servo_pan.h
void servo_init(void);
void servo_set_deg(uint8_t deg);      // 0..180

// battery.h
void  battery_init(void);
float battery_volts(void);
bool  battery_low(void);

// log.h  — compiles to nothing when LOG_LEVEL excludes it
LOG_ERR(fmt, ...)  LOG_INFO(fmt, ...)  LOG_DBG(fmt, ...)
```

### Control interfaces

These two take plain numbers and return plain numbers, which is exactly why they are testable without a robot.

```markdown
// wall_follow.h
typedef struct { int16_t left_pct, right_pct; } drive_cmd_t;
typedef struct { uint16_t front_mm, left_mm, right_mm; float yaw_deg; } sensors_t;

drive_cmd_t wall_follow_step(const sensors_t *s, float dt_s);

// turn_controller.h
void        turn_begin(float target_deg);
drive_cmd_t turn_step(float yaw_deg, float dt_s);
bool        turn_done(void);
```

### Two conventions worth fixing now

- **Distances are `uint16_t` millimetres, never floats.** Integer maths, no formatter cost when logging, and no ambiguity about units.
- **Every `*_init()` returns `bool`, and `main` checks it.** A sensor that silently failed to initialise reads zero, and a wall-follower that believes a wall is zero millimetres away behaves spectacularly badly.

## Bring-up order and test protocol

Bring up one component at a time, and do not start the next until the current one has a written pass. The discipline matters more than the speed: when the integrated robot misbehaves in week five, the test log is what tells you which subsystem to suspect.

### The rule

Every component gets a standalone program in `test/hardware/`, a numeric pass criterion, and a row in `docs/test-log.md`. A component is not done when it works once. It is done when it works three times in a row and somebody other than the author has seen it work.

### Order and pass criteria

| # | Component | Standalone test | Pass criterion |
| --- | --- | --- | --- |
| 0 | Toolchain | Blink + serial hello | LED blinks, serial readable, all 3 laptops build |
| 1 | Logging | Flood 1000 lines | No dropped lines, loop time unchanged |
| 2 | Motors | Each direction, each side | Both wheels spin both ways, no brownout |
| 3 | Single ToF | Front sensor only | Reading within 5% of a ruler at 100 and 300 mm |
| 4 | ToF array | All 3, address assignment | All 3 report distinct addresses, 3 plausible readings |
| 5 | IMU | Gyro bias + yaw integration | Hand-turn 90 degrees reads 90 +/- 5 after 10 s |
| 6 | Servo | 0 to 180 sweep | Reaches both ends, no jitter at rest |
| 7 | Battery sense | Compare to multimeter | Within 0.2 V across 7-10 V |
| 8 | Integration | Drive a straight 1 m corridor | Stays centred, stops before the wall |

### Test log format

Keep `docs/test-log.md` in the repository, not in a chat thread. One row per attempt, newest first:

```markdown
| Date | Component | Tester | Result | Measured | Notes |
| 2026-11-26 | ToF array | Sam | PASS | 98/301/499 mm | Right sensor reads 8mm low, within spec |
| 2026-11-25 | Motors | Alex | FAIL | - | Brownout at 80% duty, LM2596 set to 4.1V |
```

A FAIL row is more valuable than a PASS row. Record the failure and what fixed it, because the same failure will happen again on competition day and you will want the note.

### Two measurements worth taking early

- **Full ToF scan time.** Read all three sensors in a loop and print the elapsed microseconds. This number sets your control loop period and you cannot plan the controller without it.
- **Gyro drift over 60 seconds.** Leave the robot still, integrate yaw, and record the error. If it drifts more than a few degrees a minute you need to recalibrate before every run, which is a thing to design in rather than discover.

### Host tests

Anything in `control/` gets a unit test that runs on your laptop with no hardware attached. Feed `wall_follow_step()` a sequence of fake distances and assert the motor commands. These run in CI on every push and catch the logic bugs that are miserable to find on a moving robot.

## Team workflow for three people

Three people and one robot means two of you are always blocked on hardware unless you split the work so that only one track needs the physical board at a time. The ownership split below is built around that constraint.

### Ownership

| Track | Owns | Needs the robot? |
| --- | --- | --- |
| Sensing | ToF array, IMU, I2C bring-up, calibration | Yes, heavily |
| Actuation | Motors, servo, power wiring, battery sense | Yes, heavily |
| Platform | Build system, logging, control logic, host tests, CI, CAD | Rarely |

Rotate the Platform role every two weeks so all three of you touch the hardware and all three can flash a board under pressure. The person who has never flashed the robot is the person who cannot help on competition day.

### Branching

Keep it simple. `main` always builds and always flashes to a robot that moves.

- Branch per component: `feat/tof-array`, `feat/motor-driver`, `fix/imu-drift`.
- Open a pull request even for small changes. The review is not about catching bugs, it is about making sure two people understand every subsystem.
- **No merge to `main` without a hardware pass logged in `docs/test-log.md`.** A green CI build proves it compiles, not that it works.
- Nobody commits directly to `main` after week two.

### Code review rules

The reviewer checks three things and ignores style, which the formatter handles:

1. Does any pin number appear outside `board_pins.h`?
2. Does anything in `control/` include a hardware header?
3. Is there a blocking call inside the control loop, including a `printf` that is not behind the ring buffer?

### Definition of done

A component is done when all five are true: it has a standalone hardware test, a logged pass, a reviewed pull request merged to `main`, its tunables in `config.h`, and a second team member who has run it.

### Meeting rhythm

Twenty minutes, twice a week, in front of the robot rather than a screen. Each person answers what they got working, what is blocking them, and what they need from the other two. Keep decisions in this document so that nobody has to search a chat history for the pin map.

### Shared environment

Agree on one compiler version and one Mbed CE revision in week one, and pin both in the repository. The single most common lost afternoon on three-person embedded projects is a build that works on two laptops and fails on the third.

## Mechanical work and CAD

Measure everything before you open FreeCAD. The chassis kit's hole pattern is the constraint every mount has to respect, and remeasuring after a failed print costs more than measuring carefully once.

### Measure first

- [ ] Chassis plate: hole pattern, spacing, plate thickness, overall footprint
- [ ] Wheel diameter and track width, plus ground clearance under the plate
- [ ] Nucleo F401RE: mounting hole positions, board outline, USB connector height and position
- [ ] L298N: hole spacing, heatsink height, terminal block clearance
- [ ] Battery holder footprint and how it will be removed for recharging
- [ ] VL53L0X breakout: board outline, hole position, where the sensor window sits relative to the edge
- [ ] MPU-6050 and LM2596 footprints

Put the numbers in `cad/measurements.md` as you take them. Three people designing to three different remembered values is a predictable disaster.

### Parts to design

| Part | Purpose | Notes |
| --- | --- | --- |
| Top deck | Mounts Nucleo, L298N, LM2596 | M3 holes, standoffs, cable pass-throughs |
| Front sensor mount | Holds front VL53L0X | Face square to travel, approx. 30-50 mm above floor |
| Side sensor mounts (x2) | Left and right VL53L0X | Perpendicular to travel, same height as front |
| Servo bracket | Optional pan head | Only if you commit to a scanning sensor |
| Battery retainer | Stops the pack shifting | A shifting pack changes your centre of mass mid-run |

### Sensor mounting rules

These three decide whether your wall-follower works, so get them right in CAD rather than with tape later.

1. **All three sensors at the same height**, high enough to clear floor texture and low enough to see the maze wall reliably. Check the wall height before committing.
2. **Square to their axis.** A sensor mounted a few degrees off reads a longer distance than the true perpendicular gap, and your controller will steer into the wall trying to correct a number that is lying to it.
3. **Nothing in the cone.** The VL53L0X has a wide field of view, so keep wires, screws, and the chassis edge out of the beam path or you will measure your own robot.

### Print plan

Print a test coupon of each mount's hole pattern first, a few grams of filament, and check the fit before printing the full part. Then print the real parts, assemble, and only then start integration testing. Design slots rather than round holes wherever a sensor angle might need adjusting, because it will.

## Schedule and today's meeting

**Open question: what is the actual deadline?** Nobody has written it down yet, so the phases below are gated on what must be true rather than on dates. Fill in the demo or competition date at your meeting today and work backwards, leaving the last two weeks for integration, because integration always takes longer than the estimate.

### Phases and gates

A phase ends when its gate is true, not when a week ends. Do not start the next phase early; the robot will punish you for it later.

| Phase | Gate to move on | Lead |
| --- | --- | --- |
| 0. Setup | All 3 laptops build and flash blink from the same repo | Platform |
| 1. Power and wiring | LM2596 set to 5 V, common ground, motors spin under battery | Actuation |
| 2. Sensing | 3 ToF addresses assigned, readings match a ruler, gyro drift measured | Sensing |
| 3. Mechanical | Mounts printed and fitted, sensors square and at equal height | Platform |
| 4. Control | Robot drives a straight corridor and stops before a wall | All |
| 5. Maze | Completes a practice maze 3 times in a row | All |
| 6. Tuning | Run time improved, failure modes documented | All |

If you run short on time, cut phase 6 and the servo. Never cut phase 0, which is the one that looks skippable and is not.

### Agenda for today

Two hours, in this order. The toolchain decision gates everything else, so make it before anyone writes code.

- [ ] **Solder the GY-521 headers (30 min).** Push the header pins into a breadboard, sit the board on top, and tack one corner pin first. Check the board is square before you solder the rest, because reflowing eight crooked pins is unpleasant.
- [ ] **Decide the toolchain (15 min).** Mbed CE with CMake, or STM32Cube HAL with CMake. Confirm first whether your course requires Mbed Studio specifically.
- [ ] **Create the repo (45 min).** One person creates it, adds `.gitignore` and `CMakePresets.json`, and pushes blink. The other two clone, build, and flash it. Nobody leaves until all three have flashed the board.
- [ ] **Assign tracks (10 min).** Sensing, Actuation, Platform. Write the names in this document.
- [ ] **Measure the chassis (20 min).** Hole pattern, plate dimensions, board footprints, into `cad/measurements.md`.
- [ ] **Set the deadline and two weekly meeting slots (5 min).**

### What usually goes wrong

These four account for most of the lost time on projects like this, and all four are cheap to avoid:

1. **Power problems diagnosed as software problems.** A brownout looks exactly like a firmware bug. Check the LM2596 output under load before you debug anything else.
2. **A pin changed without telling anyone.** This is why `board_pins.h` exists and why pin changes go through a pull request.
3. **Everything integrated on the last weekend.** Three subsystems that each work alone will not work together on the first try. Integrate in phase 4, not at the end.
4. **One person owns the whole build.** If only one of you can flash the robot, you have a single point of failure sitting in a seminar when something breaks.
