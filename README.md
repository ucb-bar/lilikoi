# Lilikoi
Fletcher Lake control and programming interface API

Lilikoi is the firmware backend of the Fletcher Lake remote bring-up system. It lets the
scheduler (`fletcherlake-scheduler`, fl-agent) power, program, run and measure a Fletcher
Lake board without anyone at the bench. It has two halves:

| Half | Where | Document |
|---|---|---|
| RP2350 firmware: resets, FPGA `DONE`, VCORE regulators, PMIC bus, SerialTL diagnostics | `fletcherlake/lilikoi.c`, `pmic.c` | [`fletcherlake/LILIKOI.md`](fletcherlake/LILIKOI.md) |
| Mac host tooling: `lilikoi` CLI + per-board daemon, `uart_tsi` (macOS port), OpenOCD cfg, example `cluster.yaml` slot | `host/` | [`host/README.md`](host/README.md) |

```
fl-agent (scheduler) ──argv──> lilikoi CLI ──> per-board daemon ──┬── USB ──> RP2350 `lilikoi` firmware ──> resets, PMIC, DONE
                                                                  ├── uart_tsi ──> FPGA UART-TSI ──> load + run ELF, console
                                                                  └── openocd  ──> JTAG ──> FPGA bitstream
```

Every chip-specific value (which chip is installed, its approved voltage range, which ports it
is on) lives in the scheduler's `cluster.yaml` and reaches lilikoi with each command. The
firmware and daemon store nothing, so swapping the chip is a configuration change.

This project targets the RP2350 on the Fletcher Lake PCB. Firmware is built using the Pico
SDK and programmed over USB using `picotool`. The production board uses the 48-GPIO
**RP2350B**, despite older bring-up commands referring to `pico2` (RP2350A). Use the included
`fletcherlake_rp2350b` board definition for any firmware that accesses GPIO32-39, which
includes `lilikoi` and `fletcherlake_diag`.

The rest of this file covers the Pico SDK setup shared by every image, then the legacy
standalone bring-up tools (`hello`, `i2c_check`, `fletcherlake_diag`, `vcore_sweep`). Those
remain useful for one-off bench diagnostics, but their consoles are interactive and are not
what the scheduler drives; `lilikoi` supersedes them.

## 1. Project Structure

```text
pico/
├── pico-sdk/                      separate SDK checkout
├── fletcherlake/                  RP2350 firmware
│   ├── CMakeLists.txt             targets: lilikoi, hello, i2c_check, vcore_sweep, fletcherlake_diag
│   ├── lilikoi.c, pmic.c, pmic.h  production image (see LILIKOI.md)
│   ├── serialtl_phy.c, *.pio      4-bit SerialTL transport (shared by lilikoi and fletcherlake_diag)
│   ├── boards/fletcherlake_rp2350b.h
│   └── build-*/                   one build dir per configuration
└── host/                          Mac-side tooling (see host/README.md)
    ├── lilikoi/                   Python package: cli, daemon, mcu link, tsi, openocd, fakes
    ├── uart_tsi/                  vendored testchipip uart_tsi with the macOS port
    ├── openocd/fletcherlake.cfg
    ├── examples/cluster-board.yaml
    └── tests/
```

The `pico-sdk` directory should be separate from the `fletcherlake` project.

## 2. Install the Pico SDK

Clone the SDK:

```bash
cd ~/pico
git clone https://github.com/raspberrypi/pico-sdk.git
cd pico-sdk
git submodule update --init
```

From the `fletcherlake` directory, point CMake to the SDK:

```bash
export PICO_SDK_PATH=/path/to/pico-sdk
```

## 3. I2C Device Check

The I2C checker is implemented separately in
`/pico/fletcherlake/i2c_check.c`; the original Hello World remains in
`/pico/fletcherlake/main.c`.

The firmware configures the schematic's PMIC bus on I2C0 at 100 kHz and repeats
the device check every five seconds:

```text
SDA_PMIC: GP0 (physical pin 1)
SCL_PMIC: GP1 (physical pin 2)
```

It checks the seven populated devices from the schematic:

| Device | 7-bit address |
| --- | --- |
| ISENSE0 | `0x10` |
| ISENSE1 | `0x1F` |
| VCORE0 | `0x45` |
| VCORE1 | `0x47` |
| VCORE2 | `0x40` |
| VSENSE_PMIC | `0x48` |
| VSENSE_VCORE | `0x49` |

The unpopulated VIO device at `0x54` is intentionally not checked.

## 4. Build the Firmware

For the production `lilikoi` image see
[`fletcherlake/LILIKOI.md`](fletcherlake/LILIKOI.md#build-and-flash); it needs
`-DPICO_BOARD=fletcherlake_rp2350b`. The legacy tools build as follows, from the
`fletcherlake` directory:

```bash
cmake -S . -B build -DPICO_BOARD=pico2
cmake --build build -j
```

This generates:

```text
build/hello.elf
build/hello.bin
build/hello.uf2
build/i2c_check.elf
build/i2c_check.bin
build/i2c_check.uf2
build/vcore_sweep.elf
build/vcore_sweep.bin
build/vcore_sweep.uf2
```

Flash `hello.uf2` for the original Hello World, `i2c_check.uf2` for the I2C
device check, or `vcore_sweep.uf2` for the read-only VCORE register tool.

## Legacy tool: Fletcher Lake FPGA and DSP24 diagnostics

> `lilikoi` includes the same reset, `DONE` and SerialTL functions behind its machine
> protocol (`reset`, `fpga done`, `sertl`). Use `fletcherlake_diag` only when you want the
> interactive console. The SerialTL pin table below applies to both images.

`fletcherlake_diag` controls the two reset-request nets and implements the
4-bit RP2350-to-FPGA SerialTL physical transport using two PIO state machines.
It is separate from, and does not alter, the FPGA's fixed 8-bit and 1-bit
legacy SerialTL connections to the physical DSP24 chip.

Build and flash it with the RP2350B board definition:

```bash
cmake -S . -B build-diag \
  -DPICO_BOARD=fletcherlake_rp2350b \
  -DPICO_SDK_PATH=/path/to/pico-sdk
cmake --build build-diag --target fletcherlake_diag -j
picotool load build-diag/fletcherlake_diag.uf2 -vx
```

### RP2350-to-FPGA SerialTL assignment

The FPGA is the 5 MHz clock source. Transfers use rising clock edges and the
standard decoupled `valid && ready` handshake. All FPGA pins are `LVCMOS33`.
The clock is an FPGA output, so the selected non-MRCC/SRCC bus pin is not being
used to clock FPGA fabric. Vivado is nevertheless given an explicit 5 MHz
clock constraint and `CLOCK_DEDICATED_ROUTE FALSE` for this forwarded port.

| Function | Board net | RP2350 GPIO | FPGA pin | Direction |
| --- | --- | ---: | --- | --- |
| Forwarded clock | `FPGA_MCU_D4` | 16 | W25 | FPGA → RP2350 |
| FPGA→MCU data 0 | `FPGA_MCU_D0` | 12 | T25 | FPGA → RP2350 |
| FPGA→MCU data 1 | `FPGA_MCU_D1` | 13 | U26 | FPGA → RP2350 |
| FPGA→MCU data 2 | `FPGA_MCU_D2` | 14 | U25 | FPGA → RP2350 |
| FPGA→MCU data 3 | `FPGA_MCU_D3` | 15 | V26 | FPGA → RP2350 |
| FPGA→MCU valid | `FPGA_MCU_D5` | 17 | W26 | FPGA → RP2350 |
| MCU ready | `FPGA_MCU_D6` | 18 | Y25 | RP2350 → FPGA |
| MCU→FPGA data 0 | `FPGA_MCU_D8` | 32 | V23 | RP2350 → FPGA |
| MCU→FPGA data 1 | `FPGA_MCU_D9` | 33 | T23 | RP2350 → FPGA |
| MCU→FPGA data 2 | `FPGA_MCU_D10` | 34 | W24 | RP2350 → FPGA |
| MCU→FPGA data 3 | `FPGA_MCU_D11` | 35 | V24 | RP2350 → FPGA |
| MCU→FPGA valid | `FPGA_MCU_D12` | 36 | U24 | RP2350 → FPGA |
| FPGA ready | `FPGA_MCU_D13` | 37 | T24 | FPGA → RP2350 |

`D7`, `D14`, and `D15` are intentionally unused. The PIO layer transfers raw
4-bit SerialTL phits and honors backpressure. USB diagnostics expose link
state and received phits; they intentionally do not inject arbitrary phits,
which could leave the FPGA TileLink deserializer in a partial transaction.

### Reset and status commands

Connect to USB serial and use:

```text
status
sertl status
sertl rx 16
sertl clear
reset fpga assert
reset fpga release
reset fpga pulse 10
reset chip assert
reset chip release
reset chip pulse 10
```

The safe startup state releases both resets and never automatically pulses the
DSP24 reset:

- FPGA reset: GPIO8 drives high to assert; input/high-impedance releases it.
- FPGA `DONE`: GPIO7 input.
- DSP24 reset request: GPIO9 drives low to simulate the button; input/high-
  impedance releases it.

Before testing DSP24 reset, set SW7 `RESET_DIR` for the installed chip:

- Active-low chip reset: close SW7 pins 2-3 (`RESET_DIR=0`).
- Active-high chip reset: leave SW7 pins 2-3 open (`RESET_DIR=1`).

Naichen Zhao tentatively recalls DSP24 reset as active-high; neither the chip
polarity nor the physical SW7 position has been verified. The firmware's GPIO9
behavior is independent of that selection.

## Legacy tool: VCORE voltage sweep

> `lilikoi` carries this regulator driver (`pmic.c`) without the compile-time limits or
> `arm WRITE` step: chip limits arrive with every `rail set` from the scheduler, and the
> firmware enforces only PCB bounds. Use `vcore_sweep` for a hand-driven sweep with a
> measurement prompt at each step.

`vcore_sweep` provides a USB-serial command interface for reading and changing
the three VCORE regulator setpoints. It supports the TPS6287B20 at `0x45` and
the TPS628681A devices at `0x47` and `0x40` shown in the Fletcher Lake
schematic.

The default build is deliberately read-only. To include voltage writes, obtain
the approved limits for the load attached to every rail. The simplest method is
to edit the clearly marked `USER CONFIGURATION` block near the top of
`vcore_sweep.c`: replace the six `0` limit placeholders with integer millivolt
values, then change `VCORE_SWEEP_WRITES_ENABLED` from `0` to `1`.

Build the edited source normally:

```bash
cmake -S . -B build-sweep -DPICO_BOARD=pico2
cmake --build build-sweep --target vcore_sweep -j
picotool load build-sweep/vcore_sweep.uf2 -vx
```

As an alternative to editing the source, all seven values can be supplied as
CMake options (the values below are placeholders, not recommendations):

```bash
cmake -S . -B build-sweep -DPICO_BOARD=pico2 \
  -DVCORE_SWEEP_WRITES_ENABLED=ON \
  -DVCORE0_SAFE_MIN_MV=<approved-min> \
  -DVCORE0_SAFE_MAX_MV=<approved-max> \
  -DVCORE1_SAFE_MIN_MV=<approved-min> \
  -DVCORE1_SAFE_MAX_MV=<approved-max> \
  -DVCORE2_SAFE_MIN_MV=<approved-min> \
  -DVCORE2_SAFE_MAX_MV=<approved-max>
cmake --build build-sweep -j
picotool load build-sweep/vcore_sweep.uf2 -vx
```

After connecting to the USB serial port, inspect the captured startup values,
then explicitly arm writes:

```text
status
arm WRITE
set vcore1 700
sweep vcore1 650 750 25 5000
restore all
disarm
```

Voltages are specified in integer millivolts. A sweep pauses at each setting
for the requested number of milliseconds, prints a measurement prompt, and
restores that rail's startup register value when it completes or aborts.
Requests outside the configured safety limits are rejected. The tool also
rejects voltages that are not exactly representable by the regulator's current
voltage range.

The firmware changes PMIC setpoints only. It does not drive `CORE_SEL1` or
`CORE_SEL2`, so verify the board's rail-routing switches before probing a
downstream VCORE test point.

## 5. Install `picotool`

On macOS:

```bash
brew install picotool
```

Verify the installation:

```bash
picotool version
```

## 6. Put the RP2350 in USB Boot Mode

When the RP2350 is in BOOTSEL/USB boot mode, macOS should mount a volume named:

```text
/Volumes/RP2350
```

Check with:

```bash
mount | grep RP2350
```

For example:

```text
/dev/disk5s1 on /Volumes/RP2350 (...)
```

You can also verify that `picotool` can communicate with the RP2350:

```bash
picotool info
```

A blank device may report:

```text
Program Information

 none
```

This is okay.

## 7. Verify the Firmware Image

Before flashing, optionally inspect the UF2:

```bash
picotool info -a build/hello.uf2
```

The important fields should include:

```text
target chip: RP2350
pico_board:  pico2
features:    USB stdin / stdout
```

## 8. Program and Boot the RP2350

Flash the firmware over USB:

```bash
picotool load build/hello.uf2 -vx
```

The expected output is approximately:

```text
Loading into Flash:   [==============================] 100%
Verifying Flash:      [==============================] 100%

OK

The device was rebooted to start the application.
```

The options are:

```text
-v    verify the flash contents after programming
-x    reboot and execute the application
```

After this command completes, the RP2350 leaves BOOTSEL mode and starts executing the firmware.

## 9. View USB Serial Output

Find the new USB serial device:

```bash
ls /dev/cu.usb*
```

The RP2350 will typically appear as something similar to:

```text
/dev/cu.usbmodem31101
```

Connect to it:

```bash
screen /dev/cu.usbmodem31101 115200
```

You should see output similar to:

```text
I2C device check (i2c0, SDA=GP0, SCL=GP1, 100000 Hz)
  [OK     ] ISENSE0       at 0x10
  [OK     ] ISENSE1       at 0x1f
  [OK     ] VCORE0        at 0x45
  [OK     ] VCORE1        at 0x47
  [OK     ] VCORE2        at 0x40
  [OK     ] VSENSE_PMIC   at 0x48
  [OK     ] VSENSE_VCORE  at 0x49
I2C result: 7/7 expected devices responded
```

To exit `screen`:

```text
Ctrl-A
Ctrl-\
```

then press `y`.

## Normal Development Flow

After the initial setup, the basic development loop is:

```bash
cmake --build build -j
picotool load build/hello.uf2 -vx
```

Then reconnect to the USB serial port:

```bash
ls /dev/cu.usb*
screen /dev/cu.usbmodemXXXXX 115200
```

Overall flow:

```text
main.c
   ↓
Pico SDK / CMake
   ↓
hello.uf2
   ↓
picotool over USB
   ↓
RP2350 external flash
   ↓
RP2350 reboots
   ↓
application runs
   ↓
USB serial output
```
