"""Encode nine seconds of desktop poses from the production C++ rasterizer."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

import numpy as np
from PIL import Image

def generate(exe, root, out, mode, name):
    bank = root / "character" / "aura" / name
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
        frames[0].save(out / (name + ".gif"), save_all=True, append_images=frames[1:],
                       duration=40, loop=0, optimize=False, disposal=1)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for mode, name in enumerate(("full_body", "half_body")):
        generate(args.executable.resolve(), Path(__file__).resolve().parents[1], args.output, mode, name)

