# 压缩地图与 PSRAM 固件

默认 `firmware-c2` 是当前发布固件：启用 Minecraft 协议压缩，构建时把 `maps/showcase.png` 或 fallback superflat 地图生成成 3x3 spawn chunks 的 zlib 压缩资产，启动时复制到 C2 外部 PSRAM，然后在 Play bootstrap 阶段直接发送 Minecraft compressed packet frame。

旧的 `firmware-c2-legacy` 仍保留 SRAM 地图 fallback。当前 legacy profile 不启用协议压缩，不链接生成的压缩地图资产，也不走 PSRAM runtime path。

## 构建流程

`nix build .#firmware-c2` 会执行：

1. 运行 [scripts/generate_compressed_chunks.py](../scripts/generate_compressed_chunks.py)。
2. 如果 [maps/showcase.png](../maps/showcase.png) 存在，读取它作为地图源；如果不存在，生成原来的 superflat fallback。
3. 在 `core/generated/` 生成 `mc_world_compressed_assets.h` 和 `mc_world_compressed_assets.c`。
4. 从生成的 C 文件提取 `mc_world_compressed_total_bytes`，作为 PSRAM runtime arena 占用写入固件内存报告。
5. 交叉编译 C2 固件。
6. 安装 `mc_uart_fw.bin`、ELF、HEX、反汇编和 `memory-report.txt`。

构建命令：

```bash
nix build .#firmware-c2
```

产物：

```text
result/mc_uart_fw.bin
result/memory-report.txt
```

## 运行时行为

固件启动后：

1. `platform_psram_init()` 配置 QSPI/PSRAM 控制器。
2. `mc_world_compressed_init(platform_psram_base(), platform_psram_size())` 校验生成资产并复制压缩 payload。
3. 日志输出 `compressed map psram ready chunks=9`。
4. Login 阶段启用 Minecraft 协议压缩。
5. Play bootstrap 阶段逐个调用 `mc_world_queue_compressed_spawn_chunk()` 排队发送 9 个 compressed chunk frame。

如果 PSRAM 初始化或压缩地图初始化失败，固件会打印错误并停在死循环：

```text
psram init failed; halted
compressed map psram init failed; halted
```

## 数据格式

生成脚本构造的 chunk body 与 fallback 路径保持一致：

- packet id：`0x21`，即 Minecraft 1.8 clientbound Chunk Data。
- chunk 坐标：3x3 spawn chunks，中心 `(0, 0)`。
- ground-up continuous：true。
- primary bit mask：`0x0001`，只发送一个 section。
- block data：底层 bedrock，`y=1..3` 为 dirt，`y=4` 为 PNG 或 fallback 选择的顶层方块，其他为空气。
- light 和 biome：固定填充。

压缩 payload 是完整 chunk body 的 zlib 结果。发送时外层使用 Minecraft compression frame：

```text
packet length VarInt
data length VarInt = raw chunk body length
zlib compressed chunk body
```

小包例如 Login Success、Join Game、KeepAlive 仍可使用 `data length = 0` 的压缩 plain frame。

## 资源占用

不要在文档里固化某一次构建的 `text/data/bss` 数值。当前 Nix 构建会安装权威报告：

```bash
nix build .#firmware-c2
cat result/memory-report.txt
```

报告会区分 FLASH、SRAM 和 PSRAM。PSRAM 行包含运行时 arena，例如压缩地图资产复制后的 `mc_world_compressed_total_bytes`。

## 自定义地图

推荐的自定义路径是提交一个 `maps/showcase.png`。生成器在编译阶段固定读取这个路径，因此每个固件镜像的地图由构建时的源码资产决定。

PNG 要求：

- 尺寸必须是 48x48，对应当前 3x3 spawn chunk footprint。
- 支持 8-bit RGB/RGBA、非隔行 PNG，使用标准 zlib IDAT 数据。
- 每个像素必须完全不透明。
- 每个 RGB 必须精确命中生成器调色板。

PNG 到 chunk 的映射是：

```text
pixel_x = (chunk_x + 1) * 16 + local_x
pixel_z = (chunk_z + 1) * 16 + local_z
```

因此 chunk `(-1, -1)` 对应 PNG 左上 16x16，`(0, 0)` 对应中心，`(1, 1)` 对应右下。

内置调色板覆盖常用展示块：

| RGB | 方块 |
| --- | --- |
| `#00ff00` | grass |
| `#7f7f7f` | stone |
| `#8b4513` | dirt |
| `#ffff00` | sand |
| `#ffffff` | white wool |
| `#ff0000` | red wool |
| `#00ffff` | cyan wool |
| `#0000ff` | blue wool |
| `#000000` | black wool |

可选的 `maps/showcase.palette.json` 可以覆盖或扩展调色板：

```json
{
  "#00ff00": { "id": 2, "meta": 0 },
  "#ff0000": { "id": 35, "meta": 14 }
}
```

调色板 key 大小写不敏感，格式为 `#rrggbb`；`id` 必须是 `0..4095`，`meta` 必须是 `0..15`。透明像素、未知颜色、非法 palette JSON、非 48x48 尺寸和不支持的 PNG 特性都会让构建失败并输出具体错误。

无论使用 PNG 还是 fallback，仍需保持以下约束：

- `mc_world_compressed_asset_count` 不超过 `MC_WORLD_COMPRESSED_MAX_CHUNKS`。
- 每个 raw body 长度不超过 `MC_MAX_PACKET_BODY`。
- 每个 compressed payload 长度不超过 `MC_MAX_PACKET_BODY`。
- `mc_world_compressed_total_bytes` 不超过 `platform_psram_size()`。
- runtime chunk storage 继续放在 PSRAM，不要把大块地图数据搬回 SRAM。

改完后至少运行：

```bash
nix build .#native-tests
nix build .#firmware-c2
```
