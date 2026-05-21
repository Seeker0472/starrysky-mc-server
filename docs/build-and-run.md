# 编译与复现

本文记录当前 `master` 的推荐复现流程。命令默认在仓库根目录运行。

## 环境要求

- Nix flakes 可用。
- 本仓库旁边存在 `../embedded-sdk`，因为 [flake.nix](../flake.nix) 通过 `git+file:../embedded-sdk` 引入 C2 SDK。
- 运行 bridge 的主机能访问连接 C2 的串口。
- Minecraft Java Edition 1.8.x 客户端，推荐本机离线测试服务器地址 `127.0.0.1:25565`。

不需要手动安装 RISC-V 工具链、Rust 或 SDK 依赖；Nix derivation 会提供构建所需工具。

## 一次性验证

```bash
nix build .#native-tests
nix build .#bridge
nix build .#firmware-c2
```

完整检查：

```bash
nix flake check
```

注意不要提交 `result`、`result-*`、`build/`、`.ecos-build/` 或 `bridge/target/`。

## 固件构建

默认固件：

```bash
nix build .#firmware-c2
```

输出：

```text
result/mc_uart_fw
result/mc_uart_fw.elf
result/mc_uart_fw.bin
result/mc_uart_fw.hex
result/mc_uart_fw.txt
result/memory-report.txt
```

查看内存报告：

```bash
cat result/memory-report.txt
```

固件 profile：

| Nix package | 用途 |
| --- | --- |
| `firmware-c2` | 默认发布固件，启用协议压缩和 PSRAM 压缩地图 |
| `firmware-c2-aggressive` | 默认固件加 MCU-to-PC aggressive TX pacing |
| `firmware-c2-legacy` | 旧 SRAM 地图 fallback，不启用协议压缩，不链接压缩地图资产 |
| `log-debug` | 默认固件加 `MC_LOG_DEBUG` |

对应 Make 包装：

```bash
make firmware
make firmware-aggressive
make firmware-legacy
make firmware-log-debug
```

本项目不包含自动刷写流程。刷写时使用现有 C2 板卡工具，把 `result/mc_uart_fw.bin` 写入板卡。

## Bridge 构建

Linux bridge：

```bash
nix build .#bridge
```

Windows bridge 交叉编译：

```bash
nix build .#bridge-windows
```

也可以进入 dev shell 后构建：

```bash
nix develop
make bridge
```

## 连接和运行

1. 刷写 `firmware-c2` 或 `log-debug`。
2. 打开固件日志 UART，默认 UART1/HP_UART，115200 baud。
3. 启动 bridge。
4. 启动 Minecraft 1.8.x，连接 `127.0.0.1:25565`。

Linux bridge 示例：

```bash
./result/bin/mc-uart-bridge --serial /dev/ttyUSB0 --baud 115200 --listen 127.0.0.1:25565
```

Windows bridge 示例：

```powershell
.\mc-uart-bridge.exe --serial COM5 --baud 115200 --listen 127.0.0.1:25565
```

如果固件使用了非默认 `MC_UART0_BAUD` 或 `MC_UART1_BAUD`，bridge 的 `--baud` 必须和 `MC_BRIDGE_UART_ID` 指向的 UART 波特率一致。

## 导出 Bridge 日志

PowerShell 直接管道 native stderr 有时会显示 `NativeCommandError` 样式的包装信息。推荐通过 `cmd /c` 收集 stdout/stderr，并把日志写到当前目录：

```powershell
$log = ".\bridge-$(Get-Date -Format yyyyMMdd-HHmmss).log"
cmd /c ".\mc-uart-bridge.exe --serial COM5 --baud 115200 --listen 127.0.0.1:25565 --verbose 2>&1" | Tee-Object -FilePath $log
```

`--verbose` 会输出 TCP payload、ACK、重传、串口 RX 等调试信息。吞吐测试时不要开 `--verbose`，因为主机日志 I/O 会改变测量结果。

## 预期固件日志

默认压缩地图固件启动后应能看到：

```text
[I] mc-uart boot
[I] uart0_baud=115200 uart1_baud=115200 bridge_uart=0 log_uart=1
[I] compressed map psram ready chunks=9
[I] connection reset reason=boot
```

客户端连接后应看到：

```text
[I] connection reset reason=link_reset
[I] login username=<name>
[I] play enter
[I] world bootstrap complete chunks=9
```

`log-debug` 下还会看到 KeepAlive、frame ready、bootstrap stage、link queue 等 debug 日志。

## 常见现象

- `connection reset reason=link_reset`：bridge 新 TCP 客户端连接时会重置 link/Minecraft 逻辑会话，这是预期行为。
- `DATA_M2C sequence mismatch`、CRC 错误或 decode 错误：bridge 认为 firmware-to-PC 链路字节损坏，会退出，让问题显性化。
- 长时间 `DATA_C2M retransmit`：PC-to-firmware DATA 没有及时 ACK，bridge 会重传，并可能重新校准速率。
- 客户端 movement spam：bridge 会丢弃 `0x03..0x06` serverbound movement 包，防止 pending TCP 队列被空闲移动上报打满。
- 固件没有主动踢人：KeepAlive ACK 仅用于诊断；要强制全新硬件状态，请按板卡 reset。

## 发布检查

发布前建议执行：

```bash
nix build .#native-tests
nix build .#bridge
nix build .#firmware-c2
nix flake check
git diff --check
```

如果移动 tag 或发布版本，先确认：

```bash
git status --short --branch
git log --oneline --decorate --max-count=20
git tag --list 'v*' --format='%(refname:short) %(objectname:short) %(subject)'
```
