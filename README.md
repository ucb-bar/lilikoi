# RP2350 Bring-Up

This project targets the RP2350 on a Raspberry Pi Pico 2 integrated into the Fletcher Lake PCB. Firmware is built using the Pico SDK and programmed over USB using `picotool`.

## 1. Project Structure

Recommended directory layout:

```text
~/pico/
├── pico-sdk/
└── fletcherlake/
    ├── CMakeLists.txt
    ├── pico_sdk_import.cmake
    ├── main.c
    └── build/
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
| VCORE2 | `0x46` |
| VSENSE_PMIC | `0x48` |
| VSENSE_VCORE | `0x49` |

The unpopulated VIO device at `0x54` is intentionally not checked.

## 4. Build the Firmware

From the `fletcherlake` directory:

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
```

Flash `hello.uf2` for the original Hello World or `i2c_check.uf2` for the I2C
device check.

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
  [OK     ] VCORE2        at 0x46
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
