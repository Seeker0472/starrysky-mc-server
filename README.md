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
