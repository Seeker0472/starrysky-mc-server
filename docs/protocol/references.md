# 协议与参考来源

修改协议、板卡寄存器、UART、PSRAM 或构建逻辑前，先确认这里列出的来源。当前实现事实优先级是：源码和测试 > Nix 构建脚本 > 本仓库文档 > 外部参考。

## 本项目固定选择

- Minecraft Java protocol version：47。
- 兼容目标：Minecraft Java Edition 1.8.x，包括用户当前使用的 1.8.1。
- Server mode：offline。
- Encryption/authentication：disabled。
- Compression：默认 `firmware-c2` 启用；当前 `firmware-c2-legacy` 不启用协议压缩，也不链接默认压缩地图资产。
- Compression threshold：默认 `8192`。
- Players：1。
- World：固定 3x3 superflat spawn chunks。
- Link protocol：本项目自定义 UART link v2，COBS + CRC16 + `0x00` delimiter；完整说明见 [UART Link V2](uart-link-v2.md)。

## Minecraft 1.8

- Protocol versions：<https://c4k3.github.io/wiki.vg/Protocol_version_numbers.html>
- wiki.vg Protocol 总览镜像：<https://c4k3.github.io/wiki.vg/Protocol.html>
- wiki.vg Chunk Format 总览镜像：<https://c4k3.github.io/wiki.vg/Chunk_Format.html>
- PrismarineJS 1.8 protocol.json：<https://github.com/PrismarineJS/minecraft-data/blob/master/data/pc/1.8/protocol.json>
- UNPKG 1.8 protocol.json 镜像：<https://app.unpkg.com/minecraft-data@3.102.3/files/minecraft-data/data/pc/1.8/protocol.json>

注意：wiki.vg 的 `Protocol.html` 镜像通常是当前/通用协议页面，不是严格的 1.8 专页。涉及 1.8 packet id 和字段时，优先核对 `minecraft-data/data/pc/1.8/protocol.json`，再用 wiki.vg 页面理解字段语义。

当前源码中用到的 1.8 packet id：

| 阶段 | 方向 | Packet id | 用途 |
| --- | --- | ---: | --- |
| Handshake | serverbound | `0x00` | Handshake |
| Status | serverbound | `0x00` | Request |
| Status | serverbound | `0x01` | Ping |
| Status | clientbound | `0x00` | Response |
| Status | clientbound | `0x01` | Pong |
| Login | serverbound | `0x00` | Login Start |
| Login | clientbound | `0x02` | Login Success |
| Login | clientbound | `0x03` | Set Compression |
| Play | clientbound | `0x00` | KeepAlive |
| Play | clientbound | `0x01` | Join Game |
| Play | serverbound | `0x01` | Chat Message |
| Play | clientbound | `0x02` | Chat Message |
| Play | clientbound | `0x03` | Time Update |
| Play | clientbound | `0x05` | Spawn Position |
| Play | clientbound | `0x06` | Update Health |
| Play | clientbound | `0x08` | Player Position And Look |
| Play | clientbound | `0x21` | Chunk Data |
| Play | clientbound | `0x2b` | Change Game State |
| Play | clientbound | `0x39` | Player Abilities |
| Play | serverbound | `0x00` | KeepAlive ACK |
| Play | serverbound | `0x03..0x06` | movement packets，bridge 会过滤 |

## StarrySky C2 和 ECOS SDK

- ECOS SDK upstream：<https://github.com/openecos-projects/embedded-sdk>
- ECOS documentation upstream：<https://github.com/openecos-projects/embedded-doc>
- StarrySky C2 Pico board overview：<https://github.com/openecos-projects/embedded-doc/blob/main/doc/src/zh/page/brd/starry-sky-c/v2.0_pico.md>
- ECOS SDK v2.0 start guide：<https://github.com/openecos-projects/embedded-doc/blob/main/doc/src/zh/page/sdk/v2.0/start/introduction.md>
- HP_UART API：<https://github.com/openecos-projects/embedded-doc/blob/main/doc/src/zh/page/sdk/v2.0/api/hp_uart.md>
- SYS_UART API：<https://github.com/openecos-projects/embedded-doc/blob/main/doc/src/zh/page/sdk/v2.0/api/sys_uart.md>
- Timer API：<https://github.com/openecos-projects/embedded-doc/blob/main/doc/src/zh/page/sdk/v2.0/api/timer.md>
- GPIO API：<https://github.com/openecos-projects/embedded-doc/blob/main/doc/src/zh/page/sdk/v2.0/api/gpio.md>
- C2 board registers：<https://github.com/openecos-projects/embedded-sdk/blob/main/board/StarrySkyC2/board.h>
- C2 startup：<https://github.com/openecos-projects/embedded-sdk/blob/main/board/StarrySkyC2/start.S>
- C2 linker script：<https://github.com/openecos-projects/embedded-sdk/blob/main/board/StarrySkyC2/sections.lds>
- HP_UART implementation：<https://github.com/openecos-projects/embedded-sdk/blob/main/components/hp_uart/src/hp_uart.c>
- Timer implementation：<https://github.com/openecos-projects/embedded-sdk/blob/main/components/timer/src/timer.c>

当前 C2 事实：

- CPU：PicoRV32，RV32IMAC，72 MHz。
- SRAM：128 KiB，`0x30000000..0x3001ffff`。
- PSRAM：8 MiB，`0x40000000..0x407fffff`。
- SPI Flash：16 MiB，`0x00000000..0x00ffffff`。
- SYS_UART 寄存器在 SDK `board.h` 中映射到 UART0。
- HP_UART 寄存器在 SDK `board.h` 中映射到 UART1。

## 本项目源码锚点

- Minecraft 协议常量：`core/include/mc_config.h`。
- Minecraft packet writer：`core/src/mc_packet.c`。
- Minecraft 状态机：`core/src/mc_server.c`。
- 默认世界生成：`core/src/mc_world.c`。
- 压缩地图运行时：`core/src/mc_world_compressed.c`。
- link v2 C codec：`core/src/mc_link.c`。
- link v2 Rust codec：`bridge/src/link.rs`。
- bridge movement 过滤：`bridge/src/c2m.rs`。
- 固件 UART/log 配置：`firmware/mc_firmware_config.h`。
- 固件主循环：`firmware/main.c`。
- 固件 link session：`firmware/mc_link_session.c`。
- PSRAM 初始化：`firmware/platform_psram.c`。
- Nix 固件构建：`nix/ecos-firmware.nix`。
- native tests：`nix/native-tests.nix`。

## 维护规则

- 修改 Minecraft packet id 或字段时，更新本文表格、相关 tests 和实现。
- 修改 link v2 常量时，同步 C/Rust codec 和 tests。
- 修改 UART/PSRAM 寄存器时，引用 ECOS SDK 上游资料或 `board.h`。
- 修改构建输出或 profile 时，同步 [README.md](../../README.md)、[docs/build-and-run.md](../build-and-run.md) 和 [docs/compressed-map-psram.md](../compressed-map-psram.md)。
