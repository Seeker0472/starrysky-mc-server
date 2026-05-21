# 文档索引

本目录是发布给读者看的中文文档。实现细节以当前源码和构建脚本为准；如果文档与源码冲突，先修正文档或测试，再发布。

## 阅读路径

1. [架构说明](architecture.md)：先理解 Minecraft 客户端、Rust bridge、UART link v2、C2 固件和 core server 的数据流。
2. [编译与复现](build-and-run.md)：按 Nix 构建 native tests、bridge 和固件，并运行本机 Minecraft 连接测试。
3. [压缩地图与 PSRAM 固件](compressed-map-psram.md)：理解默认 `firmware-c2` 如何生成、复制和发送压缩 spawn chunks。
4. [UART link v2](protocol/uart-link-v2.md)：理解本项目自定义 UART 帧协议、握手、流控、重传和错误处理。
5. [协议与参考来源](protocol/references.md)：核对 Minecraft 1.8、StarrySky C2、SDK、本项目协议常量和源码锚点。

## 维护原则

- 写架构事实时引用当前源码路径，不用旧日志或记忆替代。
- 写构建命令时必须能在当前仓库根目录运行。
- 写资源占用时优先指向 `result/memory-report.txt`，不要固化某次构建的临时数字。
- 修改 Minecraft packet id、UART link 常量、固件 profile 或 Nix 输出时，同步更新对应文档。
