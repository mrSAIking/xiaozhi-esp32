"""Build Project Aura production banks from the original artwork and reviewed foreground matte."""
import json
import shutil
from pathlib import Path
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
MASK = ROOT / "source/aura_foreground.png"

def make_mask():
    # Authoring only. Use connected exterior and the two enclosed arm gaps as
    # background seeds. White fabric enclosed by its outline stays foreground.
    import cv2
    from scipy.ndimage import distance_transform_edt
    rgb = np.asarray(Image.open(ROOT / "source/character_master.png"))
    neutral = (rgb.min(2) > 235) & (np.ptp(rgb, axis=2) < 12)
    _, labels = cv2.connectedComponents(neutral.astype(np.uint8))
    exterior = np.isin(labels, [labels[0, 0], labels[680, 732], labels[681, 463]])
    _, cc, stats, _ = cv2.connectedComponentsWithStats((~exterior).astype(np.uint8))
    fg = (cc == 1 + np.argmax(stats[1:, -1])).astype(np.uint8)
    # Estimate local foreground color just inside the contour and solve the
    # white-matte equation in the narrow transition band.
    core = cv2.erode(fg, np.ones((5, 5), np.uint8))
    edge = cv2.dilate(fg, np.ones((5, 5), np.uint8)).astype(bool) & ~core.astype(bool)
    indices = distance_transform_edt(1-core, return_distances=False, return_indices=True)
    dark = 255.0 - rgb[tuple(indices[:, edge])].astype(float)
    observed = 255.0 - rgb[edge].astype(float)
    alpha = core*255
    alpha[edge] = np.rint(255*np.clip((observed*dark).sum(1)/np.maximum((dark*dark).sum(1), 1), 0, 1))
    # Resolve enclosed white gaps between wisps. Around the head the local
    # foreground is dark hair; recover its coverage against the white paper.
    hair = rgb[:335]
    near_white = ((hair.min(2)>218) & (np.ptp(hair,axis=2)<20)).astype(np.uint8)
    fringe = cv2.dilate(near_white, np.ones((13,13), np.uint8)).astype(bool)
    yy, xx = np.indices(fringe.shape)
    fringe &= (xx<490) | (xx>711) | (yy<140)
    dark_core = hair.min(2)<108
    ix = distance_transform_edt(~dark_core, return_distances=False, return_indices=True)
    color = 255.0-hair[tuple(ix[:,fringe])].astype(float)
    obs = 255.0-hair[fringe].astype(float)
    cov = np.clip((obs*color).sum(1)/np.maximum((color*color).sum(1),1),0,1)
    alpha[:335][fringe] = np.minimum(alpha[:335][fringe], np.rint(255*cov).astype(np.uint8))
    Image.fromarray(alpha).save(MASK)

def black_master():
    rgb = np.asarray(Image.open(ROOT / "source/character_master.png"), dtype=np.int16)
    alpha = np.asarray(Image.open(MASK), dtype=np.int16)
    result = np.maximum(0, rgb - (255-alpha[..., None])).astype(np.uint8)
    result[alpha == 0] = 0
    return Image.fromarray(result)

def project(im, view):
    x, y, w, h = view
    return im.crop((x, y, x+w, y+h)).resize((320, 480), Image.Resampling.LANCZOS)

def encode(im):
    a = np.asarray(im, dtype=np.uint16)
    return (((a[:,:,0]>>3)<<11) | ((a[:,:,1]>>2)<<5) | (a[:,:,2]>>3)).astype("<u2").tobytes()

def banks(check=False):
    master = black_master()
    for name in ("full_body", "half_body"):
        old, new = ROOT/name, ROOT/"aura"/name
        spec = json.loads((old/"manifest.json").read_text())
        view = spec["master_view_xywh"]
        base = project(master, view)
        if not check:
            new.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(old/"manifest.json", new/"manifest.json")
            base.save(new/"base.png")
        assert np.array_equal(np.asarray(base), np.asarray(Image.open(new/"base.png")))
        assert base.getpixel((0,0)) == (0,0,0)
        def write(path, data):
            if not check:
                path.parent.mkdir(exist_ok=True)
                path.write_bytes(data)
            assert path.read_bytes() == data
        write(new/"base.rgb565", encode(base))
        for group, states in spec["frames"].items():
            for state, f in states.items():
                overlay = Image.open(ROOT/"animations"/group/(group+"_"+state+".png"))
                composed = Image.composite(overlay.convert("RGB"), master, overlay.getchannel("A"))
                x,y,w,h = [f[k] for k in ("x","y","width","height")]
                rendered = project(composed,view)
                patch = rendered.crop((x,y,x+w,y+h))
                restored = base.copy()
                restored.paste(patch,(x,y))
                assert encode(restored) == encode(rendered)
                write(new/f["file"], encode(patch))
if __name__ == "__main__":
    import sys
    if "--mask" in sys.argv:
        make_mask()
    banks("--check" in sys.argv)

