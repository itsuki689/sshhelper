from io import BytesIO
from pathlib import Path
import struct

from PIL import Image, ImageDraw, ImageFilter


BASE_SIZE = 1024
ROOT = Path(__file__).resolve().parent
ICO_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def scaled(value: int, size: int) -> int:
    return round(value * size / BASE_SIZE)


def scaled_points(points: list[tuple[int, int]], size: int) -> list[tuple[int, int]]:
    return [(scaled(x, size), scaled(y, size)) for x, y in points]


def vertical_gradient(size: int, top: int, bottom: int) -> Image.Image:
    column = Image.new("RGBA", (1, size))
    column.putdata([
        (round(top + (bottom - top) * y / max(1, size - 1)),) * 3 + (255,)
        for y in range(size)
    ])
    return column.resize((size, size))


def rounded_mask(size: int, box: tuple[int, int, int, int], radius: int) -> Image.Image:
    mask = Image.new("L", (size, size))
    ImageDraw.Draw(mask).rounded_rectangle(box, radius=radius, fill=255)
    return mask


def composite_masked(canvas: Image.Image, layer: Image.Image, mask: Image.Image) -> None:
    canvas.alpha_composite(Image.composite(layer, Image.new("RGBA", canvas.size), mask))


def render_scene(size: int, compact: bool) -> Image.Image:
    canvas = Image.new("RGBA", (size, size))
    body_box = tuple(scaled(value, size) for value in (64, 64, 960, 960))
    body_mask = rounded_mask(size, body_box, scaled(220, size))

    if not compact:
        shadow_mask = body_mask.filter(ImageFilter.GaussianBlur(max(1, scaled(28, size))))
        shifted_shadow = Image.new("L", canvas.size)
        shifted_shadow.paste(shadow_mask, (0, scaled(14, size)))
        composite_masked(canvas, Image.new("RGBA", canvas.size, (0, 0, 0, 88)), shifted_shadow)

    composite_masked(canvas, vertical_gradient(size, 30, 4), body_mask)

    rim = Image.new("RGBA", canvas.size)
    rim_draw = ImageDraw.Draw(rim)
    rim_draw.rounded_rectangle(
        body_box,
        radius=scaled(220, size),
        outline=(142, 142, 142, 230),
        width=max(1, scaled(18, size)),
    )
    if not compact:
        inner_box = tuple(scaled(value, size) for value in (84, 84, 940, 940))
        rim_draw.rounded_rectangle(
            inner_box,
            radius=scaled(202, size),
            outline=(255, 255, 255, 38),
            width=max(1, scaled(5, size)),
        )
    canvas.alpha_composite(rim)

    up_arrow = scaled_points([
        (190, 450), (340, 300), (490, 450), (410, 450),
        (410, 610), (270, 610), (270, 450),
    ], size)
    down_arrow = scaled_points([
        (550, 570), (630, 570), (630, 410), (770, 410),
        (770, 570), (850, 570), (700, 720),
    ], size)
    arrows_mask = Image.new("L", canvas.size)
    arrows_draw = ImageDraw.Draw(arrows_mask)
    arrows_draw.polygon(up_arrow, fill=255)
    arrows_draw.polygon(down_arrow, fill=255)

    if not compact:
        glow_mask = arrows_mask.filter(ImageFilter.GaussianBlur(max(1, scaled(18, size))))
        composite_masked(canvas, Image.new("RGBA", canvas.size, (255, 255, 255, 42)), glow_mask)
    composite_masked(canvas, vertical_gradient(size, 255, 194), arrows_mask)

    edges = Image.new("RGBA", canvas.size)
    edge_draw = ImageDraw.Draw(edges)
    line_width = max(2 if compact else 1, scaled(7, size))
    edge_draw.line(up_arrow + [up_arrow[0]], fill=(255, 255, 255, 230), width=line_width, joint="curve")
    edge_draw.line(down_arrow + [down_arrow[0]], fill=(232, 232, 232, 215), width=line_width, joint="curve")
    canvas.alpha_composite(edges)
    return canvas


def render_icon(size: int) -> Image.Image:
    sample_size = size * 4
    scene = render_scene(sample_size, compact=size <= 32)
    return scene.resize((size, size), Image.Resampling.LANCZOS)


def save_ico(path: Path, images: list[Image.Image]) -> None:
    payloads: list[bytes] = []
    for image in images:
        buffer = BytesIO()
        image.save(buffer, "PNG", optimize=True)
        payloads.append(buffer.getvalue())

    header_size = 6 + 16 * len(images)
    offset = header_size
    directory = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    for image, payload in zip(images, payloads):
        width, height = image.size
        directory.extend(struct.pack(
            "<BBBBHHII",
            0 if width == 256 else width,
            0 if height == 256 else height,
            0,
            0,
            1,
            32,
            len(payload),
            offset,
        ))
        offset += len(payload)
    path.write_bytes(bytes(directory) + b"".join(payloads))


def main() -> None:
    png_path = ROOT / "AirSFTP-icon.png"
    ico_path = ROOT / "AirSFTP.ico"
    render_scene(BASE_SIZE, compact=False).save(png_path, "PNG", optimize=True)
    save_ico(ico_path, [render_icon(size) for size in ICO_SIZES])
    print(png_path)
    print(ico_path)


if __name__ == "__main__":
    main()