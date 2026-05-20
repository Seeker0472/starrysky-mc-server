# C2 Compressed Map PSRAM Firmware

The default `firmware-c2` package is the C2 firmware profile for Minecraft
protocol compression with offline precompressed spawn chunks. The build runs
`scripts/generate_compressed_chunks.py`, links the generated compressed chunk
assets into the default firmware, copies those assets into PSRAM at boot, and
then sends chunk data through Minecraft compressed packet frames after login
enables protocol compression.

The `firmware-c2-legacy` package keeps the old SRAM map behavior. It does not
link generated compressed map assets and does not use the PSRAM compressed-map
runtime path.

## Build

Build the default compressed map PSRAM firmware:

```bash
nix build .#firmware-c2
```

The firmware binary to flash is:

```text
result/mc_uart_fw.bin
```

`make firmware` is the equivalent Make target.

The maintained firmware outputs are:

```text
firmware-c2            default compressed-map PSRAM firmware
firmware-c2-legacy     old SRAM map fallback
firmware-c2-aggressive default firmware with MCU-to-PC aggressive TX enabled
log-debug              default firmware with MC_LOG_DEBUG
```

## Flash And Run

Automatic flashing is not part of this project. Build the firmware, then flash
`result/mc_uart_fw.bin` using the existing board flashing workflow for the C2.

After flashing, connect the bridge UART to the host and run the bridge with a
baud rate matching the firmware UART configuration. The ready-made C2 profiles
use UART0/SYS_UART/type-c at 115200 baud for bridge traffic and UART1/HP_UART
at 115200 baud for firmware logs.

Windows example:

```powershell
mc-uart-bridge.exe --serial COM5 --baud 115200 --listen 127.0.0.1:25565
```

Linux example:

```bash
mc-uart-bridge --serial /dev/ttyUSB0 --baud 115200 --listen 127.0.0.1:25565
```

Do not enable `--verbose` during throughput measurements unless the log output
is the measurement target. Verbose bridge logging adds substantial host-side
I/O and can noticeably reduce measured throughput.

Start Minecraft Java Edition 1.8.x and connect to `127.0.0.1:25565`.

## Expected Behavior

At firmware startup, the compressed-map profile initializes PSRAM and copies
the generated compressed chunk assets into the PSRAM runtime arena. A successful
boot logs:

```text
compressed map psram ready chunks=<count>
```

If PSRAM initialization or compressed-map initialization fails, the firmware
logs the failure and halts:

```text
psram init failed; halted
compressed map psram init failed; halted
```

After a client joins and login enables Minecraft protocol compression, spawn
chunks are queued from the PSRAM compressed-map runtime and sent as compressed
Minecraft packet frames. The uncompressed fallback chunk builder remains for
baseline firmware and for paths where protocol compression is not enabled.

## Resource Impact

The following sizes were verified from the final Task 8 firmware builds with
`size result/mc_uart_fw` on May 20, 2026:

| Firmware | text | data | bss |
| --- | ---: | ---: | ---: |
| `firmware-c2-legacy` | 43536 | 0 | 121100 |
| `firmware-c2` | 45480 | 0 | 96628 |
| `firmware-c2-aggressive` | 45480 | 0 | 96628 |
| `log-debug` | 46312 | 0 | 96628 |
| Delta | +1944 | 0 | -24472 |

Task 7 previously recorded baseline `text` as 44248. The final Task 8 rebuild
produced legacy `text` 43536, so the table above uses the final build output.
The default `firmware-c2` values match the Task 7 compressed variant
`text 45480 / data 0 / bss 96628` verification.

## Custom Maps

Future custom maps should update the offline generator and map source that
produce the generated compressed assets. Keep precompressed map payloads out of
SRAM: generated assets may be linked for this variant, but runtime chunk storage
must stay in PSRAM so the firmware preserves SRAM for link buffers, server
state, and stack.
