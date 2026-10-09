# Lilikoi firmware reference

`lilikoi` is the single RP2350 image that fl-agent's `LilikoiBoardBackend` talks to, through
the host tooling in [`../host`](../host/README.md). It replaces the four bring-up consoles
(`hello`, `i2c_check`, `fletcherlake_diag`, `vcore_sweep`) with one machine-readable protocol
over USB CDC. The firmware owns the parts of the board the Mac cannot reach directly: the two
reset nets, the FPGA `DONE` pin, the PMIC I2C bus, and the RP2350-to-FPGA SerialTL link.

Everything chip-specific arrives with each request. The firmware stores no configuration,
and the only limits it enforces on its own are the PCB bounds compiled into it.

Source: `lilikoi.c` (protocol, GPIO), `pmic.c`/`pmic.h` (regulators, I2C scan),
`serialtl_phy.c` (PIO SerialTL transport, shared with `fletcherlake_diag`).
A protocol-faithful fake lives at `../host/lilikoi/fakes/fake_mcu.py`.

## Build and flash

```sh
cd fletcherlake
cmake -S . -B build-lilikoi -DPICO_BOARD=fletcherlake_rp2350b -DPICO_SDK_PATH=../pico-sdk
cmake --build build-lilikoi --target lilikoi -j
picotool load build-lilikoi/lilikoi.uf2 -vx
```

The `fletcherlake_rp2350b` board definition is required: the production board is the 48-GPIO
RP2350B and the SerialTL pins include GPIO32-37. It defines no default UART, because GP0/GP1
carry the PMIC I2C bus; all console traffic is USB.

PCB voltage bounds can be overridden at configure time once the real ratings are known:

```sh
cmake -S . -B build-lilikoi -DPICO_BOARD=fletcherlake_rp2350b \
  -DLILIKOI_PCB_RAIL_MIN_MV=<mv> -DLILIKOI_PCB_RAIL_MAX_MV=<mv>
```

Defaults are 400 and 1200 mV, tagged `STANDIN(pcb-bounds)` in `pmic.h`. 400 mV is the
regulators' floor, not a measured board rating.

## Boot behaviour

1. USB stdio starts. The USB serial number is the flash unique ID, so the host can find the
   board by serial rather than by `/dev/cu.usbmodem*` name.
2. GPIO7 (`FPGA_DONE`) is an input. GPIO8 (FPGA reset) and GPIO9 (chip reset request) are
   released to high impedance. The chip reset is never pulsed at boot.
3. I2C0 is initialised at 100 kHz and the three VCORE setpoints are snapshotted
   (see [Snapshot and restore](#snapshot-and-restore)).
4. The SerialTL PIO transport is initialised; failure is reported as `sertl=failed` in `info`
   but does not stop the firmware.
5. One banner line is printed. Nothing else is ever sent unsolicited.

```
* lilikoi 0.1.0 proto=1 uid=E6614C311B7A2B32 ready
```

## Wire protocol

One request per line, no echo, no prompt. Lines end in `\n`; `\r` is ignored.

```
request    <token> <command> [arg ...]\n
reply      <token> OK [key=value ...]\n
           <token> ERR <CODE> <message>\n
async      * <text>\n                     (banner only)
```

- The **token** is chosen by the host, must match `[A-Za-z0-9_-]+`, and is echoed on the reply.
  The host uses it to discard stale replies after a timeout. A malformed token or an
  overlong line is answered with token `?`.
- Limits: 159 characters per line, 8 arguments per command. Fields are split on spaces and tabs.
- `OK` carries zero or more `key=value` pairs. Values never contain spaces. Numbers are
  decimal unless the key ends in `code` or `status`, which are `0x..` hex.
- `ERR` codes are single uppercase words; the message is free text to the end of the line.
- Commands are processed one at a time in order. There is no queue depth beyond the USB
  buffer, so the host waits for each reply before sending the next request.

Protocol version is `proto=1` in `info`. Additive changes (new keys, new commands) keep the
version; a change to an existing key or command bumps it.

## Commands

| Command | Reply keys | Notes |
|---|---|---|
| `ping` | none | Liveness check. |
| `info` | `fw proto board uid sertl rails pcb_min_mv pcb_max_mv` | `sertl` is `ready` or `failed`. |
| `scan` | `isense0 isense1 vcore0 vcore1 vcore2 vsense_pmic vsense_vcore found expected` | Each device key is `1` if it ACKed. Presence only; no registers are read. |
| `rail count` | `count` | Always `3`. |
| `rail snapshot` | `snapshotted expected` | Re-reads every rail's setpoint as the new restore baseline. |
| `rail get <n>` | rail state (below) | Reads VSET and STATUS. |
| `rail set <n> <mv> <min_mv> <max_mv>` | rail state | Writes a setpoint; see [Voltage writes](#voltage-writes). |
| `rail restore <n>` | rail state | Writes the snapshotted setpoint back. |
| `rail restore all` | `restored expected` | `ERR I2C` with the same keys if any rail failed. |
| `fpga done` | `done` | Level on GPIO7. |
| `reset fpga\|chip assert` | `fpga_reset chip_reset done` | Drives the net. |
| `reset fpga\|chip release` | same | Back to high impedance. |
| `reset fpga\|chip pulse [ms]` | same | Assert, hold 1..10000 ms (default 10), release. Blocks for the duration. |
| `sertl status` | `ready clk out_valid out_ready in_valid in_ready rx_phits tx_phits` | Pin levels and PIO counters. |
| `sertl clear` | `cleared` | Zeroes the counters. |
| `sertl rx [n]` | `phits count` | Drains up to `n` (1..32, default 16) received 4-bit phits as hex nibbles. Never transmits. |
| `sense …` | — | Always `ERR UNSUPPORTED`. `STANDIN(sense-parts)`. |
| `power …` | — | Always `ERR UNSUPPORTED`. `STANDIN(power-enable)`. |

Rail state keys: `rail name mv code step_uv status snapshot_mv`. `name` is `vcore0..2`,
`mv` is the setpoint decoded from `code`, `step_uv` is the regulator's current resolution,
`status` is the raw status register, and `snapshot_mv` is the restore baseline (`0` if no
snapshot exists). On an `ERR` from `rail set` other than `FAULT`, the state keys are zero.

Rail numbers match the scheduler's integer `rail` argument: `set_voltage(1, 0.75)` becomes
`rail set 1 750 <min> <max>`.

## Error codes

| Code | Origin | Meaning |
|---|---|---|
| `BAD_REQUEST` | parser | Malformed token, missing command, bad argument, line too long. The message gives the usage. |
| `UNKNOWN` | parser | Unrecognised command word. |
| `UNSUPPORTED` | `sense`, `power` | Hardware path not implemented yet. |
| `BAD_RAIL` | `rail` | Rail index outside `0..2`. |
| `NO_SNAPSHOT` | `rail set/restore` | No baseline for this rail (its I2C read failed at boot and `rail snapshot` has not succeeded since). |
| `PCB_BOUND` | `rail set` | The host's `min_mv..max_mv` is inverted or extends outside the compiled PCB bounds. |
| `RANGE` | `rail set` | `mv` is outside `min_mv..max_mv`. |
| `UNREPRESENTABLE` | `rail set` | `mv` is not exactly `400 mV + code × step` for a code in `0..255`. |
| `I2C` | any PMIC access | A transfer did not ACK or timed out (10 ms). |
| `READBACK` | `rail set/restore` | The register read back differently from what was written. |
| `FAULT` | `rail set` | The regulator's status showed a severe flag 20 ms after the write. |

## Voltage writes

`rail set` is the only command that changes board state beyond the resets. Its sequence:

1. Reject if the rail has no snapshot.
2. Reject with `PCB_BOUND` unless `LILIKOI_PCB_RAIL_MIN_MV ≤ min_mv ≤ max_mv ≤ LILIKOI_PCB_RAIL_MAX_MV`.
   This is the whole of the firmware's chip-independence: the host's limits must fit the
   board, but the firmware does not know or care which chip they describe.
3. Reject with `RANGE` unless `min_mv ≤ mv ≤ max_mv`.
4. Convert to a register code; reject with `UNREPRESENTABLE` if it does not land on a step.
5. Read STATUS once to clear latched flags.
6. Write the setpoint register, read it back. Mismatch restores the snapshot and returns `READBACK`.
7. Wait 20 ms, read STATUS. A severe flag restores the snapshot and returns `FAULT`.

Invariant for the host: after any `ERR` from `rail set` that follows the write, the rail is
back at its snapshot value. Only `OK` leaves the new setpoint in place.

### Regulators

| Rail | Address | Part | Setpoint reg | Status reg | Step | Severe mask |
|---|---|---|---|---|---|---|
| `vcore0` | `0x45` | TPS6287B | `0x00` VSET | `0x04` | 1.25, 2.5 or 5 mV by CONTROL2 bits [3:2] (read at snapshot) | `0x3c` |
| `vcore1` | `0x47` | TPS628681A | `0x01` VOUT1 | `0x05` | 5 mV | `0x19` |
| `vcore2` | `0x40` | TPS628681A | `0x01` VOUT1 | `0x05` | 5 mV | `0x19` |

Setpoint in mV is `400 + code × step`. The firmware changes setpoints only; it does not touch
`CORE_SEL1`/`CORE_SEL2`, so the rail routing switches decide which test point a change reaches.

### Snapshot and restore

A snapshot is the setpoint register value recorded per rail, taken at boot and again on every
`rail snapshot`. `rail restore` writes it back. The host's `power_on` issues `rail snapshot`,
so a later `power_off` restores whatever the rails held when the job's power-on ran, not
necessarily the factory default. A set followed by `rail snapshot` makes that set the new
baseline; the host never does this inside a job.

## GPIO map

| GPIO | Function | Direction and polarity |
|---|---|---|
| 0, 1 | I2C0 SDA/SCL (`SDA_PMIC`/`SCL_PMIC`), 100 kHz | — |
| 7 | `FPGA_DONE` | input |
| 8 | FPGA reset | drive **high** to assert, high-Z to release |
| 9 | Chip reset request | drive **low** to press the reset button, high-Z to release |
| 12-18, 32-37 | 4-bit SerialTL to the FPGA | see the pin table in [`../README.md`](../README.md#rp2350-to-fpga-serialtl-assignment) |

GPIO9 only simulates the reset button. Which polarity reaches the chip is set in hardware by
SW7 `RESET_DIR`, so the firmware has no polarity parameter and does not need to know which
chip is installed.

## Other devices on the PMIC bus

`scan` probes these addresses. Only the three regulators are driven.

| Key | Address | Role |
|---|---|---|
| `isense0`, `isense1` | `0x10`, `0x1f` | current monitors, part unknown (`STANDIN(sense-parts)`) |
| `vsense_pmic`, `vsense_vcore` | `0x48`, `0x49` | voltage monitors, part unknown (`STANDIN(sense-parts)`) |

Until the parts and register maps are known, `sense` returns `UNSUPPORTED` so the scheduler
sees an explicit failure for `read_voltage`/`read_current` instead of invented numbers.

## Talking to it by hand

Through the host CLI, which finds the port by serial number and handles tokens:

```sh
lilikoi --mcu E6614C311B7A2B32 mcu info
lilikoi --mcu E6614C311B7A2B32 mcu rail get 1
lilikoi --mcu E6614C311B7A2B32 mcu reset fpga pulse 50
```

Or directly with a terminal (any token works; `115200` is ignored by USB CDC):

```
$ screen /dev/cu.usbmodem1101 115200
a info
a OK fw=0.1.0 proto=1 board=fletcherlake_rp2350b uid=E6614C311B7A2B32 sertl=ready rails=3 pcb_min_mv=400 pcb_max_mv=1200
b rail set 1 750 650 900
b OK rail=1 name=vcore1 mv=750 code=0x46 step_uv=5000 status=0x00 snapshot_mv=800
c rail set 1 950 650 900
c ERR RANGE rail=0 name=vcore1 mv=0 code=0x00 step_uv=0 status=0x00 snapshot_mv=0
d rail restore 1
d OK rail=1 name=vcore1 mv=800 code=0x50 step_uv=5000 status=0x00 snapshot_mv=800
```

## What is not here yet

| Tag | Blocks | Needed |
|---|---|---|
| `STANDIN(power-enable)` | real power sequencing; `power_on`/`power_off` are composed from resets and rail restores | rail-enable GPIOs or PMIC enable registers from the schematic |
| `STANDIN(sense-parts)` | `read_voltage`, `read_current` | part numbers and register maps at `0x10/0x1f/0x48/0x49` |
| `STANDIN(pcb-bounds)` | nothing functionally, but the default bounds are a guess | VCORE trace and load ratings |

`set_frequency` has no firmware component at all: the clock path (FPGA PLL or an external
generator) is the host daemon's `STANDIN(clock-source)`.
