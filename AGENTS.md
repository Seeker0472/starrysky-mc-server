# AGENTS

## Project Overview

`mc-uart-server` is a minimal Minecraft Java Edition 1.8.x server for the StarrySky C2 board. A local Minecraft client connects to a Rust TCP-to-UART bridge on the host, and the bridge forwards Minecraft TCP bytes over a custom UART link to C firmware running on the board.

Current release target:

- Board: StarrySky C2.
- Minecraft protocol: Java 1.8.x / protocol 47.
- Authentication: offline mode, no encryption or online session validation.
- Players: single player.
- Default firmware: Minecraft protocol compression enabled, spawn chunks precompressed at build time and copied to PSRAM at boot.
- Default UART roles: UART0/SYS_UART/type-c for bridge data, UART1/HP_UART for firmware logs.

## Documentation Index

- [README.md](README.md): human-facing Chinese overview, quick build, and run flow.
- [docs/README.md](docs/README.md): Chinese documentation reading path.
- [docs/architecture.md](docs/architecture.md): architecture across the host bridge, custom UART link, firmware server, map data, and logging.
- [docs/build-and-run.md](docs/build-and-run.md): reproducible build and run instructions.
- [docs/compressed-map-psram.md](docs/compressed-map-psram.md): default compressed-map PSRAM firmware behavior.
- [docs/protocol/uart-link-v2.md](docs/protocol/uart-link-v2.md): custom UART link v2 frame format, handshake, flow control, reset, and error behavior.
- [docs/protocol/references.md](docs/protocol/references.md): upstream references for Minecraft 1.8, StarrySky C2, ECOS SDK, and protocol choices.

## Code Map

- `core/`: platform-independent C code for Minecraft protocol handling, the server state machine, UART link codec, ring buffers, and world data.
- `firmware/`: StarrySky C2 firmware entry point, UART/PSRAM/SDK adapters, link session scheduling, and logging.
- `bridge/`: Rust TCP-to-UART bridge with serial framing, rate calibration, retransmission, and serverbound movement filtering.
- `native/`: host-side C tests.
- `nix/`: Nix package definitions for firmware, bridge, native tests, and Windows bridge cross-builds.
- `scripts/generate_compressed_chunks.py`: generator for the default 3x3 compressed spawn chunk assets.

## Tooling

- If a task needs a command-line tool that is not installed locally, prefer using
  Nix to obtain it temporarily, for example with `nix shell nixpkgs#<package>`
  or `nix run nixpkgs#<package>`.
- Do not commit Nix result symlinks such as `result`, `result-*`, or similar
  build output links.
- If obtaining or running the tool requires network access, broader filesystem
  access, or elevated sandbox permissions, ask the user for approval first.
