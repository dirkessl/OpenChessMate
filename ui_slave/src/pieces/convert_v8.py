#!/usr/bin/env python3
"""Convert PNG chess piece images to LVGL v8 C arrays (LV_IMG_CF_TRUE_COLOR_ALPHA)."""

import os, sys
from PIL import Image

PIECES_DIR = os.path.dirname(os.path.abspath(__file__))

TEMPLATE = """/*
 * Auto-generated LVGL v8 image: {name}
 * Source: {filename}
 * Size: {w}x{h}, True color with alpha
 */
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_{NAME}
#define LV_ATTRIBUTE_{NAME}
#endif

static const
LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_{NAME}
uint8_t {name}_map[] = {{
{data}
}};

const lv_img_dsc_t {name} = {{
  .header.always_zero = 0,
  .header.w = {w},
  .header.h = {h},
  .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
  .data_size = {data_size},
  .data = {name}_map,
}};
"""

def png_to_lvgl_v8(png_path, out_path, name):
    img = Image.open(png_path).convert("RGBA")
    w, h = img.size
    pixels = list(img.getdata())
    
    # LVGL v8 TRUE_COLOR_ALPHA: each pixel = B, G, R, A (4 bytes, little-endian RGB565... 
    # Actually for LV_IMG_CF_TRUE_COLOR_ALPHA with LV_COLOR_DEPTH=16:
    # each pixel = 2 bytes color (RGB565 LE) + 1 byte alpha = 3 bytes per pixel
    # For LV_COLOR_DEPTH=32: 4 bytes BGRA
    # ESP32 with TFT_eSPI typically uses 16-bit color.
    # Let's use LV_COLOR_DEPTH 16 (RGB565) + alpha = 3 bytes/pixel
    
    lines = []
    row_bytes = []
    for i, (r, g, b, a) in enumerate(pixels):
        # Convert to RGB565 little-endian
        r5 = (r >> 3) & 0x1F
        g6 = (g >> 2) & 0x3F
        b5 = (b >> 3) & 0x1F
        rgb565 = (r5 << 11) | (g6 << 5) | b5
        lo = rgb565 & 0xFF
        hi = (rgb565 >> 8) & 0xFF
        row_bytes.extend([lo, hi, a])
        
        if (i + 1) % w == 0:
            hex_str = ','.join(f'0x{b:02x}' for b in row_bytes)
            lines.append(f'    {hex_str},')
            row_bytes = []
    
    data_size = w * h * 3  # 3 bytes per pixel (RGB565 + alpha)
    data_str = '\n'.join(lines)
    
    c_code = TEMPLATE.format(
        name=name, NAME=name.upper(), filename=os.path.basename(png_path),
        w=w, h=h, data=data_str, data_size=data_size
    )
    
    with open(out_path, 'w') as f:
        f.write(c_code)
    print(f"  {name}: {w}x{h}, {data_size} bytes -> {out_path}")

def main():
    piece_names = ['wK','wQ','wR','wB','wN','wP','bK','bQ','bR','bB','bN','bP']
    print(f"Converting {len(piece_names)} pieces to LVGL v8 C arrays...")
    for name in piece_names:
        png = os.path.join(PIECES_DIR, f"{name}.png")
        out = os.path.join(PIECES_DIR, f"{name}.c")
        if not os.path.exists(png):
            print(f"  SKIP {name}: {png} not found")
            continue
        png_to_lvgl_v8(png, out, name)
    
    # Generate header file
    header = os.path.join(PIECES_DIR, "pieces.h")
    with open(header, 'w') as f:
        f.write("#pragma once\n")
        f.write("#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include \"lvgl.h\"\n")
        f.write("#else\n#include \"lvgl/lvgl.h\"\n#endif\n\n")
        for name in piece_names:
            f.write(f"extern const lv_img_dsc_t {name};\n")
    print(f"Header -> {header}")
    print("Done!")

if __name__ == "__main__":
    main()
