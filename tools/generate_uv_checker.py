"""英数字付きUVチェッカーを再生成する。必要なパッケージ: Pillow。"""

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=int, default=2048)
    parser.add_argument("--font", type=Path, default=Path("C:/Windows/Fonts/bahnschrift.ttf"))
    parser.add_argument(
        "--output", type=Path,
        default=Path(__file__).resolve().parents[1] / "assets/textures/uv_checker.png",
    )
    args = parser.parse_args()
    if args.size < 256 or args.size % 16:
        parser.error("size must be a multiple of 16 and at least 256")

    # 文字の輪郭を滑らかにし、同じ画像をアプリでもそのまま使用する。
    scale = 2
    cell = args.size * scale // 16
    image = Image.new("RGB", (args.size * scale, args.size * scale))
    draw = ImageDraw.Draw(image)
    font = ImageFont.truetype(str(args.font), round(cell * 0.56))
    for row in range(16):
        for column in range(16):
            x, y = column * cell, row * cell
            light = (column + row) % 2 == 0
            value = 184 if light else 100
            draw.rectangle((x, y, x + cell - 1, y + cell - 1), fill=(value,) * 3)
            # 明るいマスは行名A〜P、暗いマスは列番号1〜16。
            label = chr(ord("A") + row) if light else str(column + 1)
            left, top, right, bottom = draw.textbbox((0, 0), label, font=font)
            draw.text(
                (x + (cell - right + left) / 2 - left,
                 y + (cell - bottom + top) / 2 - top),
                label, font=font, fill=(75,) * 3 if light else (174,) * 3,
            )
    line = max(1, args.size // 1024) * scale
    for i in range(17):
        p = min(i * cell, args.size * scale - 1)
        draw.line((p, 0, p, args.size * scale), fill=(89,) * 3, width=line)
        draw.line((0, p, args.size * scale, p), fill=(89,) * 3, width=line)
    image = image.resize((args.size, args.size), Image.Resampling.LANCZOS)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    image.save(args.output)
    print(args.output)


if __name__ == "__main__":
    main()
