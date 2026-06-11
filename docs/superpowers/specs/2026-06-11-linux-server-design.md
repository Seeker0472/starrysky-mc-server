# Linux Direct Server Design

## Goal

Add a minimal Linux user-space Minecraft server entry point for x86-64 testing
and later LA32R Linux packaging. The program runs the existing architecture
independent `core/` Minecraft server directly behind a TCP socket.

The first version must match the default C2 gameplay behavior where practical:
Minecraft Java 1.8.x / protocol 47, offline mode, single player, protocol
compression enabled, and the generated showcase spawn chunks.

## Non-Goals

- Do not change the C2 firmware boot flow.
- Do not change the Rust TCP-to-UART bridge.
- Do not change UART link v2 framing, ACK, COBS, CRC, or rate probing.
- Do not add multi-player support.
- Do not add authentication, encryption, world simulation, mobs, physics, or a
  general-purpose Minecraft server feature set.

## Current System Boundary

The existing C2 path remains:

```text
Minecraft client
  -> Rust bridge
  -> UART link v2
  -> firmware/main.c
  -> core server
```

The new Linux direct path is separate:

```text
Minecraft client
  -> Linux TCP socket
  -> linux/mc_linux_server.c
  -> core server
```

The Linux direct server must not link or call `firmware/main.c`,
`firmware/platform_uart0.c`, `firmware/platform_ecos.c`,
`firmware/mc_link_session.c`, `core/src/mc_link.c`, or Rust `bridge/` code.

## Architecture

Add a small POSIX C entry point at `linux/mc_linux_server.c`, with these
responsibilities:

- Parse minimal CLI options.
- Create a listening TCP socket.
- Accept one Minecraft client at a time.
- Initialize `mc_server_t`, RX/TX rings, and compressed world runtime storage
  for each accepted connection.
- Feed socket bytes into `mc_server_receive()`.
- Drive `mc_server_tick_at()` from monotonic wall-clock time converted to
  `MC_SERVER_TICKS_PER_SECOND`.
- Drain the TX ring to the socket with nonblocking or short-write-safe sends.
- Reset per-connection state when the client disconnects or a protocol error
  occurs.
- Exit cleanly on `SIGINT` and `SIGTERM`.

The Linux entry point is only an I/O adapter. Minecraft protocol behavior stays
in `core/`.

## CLI

Initial CLI:

- `--listen <host:port>`: listen address. Default `127.0.0.1:25565`.
- `--verbose`: print connection and core trace events to stderr.
- `--help`: print usage.

The parser may be simple and dependency-free. IPv4 support is required for the
first version. IPv6 is optional.

## Compression And World Assets

The Linux server package must build with:

```text
MC_PROTOCOL_COMPRESSION_ENABLE=1
MC_USE_PSRAM_COMPRESSED_MAP=1
MC_COMPRESSION_THRESHOLD=8192u
```

The Nix package must run:

```text
python3 scripts/generate_compressed_chunks.py --out-dir core/generated --check
```

and compile `core/generated/mc_world_compressed_assets.c` into the Linux server.
Runtime must not require Python, zlib, or map asset files.

The compressed runtime arena must be normal process memory allocated at process
startup. It must be sized from `mc_world_compressed_total_bytes`. Failure to
initialize compressed chunks is fatal at startup.

## Socket Loop

The service loop must be deliberately simple:

1. Bind and listen.
2. Accept one TCP client.
3. Initialize a fresh core server session.
4. Repeatedly poll the socket and a short tick timeout.
5. On readable socket data, read into a small stack buffer and call
   `mc_server_receive()`.
6. On every loop, call `mc_server_tick_at()` with monotonic ticks.
7. Whenever TX ring data exists, send it to the client.
8. On EOF, send failure, or core receive/tick failure, close the connection and
   return to accept.

The first version handles one active client by only calling `accept()` after the
active client disconnects.

## Trace And Logging

`--verbose` must install `mc_server_set_trace()` and print compact events:

- handshake target state
- status request and ping
- login username
- play enter
- bootstrap stage and completion
- keepalive send and ACK
- queue full or unhandled play packet

Default logging must be quiet: startup address, accepted connection, disconnect
reason, and fatal errors.

## Build Integration

Add a new Nix package `.#linux-server`.

The package must:

- Use the repository's cleaned source.
- Generate compressed assets at build time.
- Compile with `cc -std=c11 -Wall -Wextra -Werror -O2`.
- Install `bin/mc-linux-server`.

Add a Make target:

```text
make linux-server
```

Do not alter existing package outputs:

- `.#firmware-c2`
- `.#firmware-c2-aggressive`
- `.#firmware-c2-legacy`
- `.#log-debug`
- `.#bridge`
- `.#bridge-windows`
- `.#native-tests`

## Documentation

Update the human-facing build/run docs with a Linux direct mode section:

```bash
nix build .#linux-server
./result/bin/mc-linux-server --listen 127.0.0.1:25565
```

The docs must state that this mode bypasses the Rust bridge and UART link, and
is intended for Linux host or Chiplab Linux user-space demonstrations.

## Verification

Required checks:

- `nix build .#linux-server`
- `nix build .#native-tests`
- `nix build .#bridge`
- `git diff --check`

Manual smoke test:

1. Run `./result/bin/mc-linux-server --listen 127.0.0.1:25565`.
2. Connect with Minecraft Java Edition 1.8.x to `127.0.0.1:25565`.
3. Confirm login reaches the showcase map.
4. Confirm `/help`, `/pos`, `/time day`, and `/weather clear` still work.
5. Stop with `Ctrl+C` and confirm clean exit.

## Acceptance Criteria

- Linux direct mode runs on x86-64 Linux without serial devices or the Rust
  bridge.
- C2 firmware and bridge build behavior remains unchanged.
- The Linux package uses the compressed showcase map path by default.
- The server handles one client at a time and returns to listening after
  disconnect.
- The implementation introduces no runtime dependency beyond libc and the Linux
  kernel socket/time APIs.
