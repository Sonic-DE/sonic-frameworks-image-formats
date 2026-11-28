# SPDX-FileCopyrightText: 2026 Joseph Crowell <joseph.w.crowell@gmail.com>
# SPDX-License-Identifier: LGPL-2.0-or-later
"""Writes the APNG test files and their reference frames into the directory given.

Run: python3 -I generate.py <output directory>   (needs Pillow and ffmpeg)
Each <name>.apng gets <name>_<N>.png, the fully composed frame N as ffmpeg decodes it.
Pillow writes the files but isn't used for the frames: it gets alpha wrong when blending over.
"""

import pathlib
import subprocess
import sys

from PIL import Image, ImageDraw

SIZE = (32, 32)


def solid(colour, mode="RGBA"):
    return Image.new(mode, SIZE, colour)


def square(base, box, colour):
    image = base.copy()
    ImageDraw.Draw(image).rectangle(box, fill=colour)
    return image


def fixtures():
    red, green, blue = (255, 0, 0, 255), (0, 255, 0, 255), (0, 0, 255, 255)
    clear = (0, 0, 0, 0)

    # Three full frames; the IDAT image is frame 0.
    yield "basic", [solid(red), solid(green), solid(blue)], dict(duration=[100, 200, 300], loop=0)

    # A still default image that is not part of the two-frame animation.
    yield "fallback", [solid((128, 128, 128, 255)), solid(red), solid(blue)], dict(
        duration=[100, 100, 100], loop=0, default_image=True)

    # Frame 1 is cleared to transparent black before frame 2 is drawn.
    yield "dispose_background", [
        solid(clear),
        square(solid(clear), (4, 4, 11, 11), green),
        square(solid(clear), (20, 20, 27, 27), blue),
    ], dict(duration=[100, 100, 100], loop=0, disposal=[0, 1, 0], blend=[0, 1, 1])

    # Frame 1 is undone (the canvas goes back to frame 0) before frame 2 is drawn.
    yield "dispose_previous", [
        solid(red),
        square(solid(red), (4, 4, 11, 11), green),
        square(solid(red), (20, 20, 27, 27), blue),
    ], dict(duration=[100, 100, 100], loop=0, disposal=[0, 2, 0], blend=[0, 0, 0])

    # Half-transparent squares blended over what is already there.
    yield "blend_over", [
        solid((255, 255, 255, 255)),
        square(solid(clear), (0, 0, 19, 19), (255, 0, 0, 128)),
        square(solid(clear), (12, 12, 31, 31), (0, 0, 255, 128)),
    ], dict(duration=[100, 100, 100], loop=0, disposal=[0, 0, 0], blend=[0, 1, 1])

    # Palette frames and a finite loop count.
    palette = [solid(c).convert("RGB").quantize(colors=4) for c in (red, green, blue)]
    yield "palette_loop3", palette, dict(duration=[50, 50, 50], loop=3)


def main():
    out = pathlib.Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, frames, options in fixtures():
        path = out / f"{name}.apng"
        frames[0].save(path, format="PNG", save_all=True, append_images=frames[1:], **options)
        # ffmpeg numbers frames from 1 and leaves out a still default image, as APNG requires.
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(path),
                        "-fps_mode", "passthrough", "-pix_fmt", "rgba", str(out / f"{name}_%d.ffmpeg.png")],
                       check=True)
        for frame in sorted(out.glob(f"{name}_*.ffmpeg.png"), key=lambda p: int(p.name.split("_")[-1].split(".")[0])):
            number = int(frame.name.split("_")[-1].split(".")[0]) - 1
            frame.rename(out / f"{name}_{number}.png")


if __name__ == "__main__":
    main()
