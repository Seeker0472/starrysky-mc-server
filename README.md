# StarrySky MC-Server

`starrysky-mc-server` 是一个给 StarrySky C2 板卡运行的最小 Minecraft Java Edition 1.8.x 服务器。它不是通用 Minecraft 服务端；目标是把协议、地图和传输栈压到 C2 的资源约束里，让本机 Minecraft 客户端通过串口和板卡上的固件交互。

默认发布配置面向 Minecraft Java 1.8.x / protocol 47，离线模式，单人，启用协议压缩。主机侧 Rust bridge 监听 `127.0.0.1:25565`，把 TCP 字节流转换成项目自定义的 UART link v2 帧；板卡固件在 UART0/SYS_UART/type-c 上接收桥接数据，在 UART1/HP_UART 上输出日志。

## 运行架构

```text
Minecraft Java 1.8.x client
  <-> TCP 127.0.0.1:25565
  <-> bridge/mc-uart-bridge
  <-> UART link v2 over 115200 baud serial
  <-> StarrySky C2 UART0/SYS_UART
  <-> firmware/main.c
  <-> core Minecraft server

Firmware logs
  -> StarrySky C2 UART1/HP_UART at 115200 baud
```

核心分层：

- `core/` 是平台无关 C 代码，包含 Minecraft VarInt/packet writer、服务器状态机、link codec、环形缓冲、默认世界和压缩 chunk 运行时。
- `firmware/` 绑定 StarrySky C2 SDK，负责 UART、PSRAM、主循环、link session、日志和复位后的会话清理。
- `bridge/` 是 Rust 主机工具，负责 TCP 监听、串口帧解码、PC-to-firmware ACK/重传、速率探测和 movement 包过滤。
- `nix/` 是可复现构建入口，默认固件构建会生成压缩地图资产，并导出固件内存占用报告。

更完整的实现说明见 [docs/README.md](docs/README.md) 和 [docs/architecture.md](docs/architecture.md)。

## 目标平台与默认配置

- Board：StarrySky C2。
- CPU：PicoRV32 RV32IMAC，72 MHz。
- 内存：片上 SRAM 128 KiB，外部 PSRAM 8 MiB。
- 默认 bridge UART：UART0/SYS_UART/type-c，115200 baud。
- 默认 log UART：UART1/HP_UART，115200 baud。
- Minecraft：Java Edition 1.8.x，protocol 47。
- 服务端模式：offline，无加密，单人。
- 默认地图：`maps/showcase.png` 编译成 3x3 spawn chunks，离线 zlib 压缩后在启动时复制到 PSRAM；未提供 PNG 时回退到 superflat 风格。

## 快速构建

推荐使用 Nix：

```bash
nix build .#native-tests
nix build .#bridge
nix build .#firmware-c2
```

常用包：

```bash
nix build .#firmware-c2
nix build .#firmware-c2-aggressive
nix build .#firmware-c2-legacy
nix build .#log-debug
nix build .#bridge-windows
nix flake check
```

也可以进入开发 shell 后使用 Make 包装命令：

```bash
nix develop
make test-native
make bridge
make firmware
```

固件产物在 `result/` 下：

- `mc_uart_fw.bin`：用于刷写的固件镜像。
- `mc_uart_fw.elf` / `mc_uart_fw`：ELF。
- `mc_uart_fw.hex`：HEX。
- `mc_uart_fw.txt`：反汇编文本。
- `memory-report.txt`：FLASH、SRAM、PSRAM 占用报告。

构建与运行细节见 [docs/build-and-run.md](docs/build-and-run.md)。

## 自定义地图资产

默认压缩地图由 [maps/showcase.png](maps/showcase.png) 提供。构建会在编译阶段读取这个 48x48、8-bit RGB/RGBA、非隔行 PNG，把每个像素映射为 3x3 spawn chunk footprint 中 `y=4` 的顶层方块；底层仍保持 bedrock/dirt/air 结构。这个 PNG 是源资产，可以提交到 git；`core/generated/` 仍是构建输出，不要提交。

生成器使用严格调色板匹配。内置调色板覆盖 grass、stone、dirt、sand、white/red/cyan/blue/black wool；如需扩展颜色，可添加 `maps/showcase.palette.json`：

```json
{
  "#00ff00": { "id": 2, "meta": 0 },
  "#ff0000": { "id": 35, "meta": 14 }
}
```

调色板 key 是 `#rrggbb`，`id` 范围为 `0..4095`，`meta` 范围为 `0..15`。透明像素、未知颜色、非 48x48 尺寸或不支持的 PNG 特性会让构建失败。

## 运行 Bridge

Linux 示例：

```bash
mc-uart-bridge --serial /dev/ttyUSB0 --baud 115200 --listen 127.0.0.1:25565
```

Windows 示例：

```powershell
.\mc-uart-bridge.exe --serial COM5 --baud 115200 --listen 127.0.0.1:25565
```

需要导出 bridge 日志时，PowerShell 推荐把日志写到当前目录：

```powershell
$log = ".\bridge-$(Get-Date -Format yyyyMMdd-HHmmss).log"
cmd /c ".\mc-uart-bridge.exe --serial COM5 --baud 115200 --listen 127.0.0.1:25565 --verbose 2>&1" | Tee-Object -FilePath $log
```

启动 Minecraft Java Edition 1.8.x，添加服务器 `127.0.0.1:25565`。

## 协议和传输

Minecraft 层：

- 握手和 Status 请求使用普通 Minecraft frame。
- Login 阶段默认先发送 `Set Compression`，再发送压缩格式的 `Login Success`。
- Play 阶段发送 Join Game、Spawn Position、Time、Health、Player Position And Look、spawn chunks 和 KeepAlive。
- KeepAlive id 每次递增；客户端 ACK 只用于诊断，固件不会因为 missed keepalive 主动踢出玩家。

UART link v2：

- 帧体包含 version、type、flags、seq、ack、payload length、payload 和 CRC16。
- 整帧用 COBS 编码，并以 `0x00` 分隔。
- PC-to-firmware DATA 使用 stop-and-wait、ACK、绝对 credit 和超时重传。
- Firmware-to-PC DATA 当前不做 ACK；bridge 遇到解码、CRC 或 `DATA_M2C sequence mismatch` 会退出，让链路损坏在测试时显性暴露。
- Bridge 会丢弃 Minecraft 1.8 serverbound movement 包，避免客户端空闲移动上报填满低速 UART；KeepAlive 回复和非 movement 包会继续转发。

自定义 UART link v2 的完整格式见 [docs/protocol/uart-link-v2.md](docs/protocol/uart-link-v2.md)。协议来源和具体选择见 [docs/protocol/references.md](docs/protocol/references.md)。

## UART 与日志

固件 UART 配置在 [firmware/mc_firmware_config.h](firmware/mc_firmware_config.h)：

```c
#define MC_UART0_BAUD 115200u
#define MC_UART1_BAUD 115200u
#define MC_BRIDGE_UART_ID MC_UART_ID_0
#define MC_LOG_UART_ID MC_UART_ID_1
#define MC_LOG_LEVEL MC_LOG_INFO
```

`MC_BRIDGE_UART_ID` 和 `MC_LOG_UART_ID` 必须不同，避免日志字节污染 bridge 数据链路。

日志级别：

- `MC_LOG_OFF`：关闭日志。
- `MC_LOG_INFO`：启动、登录、连接复位和关键错误。
- `MC_LOG_DEBUG`：额外的帧、KeepAlive、回压和链路诊断。
- `MC_LOG_TRACE`：原始数据 dump，仅用于短时诊断，可能改变串口时序。

`log-debug` 包使用默认压缩地图 PSRAM 配置，并把固件日志级别设为 `MC_LOG_DEBUG`：

```bash
nix build .#log-debug
```

## 参考来源

Minecraft 1.8、StarrySky C2、ECOS SDK 和本项目协议选择的来源见 [docs/protocol/references.md](docs/protocol/references.md)。
