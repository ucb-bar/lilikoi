# Lilikoi host tooling

The Mac-side half of Lilikoi. fl-agent's `LilikoiBoardBackend` runs one command per hardware
operation; this package turns each of those into the right mix of three tools:

| Tool | Role |
|---|---|
| `lilikoi` firmware on the RP2350 ([`../fletcherlake/LILIKOI.md`](../fletcherlake/LILIKOI.md)) | resets, FPGA `DONE`, VCORE regulators, I2C presence, SerialTL diagnostics |
| `uart_tsi` (`uart_tsi/`, ported to macOS) | loads the ELF over the FPGA's UART-TSI port, starts hart 0, proxies the console, exits with the program's `tohost` code |
| OpenOCD + `openocd/fletcherlake.cfg` | programs the FPGA from a bitstream built elsewhere |

```
fl-agent ──argv──> lilikoi CLI ──unix socket──> per-board daemon ──┬── USB CDC ──> RP2350 firmware
          (one process per op)                  (spawned on first   ├── spawns uart_tsi ──> UART-TSI tty
                                                 use, holds ports)  └── spawns openocd  ──> JTAG
```

The daemon exists because `BoardBackend` separates `program_soc`, `start`, `read_uart` and
`wait_for_completion`, while `uart_tsi` does all four in one process. The daemon owns that
child (and the RP2350 serial port) across CLI calls; the CLI holds no state. Configuration is
never stored on the Mac or the MCU: every call carries it from `cluster.yaml` via
`device_mapping` placeholders (see [Connecting to the scheduler](#connecting-to-the-scheduler)).

Contents: [Operation map](#operation-map) · [How a job flows](#how-a-job-flows) ·
[The daemon](#the-daemon) · [CLI reference](#cli-reference) · [Error codes](#error-codes) ·
[Connecting to the scheduler](#connecting-to-the-scheduler) · [Install](#install-on-a-mac-mini) ·
[Bench use](#bench-use) · [Tests](#tests) · [Standins](#standins) ·
[Scheduler-side notes](#scheduler-side-notes) · [Troubleshooting](#troubleshooting)

## Operation map

| `BoardBackend` op | What happens |
|---|---|
| `discover` | firmware `info`/`scan`/`fpga done` + tool versions → JSON string map |
| `power_on` | release both resets, snapshot rails, require all three VCORE regulators to ACK |
| `power_off` | stop any run, restore rail setpoints, pulse chip then FPGA reset, forget the staged ELF |
| `set_voltage rail volts` | `rail set` with the slot's `--rail-limits`; firmware also enforces PCB bounds |
| `read_voltage`, `read_current` | **unsupported** until sense-part register maps are known |
| `set_frequency` | **unsupported** until a clock path exists |
| `program_fpga bitstream` | `openocd -f cfg [-c "adapter serial …"] -c "init; pld load 0 {bit}; shutdown"`, then wait for `DONE` on GPIO7, then release chip reset |
| `program_soc elf` | validates and stages the ELF (uart_tsi loads at `start`) |
| `start` | spawns `uart_tsi +tty=… +baudrate=921600 +selfcheck elf` |
| `stop` | kills uart_tsi, pulses chip reset (idempotent) |
| `read_uart` | drains buffered uart_tsi stdout as raw bytes |
| `wait_for_completion` | waits for uart_tsi; `{"passed": rc==0, "exit_code": rc, "detail": …}` |

## How a job flows

fl-agent's executor calls the ops in this order for a plain run. Each line is one CLI
process; the daemon does the work.

| Step | fl-agent calls | Daemon does | Hardware state afterwards |
|---|---|---|---|
| 0 | `discover` (at agent start and on health checks) | opens the RP2350 port by serial, `info`, `scan`, `fpga done`, probes `openocd --version` and `uart_tsi` on PATH | unchanged |
| 1 | `power_on` | `ping`, `reset fpga release`, `reset chip release`, `rail snapshot`, `scan`; fails `PMIC_MISSING` unless `vcore0..2` all ACK | resets released, rail baseline recorded |
| 2 | `program_fpga <bit>` (skipped if the agent's cache says the bitstream is already loaded and `force_reflash` is false) | kills any run, runs OpenOCD (default 180 s budget), polls `fpga done` until `1` (default 5 s) or `FPGA_NOT_DONE`, then `reset chip release` | FPGA configured, `DONE` high |
| 3 | `set_voltage r v` (per shmoo point, if any) | `rail set r round(v×1000) min max` | setpoint changed, or unchanged on any error |
| 4 | `program_soc <elf>` | checks the file exists and starts with `\x7fELF`, checks `--tsi-tty` and the `uart_tsi` binary are present, remembers the path | unchanged |
| 5 | `start` | spawns `uart_tsi` in its own session; stdout+stderr are pumped into a buffer and `<mcu>.uart_tsi.log` | ELF loading over UART-TSI, then hart 0 running |
| 6 | `read_uart` (polled every 20 ms) | returns and clears the buffer, base64 over the socket, raw bytes on stdout | — |
| 7 | `wait_for_completion` | blocks until `uart_tsi` exits; its exit status is the program's `tohost` code | program finished |
| 8 | `stop` | SIGTERM then SIGKILL the `uart_tsi` process group, `reset chip pulse 50` | chip reset, FPGA still configured |
| 9 | `power_off` | `stop` + `rail restore all` + `reset chip pulse 50` + `reset fpga pulse 50`; forgets the staged ELF | rails back at the step-1 baseline, both resets pulsed |

A shmoo repeats steps 3-8 per point. `program_fpga` deliberately does not run `power_on`'s
checks and `power_on` deliberately does not touch the FPGA, so the agent can keep a programmed
bitstream across jobs.

## The daemon

`lilikoi.daemon` is one process per RP2350, identified by `--mcu`. It is an asyncio Unix
socket server; each connection is one JSON request line and one JSON reply line.

```json
{"op": "set_voltage", "args": {"rail": 1, "volts": 0.75},
 "settings": {"rail_limits": "650-900,650-900,650-900", "tsi_tty": "/dev/cu.usbserial-X"}}
{"ok": true, "result": {"rail": "1", "name": "vcore1", "mv": "750", "...": "..."}}
{"ok": false, "code": "RANGE", "message": "rail=0 name=vcore1 ...", "details": {"command": "rail"}}
```

### Lifecycle

- **Spawn.** The CLI tries to connect to the board's socket. If that fails it starts
  `python -m lilikoi.daemon --mcu … --run-dir …` fully detached (`start_new_session`, stdio to
  the daemon log) and retries the connection for up to 8 s, then fails `DAEMON_UNAVAILABLE`.
- **Single instance.** The daemon takes a non-blocking `flock` on `<mcu>.lock` before serving.
  A loser exits 0 immediately, so concurrent first calls from fl-agent produce exactly one
  daemon. The socket is created mode 0600 and an old socket file is replaced.
- **Surviving fl-agent timeouts.** When a CLI call exceeds `firmware_timeout_seconds`, fl-agent
  kills the CLI's process group. The daemon is in a different session, so it, the serial port
  and a running `uart_tsi` all survive. The next `stop` or `power_off` cleans up.
- **Shutdown.** `lilikoi shutdown` stops any run, closes the serial port, removes the socket
  and exits. Nothing restarts it except the next CLI call. There is no idle timeout.
- **Crash.** An unhandled exception inside an op is returned as `INTERNAL` and logged with a
  traceback; the daemon keeps serving. If the daemon process dies, the next CLI call spawns a
  fresh one, which has no staged ELF and no run.

### State

The daemon holds exactly three pieces of state, all lost on shutdown:

| State | Set by | Cleared by |
|---|---|---|
| open serial link to the RP2350 | first op that needs the MCU | `MCU_IO`/`MCU_TIMEOUT` (reopened lazily next call, so a re-enumerated board recovers), `shutdown` |
| staged ELF path | `program_soc` | `power_off`, daemon restart |
| current `uart_tsi` run (process, output buffer, exit code) | `start` | `stop`, `power_off`, `program_fpga`, `shutdown`; a finished run stays readable until then |

`start` refuses with `NO_PROGRAM` if nothing is staged and `BUSY` if a run is still alive.
`wait_for_completion` refuses with `NO_RUN` if there is no run; after `stop` that is the
expected answer. `read_uart` with no run returns zero bytes.

### Concurrency

Every op except `read_uart` holds a per-board lock, so hardware actions serialise even when
fl-agent's `read_uart` polling overlaps a `wait_for_completion` that is blocking. Serial
requests to the MCU additionally hold a link lock and run in a worker thread; each request
has a 2 s reply timeout (12 s for the `mcu` passthrough) and discards replies whose token does
not match.

### Files

Under `--run-dir` (default `~/.lilikoi/run`, env `LILIKOI_RUN_DIR`), per MCU:

| File | Purpose |
|---|---|
| `<mcu>.sock` | the daemon's Unix socket. If the full path would exceed macOS's 104-byte AF_UNIX limit it moves to `$TMPDIR/lilikoi-<hash>.sock`; everything else stays put |
| `<mcu>.lock` | flock single-instance lock |
| `<mcu>.log` | daemon log: spawn, `serving`, every op failure with traceback |
| `<mcu>.uart_tsi.log` | everything `uart_tsi` printed, appended across runs; `wait_for_completion` names it in `detail` |

`<mcu>` is the `--mcu` value with anything outside `[A-Za-z0-9_.-]` replaced by `_`.

## CLI reference

```
lilikoi [global flags] <subcommand> [args]
```

Global flags are the per-call settings. fl-agent supplies them from `device_mapping`
placeholders; on the bench you type them. Flags not given fall back to the defaults shown.

| Flag | Default | Used by |
|---|---|---|
| `--mcu ID` (env `LILIKOI_MCU`) | required | all; RP2350 USB serial number or `/dev/cu.*` path |
| `--run-dir DIR` (env `LILIKOI_RUN_DIR`) | `~/.lilikoi/run` | all; selects the daemon instance |
| `--tsi-tty DEV` | none | `discover` (reported), `program-soc`, `start` |
| `--tsi-baud N` | `921600` | `start`; must match the bitstream's UART-TSI baud |
| `--uart-tsi PATH` | `uart_tsi` on PATH | `discover`, `program-soc`, `start` |
| `--no-selfcheck` | selfcheck on | `start`; drops `+selfcheck` (readback after load) |
| `--openocd PATH` | `openocd` on PATH | `discover`, `program-fpga` |
| `--openocd-cfg PATH` | none | `program-fpga` (required); resolved to an absolute path |
| `--jtag-serial ID` | none | `program-fpga`; adds `adapter serial ID` for Macs with several cables |
| `--openocd-timeout S` | `180` | `program-fpga` |
| `--fpga-done-timeout S` | `5` | `program-fpga`; wait for `DONE` after OpenOCD exits |
| `--rail-limits SPEC` | none | `set-voltage` (required); `MIN-MAX[,MIN-MAX…]` in mV, `-` for a rail that may not be changed |
| `--mcu-baud N` | `115200` | ignored by USB CDC; present for completeness |

Subcommands and what they print on success:

| Subcommand | stdout |
|---|---|
| `discover` | one JSON object, string values only |
| `power-on`, `power-off`, `set-voltage RAIL VOLTS`, `set-frequency HZ`, `program-fpga BIT`, `program-soc ELF`, `start`, `stop` | nothing |
| `read-voltage CH`, `read-current CH` | one number (volts or amps) |
| `read-uart` | raw bytes, may be empty |
| `wait-for-completion` | `{"passed": bool, "exit_code": int, "detail": str}` |
| `mcu WORD…` | the firmware's reply as `key=value` pairs; e.g. `lilikoi --mcu X mcu rail get 0` |
| `shutdown` | nothing; stops this board's daemon |

Exit status is 0 on success and 1 on any error, with `CODE: message [details-json]` on stderr.
That is exactly the contract `fl_agent.hardware.lilikoi.LilikoiBoardBackend` parses.

`discover` output keys: `backend` (`lilikoi`), `mcu_port`, the firmware `info` keys
(`fw proto board uid sertl rails pcb_min_mv pcb_max_mv`), `i2c_<device>` presence flags and
`i2c_found`/`i2c_expected`, `fpga_done`, `openocd` (version line or `missing`), `uart_tsi`
(`found` or `missing`), `tsi_tty`.

## Error codes

Host-side codes. Firmware codes (`RANGE`, `UNREPRESENTABLE`, `PCB_BOUND`, `FAULT`,
`READBACK`, `I2C`, `NO_SNAPSHOT`, `BAD_RAIL`, `UNSUPPORTED`, `BAD_REQUEST`) pass through
unchanged with `{"command": …}` in details; see the
[firmware reference](../fletcherlake/LILIKOI.md#error-codes).

| Code | Raised by | Meaning |
|---|---|---|
| `DAEMON_UNAVAILABLE` | CLI | daemon did not come up within 8 s; read `<mcu>.log` |
| `DAEMON_CLOSED` | CLI | daemon dropped the connection without a reply |
| `UNKNOWN_OPERATION`, `INTERNAL` | daemon | bad request, or an unexpected exception (traceback in the log) |
| `MCU_NOT_FOUND`, `MCU_AMBIGUOUS` | port lookup | no, or more than one, USB serial device with that serial number |
| `MCU_OPEN_FAILED`, `MCU_IO`, `MCU_TIMEOUT`, `MCU_CLOSED` | serial link | open failed, read/write error, no matching reply in time. The link is reopened on the next call |
| `PMIC_MISSING` | `power_on` | one or more of `vcore0..2` did not ACK; `details.scan` has the full presence map |
| `POWER_OFF_INCOMPLETE` | `power_off` | resets were pulsed but `rail restore all` failed; message says why |
| `NO_RAIL_LIMITS`, `BAD_RAIL_LIMITS` | `set_voltage` | the slot gave no (or malformed) limits for that rail. Limits are mandatory |
| `UNSUPPORTED` | `set_frequency`, and the firmware for `sense` | no hardware path yet (standins) |
| `NO_BITSTREAM`, `BAD_PATH`, `NO_OPENOCD_CFG` | `program_fpga` | missing file, braces/newlines in the path, or no `--openocd-cfg` |
| `OPENOCD_START_FAILED`, `OPENOCD_TIMED_OUT`, `OPENOCD_FAILED` | `program_fpga` | exec failed, exceeded `--openocd-timeout`, or non-zero exit (last 4000 chars of output in details) |
| `FPGA_NOT_DONE` | `program_fpga` | OpenOCD succeeded but GPIO7 never went high |
| `NO_ELF`, `NOT_ELF`, `NO_TSI_TTY`, `NO_UART_TSI` | `program_soc` | file missing, no ELF magic, no `--tsi-tty`, `uart_tsi` not found |
| `NO_PROGRAM`, `BUSY` | `start` | nothing staged, or a run is still alive |
| `NO_RUN` | `wait_for_completion` | no run exists (normal after `stop`) |

## Connecting to the scheduler

Nothing in `fletcherlake-scheduler` changes. A board slot with `backend: lilikoi` makes
fl-agent construct `LilikoiBoardBackend(firmware_commands, firmware_timeout_seconds,
device_mapping)` (see `fl_agent/service.py`), and every op becomes one argv from
`firmware_commands` with `{placeholders}` filled from `device_mapping` plus the op's own
`{rail} {volts} {channel} {hz} {bitstream} {elf}`. `examples/cluster-board.yaml` is a complete
slot written against that contract; `tests/test_scheduler_contract.py` drives the real backend
class through it.

1. **Install the tooling** ([below](#install-on-a-mac-mini)) and flash the firmware.
2. **Put `lilikoi` where the agent can exec it.** Under launchd the agent's PATH is only
   `/opt/homebrew/bin` and `/usr/local/bin`. Either install the package into one of those
   prefixes or make the first element of every `firmware_commands` entry an absolute path.
3. **Fill the slot.** Copy the board from `examples/cluster-board.yaml` into the cluster
   config and replace the `STANDIN` values: `mcu` (from `lilikoi discover` or `picotool info`),
   `tsi_tty`, `jtag_serial` (drop the `--jtag-serial` flag entirely if there is one cable),
   absolute `openocd_cfg` and `uart_tsi` paths, `run_dir`, and `rail_limits` approved for the
   installed chip. Set `socs[].name` and `fpgas[].model` to whatever job files will request in
   `resource_constraints`.
4. **Bench run without launchd.** The scheduler's `examples/init_simulation.py` refuses
   non-mock boards, so write and confirm the config directly, then run the agent:

   ```sh
   cd fletcherlake-scheduler
   pixi run python - <<'PY'
   import yaml
   from pathlib import Path
   from fl_common.models import ClusterConfig
   from fl_agent.configuration import write_config, confirm_config
   root = Path("/tmp/fl-bench")
   cfg = ClusterConfig.model_validate(yaml.safe_load(Path("/path/to/my-cluster.yaml").read_text()))
   write_config(root, cfg); confirm_config(root)
   PY
   pixi run fl-agent --state-root /tmp/fl-bench --runtime-root /tmp/fl-bench/run
   ```

   In another shell: `export FL_AGENT_SOCKET=/tmp/fl-bench/run/agent.sock`, then
   `pixi run fl cluster status` should show the board, and `pixi run fl job submit job.yaml`
   runs one. The agent calls `discover` first, so a wrong `mcu` or a missing PMIC shows up
   before any job.
5. **Production.** Follow `docs/macos-operations.md`: `fl cluster setup init overrides.yaml
   --scheduler …`, then `fl cluster setup confirm /opt/fl`. The overrides file carries the
   same slot. Swapping chips later is `setup reconfigure --config`, which drains the agent,
   validates the new file and restarts it.

Keep these consistent between the two sides:

- `firmware_timeout_seconds` on the slot must exceed the job's `run_timeout`, because
  `wait_for_completion` is one blocking CLI call.
- Under launchd the agent runs as root. `run_dir`, the Python environment providing `lilikoi`,
  and the `uart_tsi` binary must all be readable by root; the example uses
  `/Library/Application Support/fl/lilikoi` for that reason.
- One agent per RP2350. The daemon is single-instance per MCU, but two agents pointing at the
  same serial would fight over the port.
- Only ops with a hardware path work today. Voltage shmoo runs; frequency shmoo and sense
  readback fail at that step until their standins are filled.

## Install on a Mac Mini

```sh
brew install open-ocd riscv-isa-sim        # openocd 0.12, libfesvr
cd host/uart_tsi && make RISCV=/usr/local   # builds ./uart_tsi
cd .. && python3 -m pip install .           # provides the `lilikoi` command (needs pyserial)
```

`RISCV` must point at the prefix that holds `include/fesvr` and `lib/libfesvr.a`. If that
library was built for a different CPU than the Mac (an Intel-prefix Homebrew under Rosetta
installs an x86_64 `libfesvr.a` under `/usr/local`), the Makefile adds the matching `-arch`
flag and the resulting `uart_tsi` runs under Rosetta. A native arm64 `riscv-isa-sim` under
`/opt/homebrew` avoids that; pass `RISCV=/opt/homebrew` in that case.

Flash the firmware with the RP2350B board definition:

```sh
cd ../fletcherlake
cmake -S . -B build-lilikoi -DPICO_BOARD=fletcherlake_rp2350b -DPICO_SDK_PATH=../pico-sdk
cmake --build build-lilikoi --target lilikoi -j
picotool load build-lilikoi/lilikoi.uf2 -vx
```

`--mcu` takes the RP2350's USB serial number (its flash unique ID, shown by `picotool info`
and by `lilikoi discover`), so `/dev/cu.usbmodem*` renumbering is harmless.

### OpenOCD cfg

`openocd/fletcherlake.cfg` is static and must not call `init` or `pld load`; the daemon appends
those on the command line so the trusted cfg is never rewritten per job. It declares the
adapter (`STANDIN(jtag-adapter)`, currently a Digilent HS3 guess), the Kintex-7 XC7K160T tap
(IDCODE `0x0364c093`) and `pld device virtex2 xc7.tap 1`. To adopt the hardware team's cfg,
copy its interface/adapter lines over the standin block and drop any trailing
`init`/`pld load` lines.

## Bench use

```sh
lilikoi --mcu E6614C311B7A2B32 discover
lilikoi --mcu E6614C311B7A2B32 mcu rail get 0
lilikoi --mcu E6614C311B7A2B32 --rail-limits 650-900,650-900,650-900 set-voltage 1 0.75
lilikoi --mcu E6614C311B7A2B32 --openocd-cfg openocd/fletcherlake.cfg program-fpga top.bit
lilikoi --mcu E6614C311B7A2B32 --tsi-tty /dev/cu.usbserial-X --uart-tsi uart_tsi/uart_tsi program-soc hello.riscv
lilikoi --mcu E6614C311B7A2B32 --tsi-tty /dev/cu.usbserial-X --uart-tsi uart_tsi/uart_tsi start
lilikoi --mcu E6614C311B7A2B32 wait-for-completion
lilikoi --mcu E6614C311B7A2B32 shutdown          # stop that board's daemon
```

`uart_tsi` can also be run by hand to check the FPGA link without the daemon:

```sh
uart_tsi/uart_tsi +tty=/dev/cu.usbserial-X +baudrate=921600 +selfcheck hello.riscv
```

## Tests

```sh
python3 -m venv .venv && .venv/bin/pip install -e '.[test]'
.venv/bin/pytest
```

Tests run the real CLI → daemon → pyserial path against `lilikoi/fakes/fake_mcu.py`, a
protocol-faithful RP2350 on a pty, with fake `uart_tsi` and `openocd` executables
(`fake_uart_tsi.py` maps ELF names to behaviour: `fail` exits 1, `hang` never exits,
`exitN` exits N; `fake_openocd.py` raises the fake `DONE` pin). The scheduler-contract test
runs only when `fl_agent` is importable. To run it, install the scheduler into the same venv:

```sh
.venv/bin/pip install -e ../../fletcherlake-scheduler pytest-asyncio
```

On Python 3.14/macOS, pip's editable `.pth` may be written with the `hidden` file flag,
which `site` then skips; `chflags nohidden .venv/lib/python3.14/site-packages/*.pth` fixes it.

## Standins

`grep -rn 'STANDIN(' ..` lists every site. By topic:

| Tag | Where | What it blocks | What fills it |
|---|---|---|---|
| `power-enable` | `lilikoi.c`, `daemon.py` | real power sequencing: `power_on` only releases resets and checks the PMIC ACKs; `power_off` leaves rails energised | rail-enable GPIOs or PMIC enable registers from the schematic |
| `sense-parts` | `pmic.c`, `lilikoi.c`, `daemon.py` | `read_voltage`, `read_current` | part numbers and register maps at I2C `0x10/0x1f/0x48/0x49` |
| `clock-source` | `daemon.py` | `set_frequency`, so frequency shmoo | the clock path: FPGA PLL reprogramming, external generator, or a firmware command |
| `pcb-bounds` | `pmic.h` | nothing functionally; the 400-1200 mV defaults are a guess | VCORE trace/load ratings, set with `-DLILIKOI_PCB_RAIL_*_MV` |
| `jtag-adapter` | `openocd/fletcherlake.cfg` | `program_fpga` on the real cable | the adapter section of the hardware team's cfg |
| `uart-tsi-port` | `examples/cluster-board.yaml` | `start` on real hardware | where FPGA pins D23/D24 surface on the Mac (which USB-serial cable or on-board bridge) |
| `mcu-serial`, `jtag-serial` | `examples/cluster-board.yaml` | board identification | read off the board with `picotool info` and the adapter's USB descriptor |
| `chip`, `chip-limits` | `examples/cluster-board.yaml` | correct job matching and safe voltages | the installed chip's name as jobs request it, and its approved mV range per rail |

## Scheduler-side notes

- `wait_for_completion` is one blocking command, so the slot's `firmware_timeout_seconds`
  must exceed the longest expected job. fl-agent kills the CLI on timeout; the daemon and
  uart_tsi survive, and `stop` cleans them up.
- `read_uart` is polled every 20 ms by the executor; each poll is a CLI process. It works,
  but a streaming variant on the scheduler side would cut load noticeably.
- Shmoo voltages arrive as floats; they are rounded to whole mV and must land on a
  regulator step (5 mV on VCORE1/2; 1.25/2.5/5 mV on VCORE0 depending on its range).
- `power_on` is strict: a board with the PMIC unpopulated fails every job at step 1 with
  `PMIC_MISSING`. That is intended for production; for such a bench board use `mcu`
  passthrough commands instead.

## Troubleshooting

| Symptom | Look at |
|---|---|
| `DAEMON_UNAVAILABLE` | `<run-dir>/<mcu>.log`: usually an import error in the Python environment the CLI used, or `run_dir` not writable |
| `MCU_NOT_FOUND` | `lilikoi --mcu /dev/cu.usbmodemXXXX discover` to bypass serial lookup; `picotool info` for the real serial; check the board is not in BOOTSEL mode |
| `MCU_TIMEOUT` on every call | wrong firmware on the RP2350 (a legacy console image prints prompts, not tokens); reflash `lilikoi.uf2` |
| `PMIC_MISSING` | `lilikoi … mcu scan` for the per-device map; check the PMIC bus is powered |
| `OPENOCD_FAILED` | `details.output` holds the tail of OpenOCD's log; most often adapter not found (wrong cfg interface or missing `--jtag-serial`) or IDCODE mismatch |
| `FPGA_NOT_DONE` | bitstream for another part, or `DONE` not wired to GPIO7 on this board revision; compare `mcu fpga done` with the FPGA's DONE test point |
| `wait_for_completion` returns `exit_code` 1 within a second of `start` | `<mcu>.uart_tsi.log`: uart_tsi exits 1 when it cannot open `--tsi-tty`, the ELF fails to load, or `+selfcheck` readback mismatches, which is indistinguishable from a program that returned 1. The log says which; run `uart_tsi` by hand to confirm |
| job passes on the fake but `start` hangs on hardware | UART-TSI baud mismatch between the bitstream and `--tsi-baud`, or the FPGA is held in reset (`mcu reset fpga release`) |
