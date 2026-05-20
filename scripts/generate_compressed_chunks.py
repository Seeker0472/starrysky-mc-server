#!/usr/bin/env python3
import argparse
import pathlib
import zlib

CHUNK_SECTION_BYTES = 8192 + 2048 + 2048 + 256
SPAWN_CHUNKS = [
    (0, 0),
    (1, 0),
    (-1, 0),
    (0, 1),
    (0, -1),
    (1, 1),
    (-1, -1),
    (1, -1),
    (-1, 1),
]


def varint(value: int) -> bytes:
    value &= 0xFFFFFFFF
    out = bytearray()
    while True:
        temp = value & 0x7F
        value >>= 7
        if value:
            temp |= 0x80
        out.append(temp)
        if not value:
            return bytes(out)


def i32(value: int) -> bytes:
    return int(value).to_bytes(4, "big", signed=True)


def u16(value: int) -> bytes:
    return int(value).to_bytes(2, "big", signed=False)


def block_for_y(y: int) -> int:
    if y == 0:
        return 7 << 4
    if y <= 3:
        return 3 << 4
    if y == 4:
        return 2 << 4
    return 0


def chunk_data() -> bytes:
    data = bytearray(CHUNK_SECTION_BYTES)
    for y in range(16):
        block = block_for_y(y)
        for z in range(16):
            for x in range(16):
                off = (((y * 16) + z) * 16 + x) * 2
                data[off] = block & 0xFF
                data[off + 1] = (block >> 8) & 0xFF

    pos = 8192
    data[pos:pos + 2048] = b"\xff" * 2048
    pos += 2048
    data[pos:pos + 2048] = b"\xff" * 2048
    pos += 2048
    data[pos:pos + 256] = b"\x01" * 256
    return bytes(data)


def chunk_body(chunk_x: int, chunk_z: int) -> bytes:
    data = chunk_data()
    return b"".join([
        varint(0x21),
        i32(chunk_x),
        i32(chunk_z),
        b"\x01",
        u16(0x0001),
        varint(len(data)),
        data,
    ])


def c_array(name: str, data: bytes) -> str:
    lines = [f"const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append("    " + ", ".join(f"0x{b:02x}u" for b in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def write_files(out_dir: pathlib.Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    chunks = []
    for index, (chunk_x, chunk_z) in enumerate(SPAWN_CHUNKS):
        raw = chunk_body(chunk_x, chunk_z)
        compressed = zlib.compress(raw, level=9)
        if zlib.decompress(compressed) != raw:
            raise SystemExit(f"compressed chunk {index} failed roundtrip")
        chunks.append((chunk_x, chunk_z, raw, compressed))

    header = out_dir / "mc_world_compressed_assets.h"
    source = out_dir / "mc_world_compressed_assets.c"

    header.write_text(
        """#ifndef MC_WORLD_COMPRESSED_ASSETS_H
#define MC_WORLD_COMPRESSED_ASSETS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t chunk_x;
    int32_t chunk_z;
    uint32_t raw_body_len;
    uint32_t compressed_len;
    const uint8_t *compressed;
} mc_world_compressed_asset_t;

extern const mc_world_compressed_asset_t mc_world_compressed_assets[];
extern const size_t mc_world_compressed_asset_count;
extern const uint32_t mc_world_compressed_total_bytes;

#endif
""",
        encoding="utf-8",
    )

    parts = [
        '#include "mc_world_compressed_assets.h"',
        "",
    ]
    total = 0
    for index, (_chunk_x, _chunk_z, _raw, compressed) in enumerate(chunks):
        total += len(compressed)
        parts.append(c_array(f"mc_world_compressed_chunk_{index}", compressed))
        parts.append("")

    parts.append("const mc_world_compressed_asset_t mc_world_compressed_assets[] = {")
    for index, (chunk_x, chunk_z, raw, compressed) in enumerate(chunks):
        parts.append(
            f"    {{ {chunk_x}, {chunk_z}, {len(raw)}u, {len(compressed)}u, mc_world_compressed_chunk_{index} }},"
        )
    parts.append("};")
    parts.append("")
    parts.append("const size_t mc_world_compressed_asset_count = sizeof(mc_world_compressed_assets) / sizeof(mc_world_compressed_assets[0]);")
    parts.append(f"const uint32_t mc_world_compressed_total_bytes = {total}u;")
    parts.append("")
    source.write_text("\n".join(parts), encoding="utf-8")


def check_files(out_dir: pathlib.Path) -> None:
    write_files(out_dir)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    out_dir = pathlib.Path(args.out_dir)
    if args.check:
        check_files(out_dir)
    else:
        write_files(out_dir)


if __name__ == "__main__":
    main()
