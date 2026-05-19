# MC UART Server

Minimal Minecraft Java Edition 1.8.x server for StarrySky C2 using a UART byte stream.

## Target

- Board: StarrySky C2
- CPU: PicoRV32 RV32IMAC, 72 MHz
- Default bridge data link: UART0/SYS_UART/type-c at 115200 baud
- Default log output: UART1/HP_UART at 115200 baud
- Minecraft protocol: Java Edition 1.8.x, protocol version 47
- Server mode: offline, no encryption, no compression, one player

## Architecture

```text
Minecraft client
  <-> TCP 127.0.0.1:25565
  <-> Rust bridge
  <-> type-c serial port at 115200 baud
  <-> StarrySky C2 UART0/SYS_UART
  <-> C server core

Firmware logs
  -> StarrySky C2 UART1/HP_UART at 115200 baud
```

## Commands

```bash
nix build
nix build .#native-tests
nix build .#bridge
nix flake check
nix develop
```

Inside `nix develop`:

```bash
make firmware
make test-native
make bridge
```

## UART Roles and Logging

Firmware UART configuration lives in `firmware/mc_firmware_config.h`.

Defaults:

```c
#define MC_UART0_BAUD 115200u
#define MC_UART1_BAUD 115200u
#define MC_BRIDGE_UART_ID MC_UART_ID_0
#define MC_LOG_UART_ID MC_UART_ID_1
#define MC_LOG_LEVEL MC_LOG_INFO
```

`MC_BRIDGE_UART_ID` and `MC_LOG_UART_ID` must be different so firmware logs never share the bridge data link. To reverse the UART responsibilities, set `MC_BRIDGE_UART_ID` to `MC_UART_ID_1` and `MC_LOG_UART_ID` to `MC_UART_ID_0`, or build the provided reversed variant:

```bash
nix build .#firmware-c2-uart-reversed
```

Firmware logging supports `MC_LOG_OFF`, `MC_LOG_INFO`, `MC_LOG_DEBUG`, and `MC_LOG_TRACE`. `INFO` logs boot, link, state, and error events; `DEBUG` adds more detailed packet and byte-flow diagnostics; `TRACE` adds raw received data dumps.

Ready-made C2 firmware variants keep UART0 as the bridge and UART1 as the log output while selecting the log level:

```bash
nix build .#log-debug
nix build .#log-trace
nix build .#log-info
nix build .#log-none
```

## Reference Policy

Do not guess board registers, UART status bits, memory ranges, Minecraft packet IDs, packet fields, or chunk layouts. Check `docs/protocol/references.md` before changing those areas.

## Flashing

Automatic flashing is not part of this project. The build produces firmware artifacts only.

## Running the Bridge

The bridge and firmware use UART link protocol V2. V2 frames are COBS-encoded
and terminated with `0x00`, so corrupted frames are discarded at the next
delimiter. PC-to-firmware DATA frames use stop-and-wait ACK/retransmission and
absolute ACK credit. Firmware-to-PC DATA frames are not ACKed in this version;
the bridge treats any decode, CRC, or DATA_M2C sequence error as fatal and exits
so link corruption is visible during testing.

PC-to-firmware traffic calibrates conservative per-byte pacing with RATE_PROBE
frames. Calibration waits for the link to settle, warms up at the slowest
profile, then requires three formal probes per profile. A 3/3 profile is stable;
a 2/3 profile is treated as borderline and stops probing. Real DATA is written
one byte at a time using one supported profile slower than the fastest stable or
borderline result, and downshifts on retransmit timeouts.

The default bridge UART baud remains 115200 for conservative bring-up. For
MCU-to-PC throughput testing, build firmware with a higher `MC_UART0_BAUD` or
`MC_UART1_BAUD` and run the bridge with the matching `--baud` value.

Use `log-debug` firmware for gameplay tests. `log-trace` is intended for short
diagnostics only because raw frame dumps can slow the firmware main loop enough
to change UART timing.

Linux example:

```bash
mc-uart-bridge --serial /dev/ttyUSB0 --baud 115200 --listen 127.0.0.1:25565
```

Windows example:

```powershell
mc-uart-bridge.exe --serial COM3 --baud 115200 --listen 127.0.0.1:25565
```

Build a Windows bridge executable from Nix on Linux:

```bash
nix build .#bridge-windows
```

Start Minecraft Java Edition 1.8.x and add server `127.0.0.1:25565`.

The first target is entering the world. Smooth gameplay is outside the first milestone.
