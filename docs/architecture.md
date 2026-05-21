# 架构说明

本文按当前 `master` 源码描述项目架构。实现事实以源码、Nix 构建脚本和测试为准；外部网页和旧文档只作为参考。

## 总体数据流

```text
Minecraft client
  -> TCP 127.0.0.1:25565
  -> Rust bridge
  -> UART link v2 frames
  -> firmware link session
  -> core server receive ring
  -> Minecraft server state machine
  -> server TX ring
  -> firmware DATA_M2C frames
  -> bridge
  -> TCP bytes back to client
```

日志独立走另一条 UART，默认是 UART1/HP_UART。数据 UART 和日志 UART 不能共用，否则日志文本会破坏 link v2 的 COBS 帧边界。

## Core

`core/` 不直接依赖 C2 SDK，便于宿主机测试。

- [core/src/mc_varint.c](../core/src/mc_varint.c)：Minecraft VarInt 编解码。
- [core/src/mc_packet.c](../core/src/mc_packet.c)：Minecraft packet writer、普通 frame、压缩 plain frame 和压缩 payload frame。
- [core/src/mc_server.c](../core/src/mc_server.c)：Minecraft 状态机。
- [core/src/mc_link.c](../core/src/mc_link.c)：UART link v2 的 COBS/CRC codec。
- [core/src/mc_ringbuf.c](../core/src/mc_ringbuf.c)：固件和 native tests 共用的环形缓冲。
- [core/src/mc_world.c](../core/src/mc_world.c)：未压缩 fallback 世界。
- [core/src/mc_world_compressed.c](../core/src/mc_world_compressed.c)：PSRAM 压缩 chunk 运行时。

Minecraft 服务端状态机支持四个状态：

- `MC_CONN_HANDSHAKE`
- `MC_CONN_STATUS`
- `MC_CONN_LOGIN`
- `MC_CONN_PLAY`

Login 后默认进入压缩模式。Play bootstrap 分阶段发送 Join Game、Spawn Position、Time、Health、Player Position And Look 和 3x3 spawn chunks。bootstrap 完成后，服务器按 `MC_KEEPALIVE_INTERVAL_TICKS` 发送 KeepAlive。KeepAlive ACK 会清除诊断用 pending 标记，但服务端不会因为 missed ACK 主动断开会话。

## Firmware

`firmware/main.c` 是板卡固件入口。主循环反复执行：

1. `pump_link_rx()` 从 bridge UART 读取 link v2 字节，解析 C2M DATA、RESET、RATE_PROBE 等帧。
2. `pump_server()` 从 RX ring 取 Minecraft 字节交给 `mc_server_receive()`，再调用 `mc_server_tick_at()` 推进 bootstrap 和 KeepAlive。
3. `pump_link_tx()` 把服务器 TX ring 包成 DATA_M2C，或优先发送 READY、ACK、ERROR 等控制帧。

固件启动时：

- 初始化平台 UART/计时器。
- 初始化 link session。
- 默认压缩地图固件会初始化 PSRAM，并把生成的压缩 chunk 资产复制到 PSRAM。
- 调用 `reset_connection("boot")` 清空 Minecraft 逻辑会话。

Bridge 发来 RESET 时，固件只重置 link/Minecraft 逻辑状态，不需要重新刷写固件。若需要从板卡硬件状态完全重来，使用 C2 reset 按键。

## Bridge

`bridge/` 是 Rust 主机程序。

- [bridge/src/main.rs](../bridge/src/main.rs)：CLI、TCP listener、串口打开。
- [bridge/src/session.rs](../bridge/src/session.rs)：主会话循环，连接 TCP、串口 RX worker、C2M sender 和重传逻辑。
- [bridge/src/link.rs](../bridge/src/link.rs)：link v2 codec，与 C 端常量保持一致。
- [bridge/src/c2m.rs](../bridge/src/c2m.rs)：PC-to-firmware 队列、ACK 处理、重传和 movement 过滤。
- [bridge/src/rate_calibration.rs](../bridge/src/rate_calibration.rs)：RATE_PROBE 速率探测。

Bridge 对 PC-to-firmware DATA 使用 stop-and-wait。每次只允许一个 outstanding DATA_C2M，收到 ACK 后再从 pending TCP 队列弹出对应字节。若超时，bridge 按当前速率 profile 重传；连续丢失会触发重新校准。

Minecraft 1.8 客户端会高频发送 serverbound movement 包。低速 UART 不能长期承载这些空闲上报，所以 bridge 在 Minecraft frame 层解析并丢弃 packet id `0x03..0x06`。解析失败时采用 fail-open：把当前缓冲转发给固件，避免过滤器误删未知数据。

## UART Link V2

UART link v2 是本项目为低速 UART 自定义的二进制链路协议，不是 Minecraft 协议的一部分，也不是 StarrySky/ECOS SDK 自带协议。它只负责在 bridge 和固件之间可靠地搬运 Minecraft TCP 字节，并提供链路 reset、能力协商、速率探测和错误上报。

link v2 原始帧体：

```text
u8  version = 2
u8  type
u8  flags
u16 seq little-endian
u16 ack little-endian
u16 payload_len little-endian
u8[payload_len] payload
u16 crc16 little-endian
```

帧体使用 CRC16/CCITT-FALSE 初始值 `0xffff`、多项式 `0x1021`。编码后用 COBS 去除内部 `0x00`，并追加 `0x00` 作为帧分隔符。

当前类型：

| Type | 名称 | 方向 | 用途 |
| --- | --- | --- | --- |
| `0x01` | HELLO | bridge -> firmware | 请求 payload、credit、rate 能力 |
| `0x02` | READY | firmware -> bridge | 返回协商结果 |
| `0x03` | DATA_C2M | bridge -> firmware | Minecraft TCP 字节 |
| `0x04` | ACK_C2M | firmware -> bridge | ACK 和当前 RX free credit |
| `0x05` | DATA_M2C | firmware -> bridge | Minecraft TCP 字节 |
| `0x06` | RATE_PROBE | bridge -> firmware | PC 写串口速率探测 |
| `0x07` | RATE_PROBE_ACK | firmware -> bridge | 速率探测回复 |
| `0x08` | RESET | bridge -> firmware | 重置 link/Minecraft 会话 |
| `0x09` | RESET_ACK | firmware -> bridge | RESET 回复 |
| `0x0a` | ERROR | firmware -> bridge | 协议错误 |
| `0x0b` | PING | 双向 | 链路探测 |
| `0x0c` | PONG | 双向 | 链路探测回复 |

最大编码帧长是 512 字节，最大 payload 是 497 字节。READY payload 当前是 11 字节：negotiated payload、credit cap、initial credit、supported rate mask、initial rate profile、flags。

完整协议说明见 [UART link v2](protocol/uart-link-v2.md)。

## 地图数据

默认地图是 3x3 spawn chunks。未压缩 fallback 路径在运行时构造 superflat chunk：底层 bedrock，若干层 dirt/grass，其余为空气，并填充 skylight/block light 和 biome。

默认 `firmware-c2` 不在运行时构造完整 chunk，而是在 Nix 构建中运行 [scripts/generate_compressed_chunks.py](../scripts/generate_compressed_chunks.py)：

1. 生成与 fallback 一致的 chunk body。
2. 用 zlib level 9 压缩。
3. 写出 `core/generated/mc_world_compressed_assets.{h,c}`。
4. 固件启动后把压缩 payload 复制到 PSRAM runtime arena。
5. Play bootstrap 时直接发送 Minecraft compressed packet frame。

这样可以把大块 chunk 数据从 SRAM 压力中移走。

## 构建与测试边界

`nix build .#native-tests` 会覆盖：

- 默认非压缩路径。
- 协议压缩但不启用 PSRAM 压缩地图的路径。
- 协议压缩加 PSRAM 压缩地图路径。
- link codec、link session、日志宏、UART 配置约束、PSRAM flow test 边界。

`nix build .#bridge` 会运行 Rust crate 的测试。`nix build .#firmware-c2` 会交叉编译 StarrySky C2 固件，并安装内存报告。
