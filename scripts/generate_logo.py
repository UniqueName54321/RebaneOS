#!/usr/bin/env python3
"""Replace the logo mask in the checked-in font atlas."""
import argparse
from pathlib import Path

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logo', type=Path)
    parser.add_argument('atlas', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()

    with Image.open(args.logo) as source:
        source = source.convert('RGBA').resize((480, 116), Image.Resampling.BICUBIC)
        image = Image.new('RGBA', source.size, 'white')
        image.alpha_composite(source)
        pixels = image.convert('RGB').getdata()

    packed = bytearray()
    current = 0
    for index, (red, green, blue) in enumerate(pixels):
        luminance = round((red * 299 + green * 587 + blue * 114) / 1000)
        coverage = min(255, max(0, round((255 - luminance) * 255 / 180)))
        level = round(coverage * 3 / 255)
        current |= level << (6 - (index % 4) * 2)
        if index % 4 == 3:
            packed.append(current)
            current = 0

    rows = [', '.join(f'0x{byte:02X}' for byte in packed[i:i + 16])
            for i in range(0, len(packed), 16)]
    logo = 'static const uint8_t rockos_logo_2bpp[] = {\n    ' + ',\n    '.join(rows) + '\n};'
    atlas = args.atlas.read_text(encoding='utf-8')
    start = atlas.index('static const uint8_t rockos_logo_2bpp[] = {')
    end = atlas.index('};', start) + 2
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(atlas[:start] + logo + atlas[end:], encoding='utf-8')


if __name__ == '__main__':
    main()
