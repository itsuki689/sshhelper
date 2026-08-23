from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter


SIZE = 1024
ROOT = Path(__file__).resolve().parent


def vertical_gradient(top: tuple[int, int, int, int], bottom: tuple[int, int, int, int]) -> Image.Image:
    image = Image.new("RGBA", (SIZE, SIZE))
    pixels = image.load()
    for y in range(SIZE):
        amount = y / (SIZE - 1)
        color = tuple(round(a + (b - a) * amount) for a, b in zip(top, bottom))
        for x in range(SIZE):
            pixels[x, y] = color
    return image


def rounded_mask(box: tuple[int, int, int, int], radius: int) -> Image.Image:
    mask = Image.new("L", (SIZE, SIZE))
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    return mask


def composite_masked(canvas: Image.Image, layer: Image.Image, mask: Image.Image) -> None:
    canvas.alpha_composite(Image.composite(layer, Image.new("RGBA", canvas.size), mask))


def main() -> None:
    canvas = Image.new("RGBA", (SIZE, SIZE))
    body_box = (72, 72, 952, 952)
    body_mask = rounded_mask(body_box, 214)

    shadow_mask = body_mask.filter(ImageFilter.GaussianBlur(30))
    shadow = Image.new("RGBA", canvas.size, (0, 0, 0, 92))
    shifted_shadow = Image.new("L", canvas.size)
    shifted_shadow.paste(shadow_mask, (0, 18))
    composite_masked(canvas, shadow, shifted_shadow)

    body = vertical_gradient((45, 48, 54, 255), (12, 13, 16, 255))
    composite_masked(canvas, body, body_mask)

    rim = Image.new("RGBA", canvas.size)
    rim_draw = ImageDraw.Draw(rim)
    rim_draw.rounded_rectangle(body_box, radius=214, outline=(142, 148, 158, 210), width=18)
    rim_draw.rounded_rectangle((90, 90, 934, 934), radius=196, outline=(255, 255, 255, 35), width=5)
    canvas.alpha_composite(rim)

    up_arrow = [(194, 466), (376, 284), (558, 466), (478, 466),
                (478, 612), (274, 612), (274, 466)]
    down_arrow = [(466, 558), (546, 558), (546, 412), (750, 412),
                  (750, 558), (830, 558), (648, 740)]
    arrows_mask = Image.new("L", canvas.size)
    arrow_draw = ImageDraw.Draw(arrows_mask)
    arrow_draw.polygon(up_arrow, fill=255)
    arrow_draw.polygon(down_arrow, fill=255)

    arrow_glow_mask = arrows_mask.filter(ImageFilter.GaussianBlur(22))
    composite_masked(canvas, Image.new("RGBA", canvas.size, (0, 122, 255, 92)), arrow_glow_mask)
    arrow_fill = vertical_gradient((57, 190, 255, 255), (0, 104, 238, 255))
    composite_masked(canvas, arrow_fill, arrows_mask)

    arrow_edges = Image.new("RGBA", canvas.size)
    edge_draw = ImageDraw.Draw(arrow_edges)
    edge_draw.line(up_arrow + [up_arrow[0]], fill=(142, 226, 255, 210), width=7, joint="curve")
    edge_draw.line(down_arrow + [down_arrow[0]], fill=(90, 207, 255, 190), width=7, joint="curve")
    canvas.alpha_composite(arrow_edges)

    terminal = Image.new("RGBA", canvas.size)
    terminal_draw = ImageDraw.Draw(terminal)
    terminal_draw.line([(486, 485), (520, 519), (486, 553)], fill=(211, 217, 225, 225), width=18, joint="curve")
    terminal_draw.rounded_rectangle((526, 546, 566, 562), radius=8, fill=(211, 217, 225, 205))
    canvas.alpha_composite(terminal)

    png_path = ROOT / "AirSFTP-icon.png"
    ico_path = ROOT / "AirSFTP.ico"
    canvas.save(png_path, "PNG", optimize=True)
    canvas.save(ico_path, "ICO", sizes=[
        (16, 16), (20, 20), (24, 24), (32, 32), (40, 40),
        (48, 48), (64, 64), (128, 128), (256, 256),
    ])
    print(png_path)
    print(ico_path)


if __name__ == "__main__":
    main()