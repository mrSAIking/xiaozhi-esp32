"""Independently verify the distributed character files. Requires Pillow and NumPy."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[1]
GROUPS = {"eyes": ["open", "half", "closed"],
          "mouth": ["closed", "small", "medium", "wide", "round"]}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def read(path):
    with Image.open(path) as im:
        return im.copy()


def display(im, view):
    x, y, w, h = view
    if x < 0:
        blank = Image.new("RGB", (w, h), "white")
        blank.paste(im, (-x, -y))
    else:
        blank = im.crop((x, y, x+w, y+h))
    return blank.resize((320, 480), Image.Resampling.LANCZOS)


def words(image):
    rgb = np.asarray(image).astype("uint16")
    return ((rgb[..., 0] >> 3) << 11) | ((rgb[..., 1] >> 2) << 5) | (rgb[..., 2] >> 3)


def validate(root, reference=None):
    original = read(root / "source/original.webp")
    master = read(root / "source/character_master.png")
    require(original.size == (948, 1659) and master.size == (1200, 2100), "source dimensions")
    if reference:
        require(np.array_equal(read(reference), original), "source differs from supplied portrait")
    require(np.array_equal(master, original.resize(master.size, Image.Resampling.LANCZOS)), "master pixels")
    metadata = json.loads((root / "source/registration.json").read_text())
    total = 0
    animated_areas = {"eyes": [], "mouth": []}
    for mode in ("full_body", "half_body"):
        folder = root / mode
        spec = json.loads((folder / "manifest.json").read_text())
        order = spec["byte_order"]
        require(order in ("little", "big"), "unknown byte order")
        dtype = np.dtype("<u2" if order == "little" else ">u2")
        base = read(folder / "base.png")
        view = spec["master_view_xywh"]
        require(spec["canvas"] == [320, 480] and spec["pixel_format"] == "RGB565", "display format")
        require(view[2]*3 == view[3]*2, "display aspect")
        require(np.array_equal(base, display(master, view)), "base transform")
        base_words = words(base)
        require(np.array_equal(np.fromfile(folder / "base.rgb565", dtype), base_words.ravel()), "base encoding")
        used = 320*480*2
        regions = []
        for group, states in GROUPS.items():
            seen = set()
            frames = spec["frames"][group]
            require(list(frames) == states, "state inventory")
            region = None
            for state in states:
                name = group + "_" + state
                overlay = read(root / "animations" / group / (name + ".png"))
                require(overlay.mode == "RGBA" and overlay.size == master.size, name + " overlay format")
                alpha = overlay.getchannel("A")
                mask = Image.new("L", master.size)
                for x, y, w, h in metadata["master_edit_xywh"][group]:
                    mask.paste(255, (x, y, x+w, y+h))
                require(not ImageChops.subtract(alpha, mask).getbbox(), name + " alpha outside masks")
                lo, hi = alpha.getextrema()
                require(lo == 0 and hi == 255 and len(alpha.getcolors(256)) > 2, name + " feather")
                composite = Image.composite(overlay.convert("RGB"), master, alpha)
                diff = ImageChops.difference(master, composite)
                require(not Image.composite(diff, Image.new("RGB", master.size), ImageChops.invert(alpha).point(lambda a: 255 if a == 255 else 0)).getbbox(), name + " changed outside alpha")
                require((diff.getbbox() is None) == (state == states[0]), name + " neutral or edited state")
                require(np.array_equal(read(root / "previews/composites" / (name + ".png")), composite), name + " saved composite")
                rendered = display(composite, view)
                expected = words(rendered)
                f = frames[state]
                x, y, w, h = [f[k] for k in ("x", "y", "width", "height")]
                require(0 <= x < x+w <= 320 and 0 <= y < y+h <= 480, "patch bounds")
                rect = (x, y, w, h)
                require(region in (None, rect), "inconsistent frame geometry")
                region = rect
                data = (folder / f["file"]).read_bytes()
                require(len(data) == w*h*2, name + " raw size")
                patch = np.frombuffer(data, dtype).reshape(h, w)
                decoded = base_words.copy()
                decoded[y:y+h, x:x+w] = patch
                require(np.array_equal(decoded, expected), name + " dirty rectangle reconstruction")
                require(data not in seen, name + " indistinct display state")
                seen.add(data)
                used += len(data)
            regions.append(region)
            x, y, w, h = region
            offset = 0 if mode == "full_body" else 320
            animated_areas[group].append((offset+x, y, offset+x+w, y+h))
        require(regions[0][1]+regions[0][3] <= regions[1][1], "overlapping eye and mouth patches")
        require(used == spec["production_bytes"], "storage accounting")
        total += used
        print(f"{mode}: eight overlays and patch blits verified, {used:,} bytes")
    for name in ("blinking", "talking"):
        with Image.open(root / f"previews/{name}_preview.gif") as gif:
            require(gif.size == (640, 480) and gif.n_frames >= 4, "animation preview")
            initial = gif.convert("RGB")
            group = "eyes" if name == "blinking" else "mouth"
            if name == "blinking":
                require(gif.n_frames == 5, "blink frame count")
            for i in range(gif.n_frames):
                gif.seek(i)
                difference = ImageChops.difference(gif.convert("RGB"), initial)
                if i == gif.n_frames-1:
                    require(not difference.getbbox(), name + " failed to return to rest")
                for rect in animated_areas[group]:
                    difference.paste((0, 0, 0), rect)
                require(not difference.getbbox(), name + " GIF flicker outside patches")
                require(gif.info["duration"] >= 10, "GIF time")
                if name == "blinking" and 1 <= i <= 3:
                    require(gif.info["duration"] == [50,60,60][i-1], "blink timing")
    print(f"PASS. Both production banks total {total:,} bytes.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--reference", type=Path)
    args = parser.parse_args()
    validate(args.root, args.reference)

