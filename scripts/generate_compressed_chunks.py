#!/usr/bin/env python3
import argparse
import binascii
import json
import pathlib
import struct
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
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
SHOWCASE_PNG = pathlib.Path("maps/showcase.png")
SHOWCASE_PALETTE = pathlib.Path("maps/showcase.palette.json")
MAP_SIZE = 48
CHUNK_SIZE = 16
DEFAULT_PALETTE = {
    "#00ff00": {"id": 2, "meta": 0},
    "#7f7f7f": {"id": 1, "meta": 0},
    "#8b4513": {"id": 3, "meta": 0},
    "#ffff00": {"id": 12, "meta": 0},
    "#ffffff": {"id": 35, "meta": 0},
    "#ff0000": {"id": 35, "meta": 14},
    "#00ffff": {"id": 35, "meta": 9},
    "#0000ff": {"id": 35, "meta": 11},
    "#000000": {"id": 35, "meta": 15},
}


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


def fail(message: str) -> None:
    raise SystemExit(message)


def paeth_predictor(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def unfilter_scanline(filter_type: int, row: bytes, prev: bytes, bpp: int) -> bytes:
    out = bytearray(row)
    if filter_type == 0:
        return bytes(out)
    if filter_type == 1:
        for i in range(len(out)):
            left = out[i - bpp] if i >= bpp else 0
            out[i] = (out[i] + left) & 0xFF
        return bytes(out)
    if filter_type == 2:
        for i in range(len(out)):
            out[i] = (out[i] + prev[i]) & 0xFF
        return bytes(out)
    if filter_type == 3:
        for i in range(len(out)):
            left = out[i - bpp] if i >= bpp else 0
            up = prev[i]
            out[i] = (out[i] + ((left + up) // 2)) & 0xFF
        return bytes(out)
    if filter_type == 4:
        for i in range(len(out)):
            left = out[i - bpp] if i >= bpp else 0
            up = prev[i]
            upper_left = prev[i - bpp] if i >= bpp else 0
            out[i] = (out[i] + paeth_predictor(left, up, upper_left)) & 0xFF
        return bytes(out)
    fail(f"unsupported PNG filter type {filter_type}")


def read_png_rgba(path: pathlib.Path) -> tuple[int, int, list[tuple[int, int, int, int]]]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIGNATURE):
        fail(f"{path}: not a PNG file")

    pos = len(PNG_SIGNATURE)
    width = height = bit_depth = color_type = compression = filter_method = interlace = None
    idat = bytearray()
    saw_idat = False
    idat_closed = False
    saw_iend = False

    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        if any(not ((65 <= byte <= 90) or (97 <= byte <= 122)) for byte in kind):
            fail(f"{path}: invalid PNG chunk type {kind.decode('ascii', 'replace')}")
        if kind == b"IDAT" and width is None:
            fail(f"{path}: IDAT before IHDR chunk")
        if width is None and kind != b"IHDR":
            fail(f"{path}: first PNG chunk must be IHDR")
        if kind == b"IHDR" and width is not None:
            fail(f"{path}: duplicate IHDR chunk")
        if kind == b"IDAT" and idat_closed:
            fail(f"{path}: non-consecutive IDAT chunks")
        chunk_start = pos + 8
        chunk_end = chunk_start + length
        crc_end = chunk_end + 4
        if crc_end > len(data):
            fail(f"{path}: truncated PNG chunk")
        payload = data[chunk_start:chunk_end]
        expected_crc = struct.unpack(">I", data[chunk_end:crc_end])[0]
        actual_crc = binascii.crc32(kind + payload) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            fail(f"{path}: PNG chunk {kind.decode('ascii', 'replace')} CRC mismatch")
        pos = crc_end

        if kind == b"IHDR":
            if length != 13:
                fail(f"{path}: invalid IHDR length")
            width, height, bit_depth, color_type, compression, filter_method, interlace = struct.unpack(">IIBBBBB", payload)
        elif kind == b"IDAT":
            saw_idat = True
            idat.extend(payload)
        elif kind == b"IEND":
            if length != 0:
                fail(f"{path}: invalid IEND length")
            saw_iend = True
            break
        elif kind == b"PLTE":
            if saw_idat:
                fail(f"{path}: PLTE after IDAT chunk")
        elif kind == b"tRNS":
            fail(f"{path}: unsupported PNG transparency chunk tRNS")
        else:
            if 65 <= kind[0] <= 90:
                fail(f"{path}: unsupported critical PNG chunk {kind.decode('ascii', 'replace')}")
            if saw_idat:
                idat_closed = True

    if not saw_iend:
        fail(f"{path}: missing IEND chunk")
    if pos != len(data):
        fail(f"{path}: data after IEND chunk")
    if width is None or height is None:
        fail(f"{path}: missing IHDR chunk")
    if width == 0 or height == 0:
        fail(f"{path}: zero PNG width or height")
    if bit_depth != 8:
        fail(f"{path}: unsupported PNG bit depth {bit_depth}; expected 8")
    if color_type not in (2, 6):
        fail(f"{path}: unsupported PNG color type {color_type}; expected RGB or RGBA")
    if compression != 0 or filter_method != 0:
        fail(f"{path}: unsupported PNG compression or filter method")
    if interlace != 0:
        fail(f"{path}: unsupported PNG interlace method {interlace}")

    channels = 3 if color_type == 2 else 4
    row_len = width * channels
    expected_len = height * (1 + row_len)
    try:
        decompressor = zlib.decompressobj()
        raw = decompressor.decompress(bytes(idat), expected_len + 1)
    except zlib.error as exc:
        fail(f"{path}: could not decompress PNG IDAT data: {exc}")
    if decompressor.unconsumed_tail:
        fail(f"{path}: decoded PNG data length {len(raw)} does not match expected {expected_len}")
    if decompressor.unused_data:
        fail(f"{path}: trailing data after PNG IDAT zlib stream")
    if not decompressor.eof:
        fail(f"{path}: incomplete PNG IDAT zlib stream")

    if len(raw) != expected_len:
        fail(f"{path}: decoded PNG data length {len(raw)} does not match expected {expected_len}")

    pixels: list[tuple[int, int, int, int]] = []
    prev = bytes(row_len)
    raw_pos = 0
    for _y in range(height):
        filter_type = raw[raw_pos]
        raw_pos += 1
        row = unfilter_scanline(filter_type, raw[raw_pos:raw_pos + row_len], prev, channels)
        raw_pos += row_len
        prev = row
        for x in range(width):
            off = x * channels
            r = row[off]
            g = row[off + 1]
            b = row[off + 2]
            a = row[off + 3] if channels == 4 else 255
            pixels.append((r, g, b, a))

    return width, height, pixels


def parse_color_key(key: str) -> tuple[int, int, int]:
    if not isinstance(key, str) or len(key) != 7 or key[0] != "#":
        fail(f"invalid color key {key!r}; expected #rrggbb")
    try:
        return (int(key[1:3], 16), int(key[3:5], 16), int(key[5:7], 16))
    except ValueError:
        fail(f"invalid color key {key!r}; expected #rrggbb")


def packed_block_state(entry: object, color_key: str) -> int:
    if not isinstance(entry, dict):
        fail(f"palette entry {color_key}: expected object with id and meta")
    if "id" not in entry:
        fail(f"palette entry {color_key}: missing id")
    block_id = entry["id"]
    meta = entry.get("meta", 0)
    if type(block_id) is not int or not 0 <= block_id <= 4095:
        fail(f"palette entry {color_key}: id must be an integer in 0..4095")
    if type(meta) is not int or not 0 <= meta <= 15:
        fail(f"palette entry {color_key}: meta must be an integer in 0..15")
    return (block_id << 4) | meta


def merge_palette_entries(entries: dict[str, object], palette: dict[tuple[int, int, int], int]) -> None:
    for color_key, entry in entries.items():
        rgb = parse_color_key(color_key.lower())
        palette[rgb] = packed_block_state(entry, color_key)


def load_palette(project_dir: pathlib.Path) -> dict[tuple[int, int, int], int]:
    palette: dict[tuple[int, int, int], int] = {}
    merge_palette_entries(DEFAULT_PALETTE, palette)
    palette_path = project_dir / SHOWCASE_PALETTE
    if not palette_path.exists():
        return palette
    try:
        loaded = json.loads(palette_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"{palette_path}: malformed palette JSON: {exc}")
    if not isinstance(loaded, dict):
        fail(f"{palette_path}: palette JSON must be an object")
    merge_palette_entries(loaded, palette)
    return palette


def pixels_to_top_blocks(
    width: int,
    height: int,
    pixels: list[tuple[int, int, int, int]],
    palette: dict[tuple[int, int, int], int],
) -> list[list[int]]:
    expected_pixels = width * height
    actual_pixels = len(pixels)
    if actual_pixels != expected_pixels:
        fail(f"PNG pixel count {actual_pixels} does not match expected {expected_pixels}")

    rows: list[list[int]] = []
    for z in range(height):
        row: list[int] = []
        for x in range(width):
            r, g, b, a = pixels[z * width + x]
            if a != 255:
                fail(f"transparent PNG pixel at x={x} z={z} alpha={a}")
            rgb = (r, g, b)
            if rgb not in palette:
                fail(f"unknown PNG color at x={x} z={z} rgb=#{r:02x}{g:02x}{b:02x}")
            row.append(palette[rgb])
        rows.append(row)
    return rows


def load_showcase_top_blocks(project_dir: pathlib.Path) -> list[list[int]] | None:
    png_path = project_dir / SHOWCASE_PNG
    if not png_path.exists():
        return None
    width, height, pixels = read_png_rgba(png_path)
    if width != MAP_SIZE or height != MAP_SIZE:
        fail(f"{png_path}: expected {MAP_SIZE}x{MAP_SIZE} PNG, got {width}x{height}")
    palette = load_palette(project_dir)
    return pixels_to_top_blocks(width, height, pixels, palette)


def block_for_position(y: int, top_block: int) -> int:
    if y == 0:
        return 7 << 4
    if y <= 3:
        return 3 << 4
    if y == 4:
        return top_block
    return 0


def top_block_for_local(top_blocks: list[list[int]] | None, chunk_x: int, chunk_z: int, local_x: int, local_z: int) -> int:
    if top_blocks is None:
        return 2 << 4
    pixel_x = (chunk_x + 1) * CHUNK_SIZE + local_x
    pixel_z = (chunk_z + 1) * CHUNK_SIZE + local_z
    if not 0 <= pixel_x < MAP_SIZE or not 0 <= pixel_z < MAP_SIZE:
        fail(f"chunk ({chunk_x}, {chunk_z}) maps outside {MAP_SIZE}x{MAP_SIZE} PNG at x={pixel_x} z={pixel_z}")
    return top_blocks[pixel_z][pixel_x]


def chunk_data(top_blocks: list[list[int]] | None, chunk_x: int, chunk_z: int) -> bytes:
    data = bytearray(CHUNK_SECTION_BYTES)
    for y in range(16):
        for z in range(16):
            for x in range(16):
                top_block = top_block_for_local(top_blocks, chunk_x, chunk_z, x, z)
                block = block_for_position(y, top_block)
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


def chunk_body(chunk_x: int, chunk_z: int, top_blocks: list[list[int]] | None = None) -> bytes:
    data = chunk_data(top_blocks, chunk_x, chunk_z)
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


def build_chunks(project_dir: pathlib.Path) -> list[tuple[int, int, bytes, bytes]]:
    top_blocks = load_showcase_top_blocks(project_dir)
    chunks = []
    for index, (chunk_x, chunk_z) in enumerate(SPAWN_CHUNKS):
        raw = chunk_body(chunk_x, chunk_z, top_blocks)
        compressed = zlib.compress(raw, level=9)
        if zlib.decompress(compressed) != raw:
            fail(f"compressed chunk {index} failed roundtrip")
        chunks.append((chunk_x, chunk_z, raw, compressed))
    return chunks


def write_files(out_dir: pathlib.Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    chunks = build_chunks(pathlib.Path.cwd())

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
