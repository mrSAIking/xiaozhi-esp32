"""Encode nine seconds of desktop poses from the production C++ rasterizer."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

import numpy as np
from PIL import Image, ImageDraw, ImageFont

def generate(exe, bank, target, mode):
    manifest = json.loads((bank / "manifest.json").read_text())
    coords = []
    for group, state in (("eyes", "open"), ("mouth", "closed")):
        patch = manifest["frames"][group][state]
        coords.extend(str(patch[k]) for k in ("x", "y", "width", "height"))
    with tempfile.TemporaryDirectory() as tmp:
        raw = Path(tmp) / "poses.rgb565"
        subprocess.run([exe, bank, str(mode), *coords, raw], check=True)
        frames = []
        with raw.open("rb") as stream:
            while data := stream.read(320 * 480 * 2):
                v = np.frombuffer(data, dtype="<u2").reshape(480, 320).astype(np.uint32)
                rgb = np.stack(((v >> 11)*255//31, ((v >> 5)&63)*255//63, (v&31)*255//31), axis=-1)
                im = Image.fromarray(rgb.astype(np.uint8))
                if not frames:
                    palette = im.quantize(colors=256)
                frames.append(im.quantize(palette=palette, dither=Image.Dither.NONE))
        frames[0].save(target, save_all=True, append_images=frames[1:],
                       duration=40, loop=0, optimize=False, disposal=1)

def subtitles(exe, motion, target):
    # Font metrics come from the desktop font. Positions, text and opacity
    # are emitted by the same caption state machine used on the device.
    try:
        font = ImageFont.truetype("DejaVuSansMono.ttf", 17)
    except OSError:
        font = ImageFont.load_default(size=17)
    advance = round(font.getlength("M"))
    trace = subprocess.check_output([exe, "--track", str(advance)], text=True)
    poses = []
    with Image.open(motion) as gif:
        for n in range(0, gif.n_frames, 2):
            gif.seek(n)
            poses.append(gif.convert("RGB"))
    frames = []
    for row in trace.splitlines():
        ms, x, opacity, answer, line = row.split("\t", 4)
        im = poses[(int(ms)//80) % len(poses)].copy()
        if int(opacity):
            area = im.crop((0,438,320,480))
            strip = Image.new("RGB",area.size,"#101012")
            draw = ImageDraw.Draw(strip)
            for i,char in enumerate(line):
                draw.text((int(x)+i*advance,21),char,font=font,anchor="lm",
                          fill="white" if answer=="1" else "#a8c7fa")
            draw.rectangle((0,0,9,41),fill="#101012")
            draw.rectangle((310,0,319,41),fill="#101012")
            im.paste(Image.blend(area,strip,int(opacity)/255),(0,438))
        frames.append(im.quantize(colors=256))
    frames[0].save(target, save_all=True, append_images=frames[1:], duration=80, loop=0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for mode, name in enumerate(("full_body", "half_body")):
        root = Path(__file__).resolve().parents[1] / "character"
        exe = args.executable.resolve()
        generate(exe, root/"aura"/name, args.output/(name+".gif"), mode)
    subtitles(exe, args.output/"half_body.gif", args.output/"subtitles-black.gif")
    with tempfile.TemporaryDirectory() as tmp:
        white = Path(tmp)/"white.gif"
        generate(exe, root/"half_body", white, 1)
        subtitles(exe, white, args.output/"subtitles-white.gif")

