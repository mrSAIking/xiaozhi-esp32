"""Rebuild the registered artwork, display banks, and previews from the supplied portrait."""
import argparse
import json
from math import ceil, floor
from pathlib import Path

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFont
from scipy.interpolate import PchipInterpolator
from scipy.ndimage import map_coordinates

ROOT = Path(__file__).resolve().parents[1]
SIZE = (1200, 2100)
SCALE = 100 / 79
LANCZOS = Image.Resampling.LANCZOS
STATES = {"eyes": ("open", "half", "closed"),
          "mouth": ("closed", "small", "medium", "wide", "round")}
# Source pixel-edge coordinates. Left and right follow the viewer.
FEATURE = {"left_eye": (418, 147, 460, 174), "right_eye": (491, 147, 532, 174),
           "mouth": (457, 207, 492, 225)}
EDIT = {"eyes": [(416, 143, 463, 178), (488, 143, 535, 178)],
        "mouth": [(449, 203, 500, 236)]}
VIEWS = {"full_body": (-100, 0, 1400, 2100), "half_body": (340, 0, 520, 780)}
# x, lower edge of upper lash band, top of lower lid, upper lash thickness.
# Each eye is traced independently, including its distinct canthi and lash weight.
TRACES = [
    [(418,158,158,1), (422,160,160.5,2), (425,160,162,3.5),
     (429,158,165,5), (434,155.6,167.8,5.1), (440,156,168.8,5.9),
     (445,156.7,168.4,5.5), (449,157.7,167.2,4.2), (453,158.8,165.6,2.8),
     (456,161,164.1,1.8), (459.5,164,164,0.8)],
    [(491.4,164,164,0.7), (494.5,160.8,164,1.6), (497.5,158.6,165.5,2.5),
     (501.5,157,167.2,4), (506,156.4,168.5,5.5), (511,155.9,169.2,5.8),
     (516,155.6,168.8,5.3), (520,157.1,167.7,5.3), (524,159.2,164.7,5.1),
     (527,159.7,161.4,3), (531.2,158.3,158.3,1.1)]]
BLINK = ["open", "half", "closed", "half", "open"]
# Time from the first half frame until restoration. Open is the rest pose.
BLINK_MS = [0, 50, 60, 60, 0]


def xywh(box):
    x, y, r, b = box
    return [x, y, r-x, b-y]


def master_box(box):
    return (floor(box[0]*SCALE), floor(box[1]*SCALE),
            ceil(box[2]*SCALE), ceil(box[3]*SCALE))


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def smooth(t):
    t = np.clip(t, 0, 1)
    return t*t*(3-2*t)


def mix(a, b, t):
    return a + (b-a)*t[..., None]


def curve(x, knots, values):
    return PchipInterpolator(knots, values)(np.clip(x, knots[0], knots[-1]))


def sample(rgb, x, y):
    return np.stack([map_coordinates(channel, [y, x], order=1, mode="nearest")
                     for channel in rgb.transpose(2, 0, 1)], axis=-1)


def eye(rgb, x, y, index, closure):
    knots, lash, lid, weight_values = np.array(TRACES[index]).T
    top = curve(x, knots, lash)
    bottom = np.maximum(top, curve(x, knots, lid))
    thick = curve(x, knots, weight_values)
    gap = bottom-top
    lift = gap * 0.11 * closure
    lower = bottom-lift
    moving = top + closure*(lower-top)
    # The original lower lashes rise slightly. The visible iris stays round while
    # the advancing upper lid occludes it. Its corneal highlight follows the eye.
    under = sample(rgb, x, y+lift*np.exp(-((y-lower)/5)**2))
    if closure == 1:
        under = sample(rgb, x, np.maximum(bottom+2.3, y+lift*np.exp(-((y-lower)/5)**2)))
    band = thick*(1-0.40*closure)
    upper = top-thick
    start = upper-4.2
    end = moving-band-2.2
    u = np.clip((y-start)/np.maximum(end-start, 0.5), 0, 1)
    # Borrow skin and shading from the same column just above the lash and
    # below the lower lid. Blend through the lid, retaining the original fold.
    original_fold = sample(rgb, x, start + 2.0*u)
    # Average along the traced fold. A single stretched column can drag
    # individual strands of fringe or lash roots down the closing eyelid.
    skin = np.zeros_like(original_fold)
    for offset, weight in [(-6,.08),(-4,.13),(-2,.18),(0,.22),(2,.18),(4,.13),(6,.08)]:
        nx = x+offset
        ny = curve(nx, knots, lash)-curve(nx, knots, weight_values)-4.0
        skin += sample(rgb,nx,ny)*weight
    skin = mix(original_fold, skin, smooth(u/.35))
    cheek = sample(rgb, x, bottom+6.5)
    skin = mix(skin, cheek, 0.62*np.sin(np.pi*u)**0.9)
    skin += (np.sin(np.pi*u)*closure*2.2)[..., None]
    line_y = top + (y-moving)*thick/band
    line = sample(rgb, x, np.clip(line_y, upper-2.5, top-1.2))
    # A catchlight touches the underside of the original lash at each iris.
    # Use the neighbouring lash pigment when that highlight travels with the lid.
    deep_line = np.minimum(line, sample(rgb, x, np.clip(line_y-1.8, upper+0.7, top-0.8)))
    line = mix(line, deep_line, smooth((line_y-upper-1.4)/1.4))
    eyelid = mix(skin, line, smooth((y-end)/2.1))
    edited = mix(eyelid, under, smooth((y-moving+0.35)/1.05))
    corner = smooth((x-knots[0])/3)*smooth((knots[-1]-x)/3)
    fade = corner*smooth((y-start+0.7)/1.2)*smooth((bottom+6.5-y)/3.5)
    return edited, fade


def mouth(rgb, x, y, state):
    opening, narrow = {"small": (2.6, .98), "medium": (6.4, .94),
                       "wide": (10.8, .92), "round": (10.4, .48)}[state]
    center, radius = 474.1, 17.3
    # The lip corners move in for the O. Relax the inverse warp toward the
    # cheeks, philtrum and chin so the surrounding face stays registered.
    d = abs(x-center)
    inner = radius*narrow
    relax = smooth((y-202.5)/6.2)*smooth((235-y)/8.0)
    dx = np.where(d < inner, d/narrow-d,
                  (radius-inner)*np.clip((25.5-d)/(25.5-inner), 0, 1))
    sx = x + np.sign(x-center)*dx*relax
    t = np.clip((sx-center)/radius, -1, 1)
    seam = curve(sx, [456.8,460.5,465,470.2,474.2,478.4,484,487.8,491.4],
                 [213.75,214.45,214.5,214.15,215.05,214.26,214.62,214.4,213.72])
    profile = np.maximum(0, 1-t*t)**(0.70 if state == "round" else 1.13)
    height = opening*profile
    top = seam-height*(.40 if state == "round" else .25)
    bottom = top+height
    # Move the photographed lip texture with the lip edges, retaining the
    # cupid's bow, lower-lip sheen and original asymmetric commissures.
    sy = np.where(y < top, y+(seam-top)*smooth((y-202.5)/np.maximum(top-202.5,1)),
                  y-(bottom-seam)*smooth((235-y)/np.maximum(235-bottom,1)))
    lips = sample(rgb, sx, sy)
    v = np.clip((y-top)/np.maximum(height, .01), 0, 1)
    depth = np.broadcast_to(np.array([85., 27., 35.]), lips.shape).copy()
    depth += ((1-t*t)*(0.3+0.7*v))[..., None]*np.array([25.,8.,10.])
    tongue = smooth((v-(.58+.26*t*t))/.25)*(1-.4*t*t)
    depth = mix(depth, np.array([184.,85.,90.]), tongue*.86)
    if state in ("medium", "wide"):
        enamel = smooth((.27-.17*t*t-v)/.10)*smooth((1-abs(t))/.50)
        depth = mix(depth, np.array([228.,197.,183.]), enamel*(.83 if state == "wide" else .42))
    coverage = smooth((y-top+.25)/.80)*smooth((bottom-y+.25)/.80)*smooth(height/1.15)
    result = mix(lips, depth, coverage)
    fade = smooth((x-449)/4)*smooth((499-x)/4)*smooth((y-203)/3)*smooth((235-y)/3)
    return result, fade


def make_layer(master, src, group, state):
    layer = Image.new("RGBA", SIZE)
    for idx, box in enumerate(EDIT[group]):
        bounds = master_box(box)
        left, top, right, bottom = bounds
        # Work at 4x master sampling inside each small patch for smooth curves.
        yy, xx = np.mgrid[:(bottom-top)*4, :(right-left)*4]
        x = (left+(xx+.5)/4)/SCALE-.5
        y = (top+(yy+.5)/4)/SCALE-.5
        neutral = state == STATES[group][0]
        if group == "eyes":
            result, fade = eye(src, x, y, idx, .53 if state == "half" else 1)
        else:
            result, fade = mouth(src, x, y, "medium" if neutral else state)
        # Filter the edit in premultiplied alpha to avoid fringes in the master.
        arr = np.uint8(np.clip(np.dstack([result, fade*255]), 0, 255)+.0)
        patch = Image.fromarray(arr).resize((right-left, bottom-top), LANCZOS)
        if neutral:
            exact = master.crop(bounds).convert("RGBA")
            exact.putalpha(patch.getchannel("A"))
            patch = exact
        layer.paste(patch, bounds[:2])
    return layer


def render(im, view):
    x, y, w, h = view
    if x < 0:
        art = Image.new("RGB", (w, h), "white")
        art.paste(im, (-x, -y))
    else:
        art = im.crop((x, y, x+w, y+h))
    return art.resize((320, 480), LANCZOS)


def project(box, view, padding=0):
    x, y, w, h = view
    a, b, c, d = box
    return (floor((a-x)*320/w)-padding, floor((b-y)*480/h)-padding,
            ceil((c-x)*320/w)+padding, ceil((d-y)*480/h)+padding)


def pack(im, order):
    r, g, b = np.asarray(im, dtype=np.uint16).transpose(2, 0, 1)
    data = ((r & 248) << 8) | ((g & 252) << 3) | (b >> 3)
    return data.astype("<u2" if order == "little" else ">u2").tobytes()


def panel_rgb(im):
    a = np.asarray(im)
    r, g, b = a.transpose(2, 0, 1)
    return Image.fromarray(np.stack([(r & 248) | (r >> 5), (g & 252) | (g >> 6),
                                     (b & 248) | (b >> 5)], axis=-1))


def mode_bank(root, mode, master, composites, order):
    folder = root/mode
    folder.mkdir(parents=True, exist_ok=True)
    view = VIEWS[mode]
    base = render(master, view)
    base.save(folder/"base.png")
    (folder/"base.rgb565").write_bytes(pack(base, order))
    panel_rgb(base).save(root/"previews"/(mode+"_preview.png"))
    total = 320*480*2
    face = {key: xywh(project(master_box(box), view)) for key, box in FEATURE.items()}
    breath = (315, 291, 638, 738) if mode == "full_body" else (315, 291, 638, 608)
    frames = {}
    for group, states in STATES.items():
        (folder/group).mkdir(exist_ok=True)
        rendered = {s: render(composites[group+"_"+s], view) for s in states}
        changed = [ImageChops.difference(base, rendered[s]).getbbox() for s in states[1:]]
        rect = (min(b[0] for b in changed), min(b[1] for b in changed),
                max(b[2] for b in changed), max(b[3] for b in changed))
        x, y, w, h = xywh(rect)
        frames[group] = {}
        for state in states:
            name = group+"_"+state
            path = f"{group}/{name}.rgb565"
            patch = rendered[state].crop(rect)
            (folder/path).write_bytes(pack(patch, order))
            frames[group][state] = dict(file=path, x=x, y=y, width=w, height=h)
            total += w*h*2
    write_json(folder/"manifest.json", dict(canvas=[320,480], pixel_format="RGB565", byte_order=order,
        layout="row-major, top-left origin, tightly packed", base_file="base.rgb565",
        master_view_xywh=view, features_xywh=face, frames=frames, production_bytes=total,
        breathing_region_xywh=xywh(project(master_box(breath), view)),
        blink=dict(sequence=BLINK, hold_ms=BLINK_MS, total_ms=170, interval_ms=[2000,6000]),
        speech=dict(input="decoded PCM currently playing on the speaker", update_ms=50,
                    measure="RMS divided by full-scale sample amplitude", thresholds=[.015,.065,.15],
                    states_by_increasing_level=["closed","small","medium","wide"],
                    optional_viseme="round", rest_states=["idle","listening","thinking"],
                    on_playback_end="immediately restore closed")))


def comparison(root, group, composites):
    states = STATES[group]
    cell, margin = 200, 16
    sheet = Image.new("RGB", (margin*2+cell*len(states), 445), "#f4f1ed")
    draw = ImageDraw.Draw(sheet)
    heading = ImageFont.load_default(size=19)
    caption = ImageFont.load_default(size=14)
    draw.text((margin,10), "Eyelid positions" if group == "eyes" else "Speech mouth shapes",
              fill="#3a302d", font=heading)
    source_face = master_box((402, 124, 547, 243))
    for i, state in enumerate(states):
        im = composites[group+"_"+state]
        x = margin+i*cell
        draw.text((x,41), state.capitalize(),font=caption,fill="#5f4941")
        sheet.paste(im.crop(source_face).resize((188,154), LANCZOS), (x,64))
        for j, (mode, view) in enumerate(VIEWS.items()):
            crop = project(source_face,view)
            preview = panel_rgb(render(im,view)).crop(crop)
            preview.thumbnail((188,86),Image.Resampling.NEAREST)
            # Integer scale when it fits, so inspection shows real screen pixels.
            mult = min(188//preview.width,85//preview.height)
            preview = preview.resize((preview.width*max(1,mult),preview.height*max(1,mult)),
                                     Image.Resampling.NEAREST)
            sheet.paste(preview, (x,244+j*99))
        if i == 0:
            draw.text((x,223),"Full-body pixels",fill="#5f4941",font=caption)
            draw.text((x,322),"Half-body pixels",fill="#5f4941",font=caption)
    sheet.save(root/"previews"/("eye_comparison.png" if group=="eyes" else "mouth_comparison.png"))


def gif_preview(path, group, sequence, durations, composites):
    panels = {}
    for state in dict.fromkeys(sequence):
        im = composites[group+"_"+state]
        canvas = Image.new("RGB",(640,480))
        for i, view in enumerate(VIEWS.values()):
            canvas.paste(panel_rgb(render(im,view)),(320*i,0))
        panels[state] = canvas
    # One shared palette keeps static hair, clothes and skin stable frame-to-frame.
    sample_sheet = Image.new("RGB",(640,480*len(panels)))
    for i, frame in enumerate(panels.values()):
        sample_sheet.paste(frame,(0,480*i))
    # Reserve entries for skin, makeup and the small changing mouth cavity.
    # A whole-frame palette otherwise gives most entries to white knit and hair.
    def swatches(sheet, count):
        return sheet.quantize(count, method=Image.Quantize.MEDIANCUT).getpalette()[:3*count]

    face_samples = Image.new("RGB",(128,112*len(panels)))
    lip_samples = Image.new("RGB",(36,28*len(panels)))
    for i, frame in enumerate(panels.values()):
        face_samples.paste(frame.crop((416,91,544,203)),(0,112*i))
        lip_samples.paste(frame.crop((462,155,498,183)),(0,28*i))
    palette = Image.new("P",(1,1))
    palette.putpalette(swatches(sample_sheet,158)+swatches(face_samples,68)+swatches(lip_samples,30))
    frames = {s: im.quantize(palette=palette, dither=Image.Dither.NONE) for s, im in panels.items()}
    first = frames[sequence[0]]
    first.save(path,save_all=True,append_images=[frames[s] for s in sequence[1:]],
               duration=durations,loop=0,disposal=1,optimize=False)


def previews(root, composites):
    for group in STATES:
        comparison(root, group, composites)
    gif_preview(root/"previews/blinking_preview.gif", "eyes", BLINK, [1900,50,60,60,1100], composites)
    # A synthetic voiced PCM sample with syllables, gaps, and a playback gate.
    # Its 50 ms RMS windows drive exactly the thresholds in the manifests.
    rate = 16000
    t = np.arange(2*rate)/rate
    env = sum(level*np.exp(-((t-center)/width)**2) for center,width,level in
              [(.20,.10,.16),(.42,.11,.32),(.69,.09,.11),(1.04,.15,.26),
               (1.29,.085,.14),(1.55,.13,.32),(1.79,.08,.075)])
    pcm = np.int16(32767*env*np.sin(2*np.pi*(148*t+2*t*t)))
    seq, ms = ["closed"], [600]
    for chunk in pcm.reshape(-1,800):
        rms = np.sqrt(np.mean((chunk.astype(float)/32768)**2))
        idx = np.searchsorted([.015,.065,.15], rms, side="right")
        seq.append(("closed","small","medium","wide")[idx])
        ms.append(50)
    gif_preview(root/"previews/talking_preview.gif","mouth",seq+["closed"],ms+[850],composites)


def build(root, reference, order):
    source = root/"source"
    source.mkdir(parents=True, exist_ok=True)
    archive = source/"original.webp"
    with Image.open(reference or archive) as input_art:
        if input_art.size != (948,1659) or input_art.mode != "RGB":
            raise ValueError("Registration requires the supplied 948 x 1659 RGB portrait")
        original = input_art.copy()
    if reference:
        original.save(archive,lossless=True,method=6,exact=True)
    master = original.resize(SIZE, LANCZOS)
    master.save(source/"character_master.png")
    write_json(source/"registration.json",dict(source_size=original.size,master_size=SIZE,
        convention="top-left pixel edges, viewer left and right, [x,y,width,height]",
        original_features_xywh={k:xywh(v) for k,v in FEATURE.items()},
        master_features_xywh={k:xywh(master_box(v)) for k,v in FEATURE.items()},
        original_edit_xywh={k:[xywh(v) for v in boxes] for k,boxes in EDIT.items()},
        master_edit_xywh={k:[xywh(master_box(v)) for v in boxes] for k,boxes in EDIT.items()}))
    preview_dir = root/"previews/composites"
    preview_dir.mkdir(parents=True,exist_ok=True)
    rgb = np.asarray(original,dtype=float)
    composites = {}
    for group, states in STATES.items():
        folder = root/"animations"/group
        folder.mkdir(parents=True,exist_ok=True)
        for state in states:
            name = group+"_"+state
            layer = make_layer(master,rgb,group,state)
            layer.save(folder/(name+".png"))
            composite = Image.composite(layer.convert("RGB"),master,layer.getchannel("A"))
            composite.save(preview_dir/(name+".png"))
            composites[name] = composite
    for mode in VIEWS:
        mode_bank(root,mode,master,composites,order)
    previews(root,composites)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input",type=Path,help="Original supplied PNG. Defaults to the packaged lossless WebP.")
    parser.add_argument("--root",type=Path,default=ROOT)
    parser.add_argument("--byte-order",choices=("little","big"),default="little")
    args = parser.parse_args()
    build(args.root,args.input,args.byte_order)


