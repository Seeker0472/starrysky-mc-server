# References

## Local StarrySky C2 and SDK

- Board overview: `/home/seeker/embedded/embedded-doc/doc/src/zh/page/brd/starry-sky-c/v2.0_pico.md`
- HP_UART API: `/home/seeker/embedded/embedded-doc/doc/src/zh/page/sdk/v2.0/api/hp_uart.md`
- SYS_UART API: `/home/seeker/embedded/embedded-doc/doc/src/zh/page/sdk/v2.0/api/sys_uart.md`
- Timer API: `/home/seeker/embedded/embedded-doc/doc/src/zh/page/sdk/v2.0/api/timer.md`
- GPIO API: `/home/seeker/embedded/embedded-doc/doc/src/zh/page/sdk/v2.0/api/gpio.md`
- C2 board registers: `/home/seeker/embedded/embedded-sdk/board/StarrySkyC2/board.h`
- C2 startup: `/home/seeker/embedded/embedded-sdk/board/StarrySkyC2/start.S`
- C2 linker script: `/home/seeker/embedded/embedded-sdk/board/StarrySkyC2/sections.lds`
- HP_UART implementation: `/home/seeker/embedded/embedded-sdk/components/hp_uart/src/hp_uart.c`
- Timer implementation: `/home/seeker/embedded/embedded-sdk/components/timer/src/timer.c`
- Nix hello reference: `/home/seeker/embedded/nix-hello`

## Minecraft Java 1.8.x

- Protocol: https://c4k3.github.io/wiki.vg/Protocol.html
- Chunk format: https://c4k3.github.io/wiki.vg/Chunk_Format.html
- Protocol versions: https://c4k3.github.io/wiki.vg/Protocol_version_numbers.html
- Machine-readable 1.8 protocol: https://github.com/PrismarineJS/minecraft-data/blob/master/data/pc/1.8/protocol.json
- UNPKG mirror: https://app.unpkg.com/minecraft-data@3.102.3/files/minecraft-data/data/pc/1.8/protocol.json

## Fixed Protocol Choices

- Java protocol version: 47
- Compression: disabled
- Encryption/authentication: disabled
- Players: 1
- World: generated superflat chunk data
