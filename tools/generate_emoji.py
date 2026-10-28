#!/usr/bin/env python3
# Generate main/tab5_emoji_dados.cpp with every emoji consisting of ONE code point—
# 1,391 in Unicode 17.0, 1,357 with artwork in Twemoji. Twemoji artwork (twemoji.twitter.com,
# CC-BY 4.0). Run once; after that it works offline—the firmware never downloads anything
# at runtime, only using the converted bytes this script stores in the repository.
#
#   pip3 install --user Pillow
#   ./generate_emoji.py
#
# "ONE CODE POINT": the variation selector (U+FE0F) does NOT count—the display
# library discards it before any emoji reaches the callback (see LGFXBase.cpp).
# Excluded are flags (regional indicator pairs), skin tones, and ZWJ sequences
# (families, professions)—each combines SEVERAL code points into one image, while
# LovyanGFX invokes one callback PER CODE POINT. It is possible to support them,
# but that is a separate project: combine N callbacks into one composite image.
#
# PALETTE FORMAT: each emoji stores its colors (at most 256; none exceeds 237)
# and a 1-byte-per-pixel index—0.95 MB instead of 1.49 MB for raw RGB565, with
# the same pixel-by-pixel image. Index 0 is always the transparent background.
#
# See main/tab5_emoji.hpp for the generated table format and main/tab5_emoji.cpp
# for the callback that consumes it (BINARY search—the table must remain SORTED
# by code point, and this script guarantees that).

import os
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed

SIZE = 24
SENTINEL = 0xF81F  # pure magenta RGB565; never appears in a real emoji
SOURCE_URL = "https://unicode.org/Public/emoji/latest/emoji-test.txt"
IMG_BASE = "https://raw.githubusercontent.com/twitter/twemoji/master/assets/72x72"

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(PROJECT_ROOT)   # the project root; this script lives in tools/
CACHE_DIR = os.path.join(PROJECT_ROOT, ".emoji_cache")
OUTPUT_PATH = os.path.join(PROJECT_ROOT, "main", "tab5_emoji_dados.cpp")

LINE_PATTERN = re.compile(
    r'^([0-9A-Fa-f ]+);\s*(\S+)\s*#\s*(\S+)\s*E[\d.]+\s*(.*)$'
)


def fetch_official_list():
    """Return [(code_point, English_name), ...] sorted by code point—only
    single-code-point, fully-qualified emoji (the canonical "complete" form;
    minimally-qualified and unqualified forms are variants of the same emoji)."""
    response = subprocess.run(["curl", "-sf", SOURCE_URL], capture_output=True, text=True, check=True)
    entries = {}
    for line in response.stdout.splitlines():
        if not line or line.startswith('#'):
            continue
        m = LINE_PATTERN.match(line)
        if not m:
            continue
        code_text, status, _emoji_char, name = m.groups()
        if status != 'fully-qualified':
            continue
        code_points = [code for code in code_text.strip().split(' ') if code.upper() != 'FE0F']
        if len(code_points) != 1:
            continue   # flag, skin tone, or ZWJ sequence—outside the scope
        code_point = int(code_points[0], 16)
        entries[code_point] = name.strip()
    return sorted(entries.items())


def download_one(code_point):
    # curl rather than urllib: this Mac's Python lacks a configured root
    # certificate (a common python.org installer issue), while curl uses the
    # system certificate, which already works.
    file_name = f"{code_point:x}"
    file_path = os.path.join(CACHE_DIR, f"{file_name}.png")
    if os.path.exists(file_path):
        return code_point, file_path, None
    url = f"{IMG_BASE}/{file_name}.png"
    r = subprocess.run(["curl", "-sf", "-o", file_path, url])
    if r.returncode != 0:
        if os.path.exists(file_path):
            os.remove(file_path)   # curl may leave an empty/partial file on failure
        return code_point, None, f"HTTP failed for U+{code_point:04X}"
    return code_point, file_path, None


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def swap_bytes(value):
    # LovyanGFX reads a uint16_t image as RGB565 with the bytes SWAPPED
    # (swap565_t) when setSwapBytes is disabled, as it is in this system.
    # Writing the bytes already swapped produces the correct color without
    # changing the display state.
    return ((value & 0xFF) << 8) | (value >> 8)


def convert_image(file_path):
    from PIL import Image
    img = Image.open(file_path).convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
    px = img.load()
    values = []
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = px[x, y]
            if a < 128:
                v = SENTINEL
            else:
                v = rgb565(r, g, b)
                if v == SENTINEL:
                    v ^= 0x0001   # never collide with the actual sentinel
            values.append(swap_bytes(v))
    return values


def make_palette(values):
    """Convert swapped RGB565 pixels to (palette, indices). The sentinel becomes
    index 0—the drawing's transparent color—and other colors are added in order
    of appearance."""
    sentinel = swap_bytes(SENTINEL)
    palette = [sentinel]
    positions = {sentinel: 0}
    pixel_indices = []
    for value in values:
        if value not in positions:
            positions[value] = len(palette)
            palette.append(value)
        pixel_indices.append(positions[value])
    if len(palette) > 256:
        raise ValueError(f"{len(palette)} colors do not fit in a 1-byte index")
    return palette, pixel_indices


def write_output(items):
    """items = [(code_point, name, swapped RGB565 pixels), ...] sorted by
    code point. Write OUTPUT_PATH."""
    lines = [
        "// GENERATED by generate_emoji.py from the official Unicode emoji list",
        "// (emoji-test.txt) and Twemoji artwork (CC-BY 4.0, twemoji.twitter.com).",
        "// Do not edit by hand—run the script again.",
        "//",
        "// INTENTIONALLY SORTED BY CODE POINT: tab5_emoji.cpp uses binary search,",
        "// not a scan; with more than a thousand entries, this matters.",
        "//",
        "// Each emoji: an RGB565 palette (swapped bytes, index 0 = transparent)",
        "// and a 1-byte-per-pixel index.",
        '#include "tab5_emoji.hpp"',
        "",
    ]
    table = []
    total = 0
    for code_point, name, values in items:
        palette, pixel_indices = make_palette(values)
        pixel_array = f"s_emoji_{code_point:x}"
        palette_array = f"s_paleta_{code_point:x}"
        lines.append(f"// U+{code_point:04X} - {name}")
        lines.append(f"static const uint16_t {palette_array}[{len(palette)}] = {{")
        for k in range(0, len(palette), 16):
            lines.append("  " + ",".join(f"0x{value:04X}" for value in palette[k:k + 16]) + ",")
        lines.append("};")
        lines.append(f"static const uint8_t {pixel_array}[{SIZE}*{SIZE}] = {{")
        for k in range(0, len(pixel_indices), 24):
            lines.append("  " + ",".join(str(value) for value in pixel_indices[k:k + 24]) + ",")
        lines.append("};")
        table.append((code_point, pixel_array, palette_array))
        total += len(pixel_indices) + 2 * len(palette)

    lines.append("")
    lines.append(f"const int TAB5_EMOJI_LADO = {SIZE};")
    lines.append("")
    lines.append("// SORTED by code point—do not shuffle. See the notice above.")
    lines.append("const tab5_emoji_t TAB5_EMOJI_TABELA[] = {")
    for code_point, pixel_array, palette_array in table:
        lines.append(f"  {{ 0x{code_point:X}, {pixel_array}, {palette_array} }},")
    lines.append("};")
    lines.append(f"const int TAB5_EMOJI_N = {len(table)};")

    with open(OUTPUT_PATH, "w") as output_file:
        output_file.write("\n".join(lines) + "\n")

    print(f"wrote {OUTPUT_PATH}: {len(table)} emoji, {SIZE}x{SIZE} palette, "
          f"{total} bytes ({total/1024/1024:.2f} MB)")


def main():
    os.makedirs(CACHE_DIR, exist_ok=True)

    print("fetching the official emoji list (unicode.org)...")
    emoji_list = fetch_official_list()
    print(f"{len(emoji_list)} single-code-point emoji")

    print("downloading (20 at a time, cached in .emoji_cache/)...")
    paths = {}
    failures = []
    with ThreadPoolExecutor(max_workers=20) as ex:
        futures = [ex.submit(download_one, code_point) for code_point, _ in emoji_list]
        completed = 0
        for future in as_completed(futures):
            code_point, file_path, error = future.result()
            completed += 1
            if error:
                failures.append((code_point, error))
            else:
                paths[code_point] = file_path
            if completed % 200 == 0:
                print(f"  {completed}/{len(emoji_list)}...")

    if failures:
        print(f"WARNING: {len(failures)} missing Twemoji images (skipped):")
        for code_point, error in failures[:20]:
            print(f"  U+{code_point:04X}: {error}")
        if len(failures) > 20:
            print(f"  ... and {len(failures) - 20} more")

    items = []
    for index, (code_point, name) in enumerate(emoji_list):
        file_path = paths.get(code_point)
        if not file_path:
            continue
        items.append((code_point, name, convert_image(file_path)))
        if (index + 1) % 200 == 0:
            print(f"  converted {index + 1}/{len(emoji_list)}...")

    write_output(items)


if __name__ == "__main__":
    main()
