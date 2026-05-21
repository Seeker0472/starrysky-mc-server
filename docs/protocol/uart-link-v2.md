# UART Link V2

UART link v2 是本项目自定义的 bridge-to-firmware 二进制链路协议。它运行在 StarrySky C2 的 bridge UART 上，用来承载 Minecraft TCP 字节流；它不是 Minecraft 协议，也不是 ECOS SDK 提供的通用串口协议。

设计目标：

- 在 115200 baud 这类低速 UART 上给 Minecraft 字节流提供明确的帧边界。
- 用 CRC 检测串口字节损坏。
- 让 PC-to-firmware 方向有 ACK、credit 和超时重传。
- 让 firmware-to-PC 方向的损坏尽早暴露，便于调试链路时序。
- 支持 bridge 和固件在新 TCP 会话、错误或速率重新校准时重置链路状态。

## 字节编码

原始帧体先计算 CRC，再用 COBS 编码，最后追加一个 `0x00` 作为帧分隔符：

```text
raw frame body -> COBS encoded bytes -> 0x00 delimiter
```

COBS 编码保证 encoded body 内部不出现 `0x00`。接收端可以一直读到下一个 `0x00`，再把这一段作为完整帧解码。损坏帧会在当前 delimiter 处失败，不会无限污染后续流。

## 原始帧体

所有多字节整数均为 little-endian：

```text
u8  version       固定为 2
u8  type          帧类型
u8  flags         当前保留，发送端写 0
u16 seq           当前方向 DATA/控制帧序号
u16 ack           ACK 序号或随帧携带的对端序号信息
u16 payload_len   payload 字节数
u8[payload_len] payload
u16 crc16         覆盖 version..payload，little-endian
```

CRC 使用 CRC16/CCITT-FALSE 参数：初始值 `0xffff`，多项式 `0x1021`。

当前限制：

| 字段 | 值 |
| --- | ---: |
| version | `2` |
| 最大 encoded frame | `512` 字节 |
| 最大 payload | `497` 字节 |
| 初始 C2M credit | `512` 字节 |
| bridge/firmware rate mask | `0x03ff` |

这些常量在 C 端 [core/include/mc_link.h](../../core/include/mc_link.h) 和 Rust 端 [bridge/src/link.rs](../../bridge/src/link.rs) 中必须保持一致。

## 帧类型

| Type | 名称 | 方向 | payload | 作用 |
| --- | --- | --- | --- | --- |
| `0x01` | HELLO | bridge -> firmware | 6 字节 | 请求 payload、credit、rate mask |
| `0x02` | READY | firmware -> bridge | 11 字节 | 返回协商后的 payload、credit、rate mask、flags |
| `0x03` | DATA_C2M | bridge -> firmware | Minecraft TCP bytes | 客户端到固件的数据 |
| `0x04` | ACK_C2M | firmware -> bridge | 2 字节 | ACK C2M seq，并返回当前 RX free credit |
| `0x05` | DATA_M2C | firmware -> bridge | Minecraft TCP bytes | 固件到客户端的数据 |
| `0x06` | RATE_PROBE | bridge -> firmware | sleep_us、nonce | 测试 PC 写串口速率 |
| `0x07` | RATE_PROBE_ACK | firmware -> bridge | 原样 echo | 回复速率探测 |
| `0x08` | RESET | bridge -> firmware | empty | 请求重置链路和 Minecraft 逻辑会话 |
| `0x09` | RESET_ACK | firmware -> bridge | empty | RESET 回复 |
| `0x0a` | ERROR | firmware -> bridge | code、detail | 报告链路协议错误 |
| `0x0b` | PING | 双向 | opaque bytes | 链路探测 |
| `0x0c` | PONG | 双向 | opaque bytes | PING 回复 |

## HELLO / READY

Bridge 打开串口后反复发送 HELLO，直到收到 READY。

HELLO payload：

```text
u16 desired_payload
u16 desired_credit
u16 supported_rate_mask
```

READY payload：

```text
u16 negotiated_payload
u16 credit_cap
u16 initial_credit
u16 supported_rate_mask
u8  initial_rate_profile
u16 flags
```

当前固件会把 payload 限制到 `497` 字节，把 credit 限制到 `512` 字节，并把 supported rate mask 限制为 bridge 请求和固件能力的交集。

## C2M：客户端到固件

C2M 是 bridge 到固件方向，即 Minecraft client 到 MCU server。

该方向使用 stop-and-wait：

1. Bridge 从 TCP 读取字节。
2. Bridge 过滤 Minecraft 1.8 serverbound movement 包，保留 KeepAlive ACK 和其他包。
3. Bridge 在 credit 允许时发送一个 DATA_C2M。
4. Bridge 等待 ACK_C2M。
5. 固件收到 DATA_C2M 后检查 seq、payload 长度和 RX ring 空间。
6. 固件把 payload 写入 server RX ring，并回复 ACK_C2M。
7. Bridge 收到匹配 ACK 后，从 pending TCP 队列删除已经确认的字节，再发送下一帧。

ACK_C2M 的 `ack` 字段是下一个期望 C2M seq。payload 是 2 字节 little-endian credit，表示固件 RX ring 当前剩余空间。

如果 ACK 超时，bridge 会按当前速率 profile 重传同一 encoded DATA_C2M。连续丢失会触发降速或重新校准。

## M2C：固件到客户端

M2C 是固件到 bridge 方向，即 MCU server 到 Minecraft client。

当前 M2C 不做 ACK。固件把 server TX ring 切成最多 `negotiated_payload` 字节的 DATA_M2C，并递增 seq。Bridge 要求 DATA_M2C seq 连续；如果出现 decode error、CRC error 或 seq mismatch，bridge 返回错误并退出当前进程。

这个策略偏向调试可见性：M2C 损坏不被悄悄吞掉，而是立刻暴露为 bridge 错误。

## RESET 和逻辑会话

Bridge 在新 TCP 客户端连接、TCP EOF、TCP/serial 错误或重校准需要时发送 RESET。固件收到 RESET 后：

- 重置 link session 的 seq、parser、DATA 队列和 RX ring。
- 标记 Minecraft 逻辑会话需要 reset。
- 回复 RESET_ACK 和 READY。

固件侧的 `connection reset reason=link_reset` 表示 link/Minecraft 逻辑会话被重置，不表示板卡硬件复位。

## RATE_PROBE

Bridge 使用 RATE_PROBE 测试 PC 写 UART 的 inter-byte sleep。固件收到 RATE_PROBE 后回复 RATE_PROBE_ACK，payload 原样返回。Bridge 根据成功次数选择保守的 active profile，并在 DATA_C2M 重传时记录 loss，必要时重新 reset/calibrate。

## 错误处理

固件可能发送 ERROR：

| Code | 含义 |
| ---: | --- |
| `1` | bad version |
| `2` | bad length |
| `3` | CRC mismatch |
| `4` | unexpected type |
| `5` | sequence error |
| `6` | RX overflow |
| `7` | protocol state error |

Bridge 收到 ERROR 会重置链路状态。Bridge 自己检测到 M2C decode、CRC、length 或 sequence 错误时，会把错误返回给主程序；当前主程序对 `InvalidData` 链路错误选择退出，以便发布前调试能看见问题。

## 源码锚点

- C 常量和类型：[core/include/mc_link.h](../../core/include/mc_link.h)
- C 编解码实现：[core/src/mc_link.c](../../core/src/mc_link.c)
- 固件会话处理：[firmware/mc_link_session.c](../../firmware/mc_link_session.c)
- Rust 常量和编解码：[bridge/src/link.rs](../../bridge/src/link.rs)
- Bridge C2M 发送和过滤：[bridge/src/c2m.rs](../../bridge/src/c2m.rs)
- Bridge 会话循环：[bridge/src/session.rs](../../bridge/src/session.rs)
