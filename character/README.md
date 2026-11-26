# Xiaozhi companion character assets

Open [the blink preview](previews/blinking_preview.gif) or [the speech preview](previews/talking_preview.gif) to see full-body and waist-up modes side by side. The [eye sheet](previews/eye_comparison.png) and [mouth sheet](previews/mouth_comparison.png) compare every state at master and display scales.

## Artwork and registration

The supplied RGB PNG measures 948 × 1659 (4:7). The lossless source WebP decodes to identical pixels. A single Lanczos enlargement by 100/79 produces the 1200 × 2100 master. All eight RGBA layers use that same full canvas. The eyes-open and mouth-closed composites reproduce the master exactly.

In the original, the exposed face runs approximately from (410, 94) to (539, 251). Dark, center-parted hair frames the forehead and falls into two braids. The almond-shaped eyes have tapered inner corners, heavier outer lashes, brown irises, and small bright catchlights. The closed mouth has a soft cupid’s bow, coral lower lip and a darker central seam. Warm peach shading deepens at the temples and below the jaw. The ivory cable-knit dress, yellow floral bows, pale-blue hem, knit sleeves and boots sit against a softly textured white background.

Representative source samples include brow brown `#351E19` at (433, 137), upper lash `#050302` at (440, 153), lower iris `#BD8474` at (443, 165), skin `#F3CBBC` at (468, 182), upper lip `#EE7571` at (469, 211), lip seam `#93171D` at (470, 214), and lower lip `#F98D8D` at (478, 219).

Each eyelid follows its own traced canthi and lash contour. The upper lid progressively covers the iris and the lower lid rises slightly. Mouth shapes move the original lip texture around a shaded opening. The wider states show a sliver of upper teeth and warmer tongue shading. Feathered local masks confine these edits to the facial features.

All rectangles below use **x, y, width, height**, measured from the top-left corner. Left and right follow the viewer. Feature boxes enclose the neutral painted features. Production patches include the pixels affected by movement and resampling.

| Canvas | Viewer-left eye | Viewer-right eye | Mouth |
|---|---|---|---|
| Original, 948 × 1659 | 418, 147, 42, 27 | 491, 147, 41, 27 | 457, 207, 35, 18 |
| Master, 1200 × 2100 | 529, 186, 54, 35 | 621, 186, 53, 35 | 578, 262, 45, 23 |
| Full body, 320 × 480 | 143, 42, 14, 9 | 164, 42, 13, 9 | 154, 59, 12, 7 |
| Half body, 320 × 480 | 116, 114, 34, 22 | 172, 114, 34, 22 | 146, 161, 29, 15 |

Full-body mode adds 100 white master pixels on either side and scales the resulting 1400 × 2100 canvas to 320 × 480. Half-body mode scales a 520 × 780 window beginning at master coordinate (340, 0). Both transformations preserve proportions. The face is about 2.69 times larger in the waist-up view.

## Display assets

| Mode | Eye patch | Mouth patch | Base bytes | All patch bytes | Total bytes |
|---|---|---|---:|---:|---:|
| Full body | 142, 40, 37, 13 | 151, 57, 18, 14 | 307,200 | 5,406 | 312,606 |
| Half body | 115, 112, 92, 27 | 139, 155, 42, 30 | 307,200 | 27,504 | 334,704 |

**Both production banks occupy 647,310 bytes (632.14 KiB).** Including both supplied JSON manifests adds 5,378 bytes, for 652,688 bytes before filesystem or partition overhead. Only the raw base and eight patch files per mode need to reside in the firmware asset partition if the manifest values are compiled into the application.

Each raw file contains tightly packed row-major RGB565 words. Red occupies bits 15–11, green 10–5 and blue 4–0. The shipped files use little-endian byte order. Draw the base on entering a mode. When an animation state changes, blit its opaque rectangle at the manifest coordinates. The eye and mouth rectangles are disjoint, so both can update independently and in either order. Restore the neutral patch to end an animation. For an ILI9488 configured for 18-bit SPI pixels, the driver unpacks RGB565, expands red and blue to six bits, and transmits three channel bytes per pixel in the panel’s configured color order. Asset endianness controls the layout of the on-disk 16-bit word.

## Animation settings

- **Blink.** Choose a random rest interval between 2 and 6 seconds. From open, display half for 50 ms, closed for 60 ms, half for 60 ms, then return to open. The active blink lasts 170 ms. The GIF loops at a fixed review cadence.
- **Speech.** Evaluate 50 ms blocks of decoded PCM as they reach speaker playback. Compute RMS divided by the full-scale sample amplitude. Below 0.015 select closed, from 0.015 select small, from 0.065 select medium, and from 0.15 select wide. These are starting thresholds for tuning to the eventual voice and gain. Round is a separately addressable O viseme. The talking GIF uses a synthetic voiced PCM envelope with deliberate gaps.
- **Rest.** Idle, listening, thinking, and ended or interrupted playback select closed. Restore closed immediately when speaker playback ends. Gate updates on rendered audio to avoid leading speech by the queued decoder latency.
- **Future breathing.** Each manifest records a suggested torso rectangle for firmware deformation. This package contains static bases and facial animation frames only.

## File inventory

Each brace-enclosed list expands to one file per listed state or mode.

```text
source/original.webp                                   948 × 1659, lossless RGB
source/character_master.png                           1200 × 2100, RGB
source/registration.json                              source/master feature and edit bounds
animations/eyes/eyes_{open,half,closed}.png           1200 × 2100, RGBA
animations/mouth/mouth_{closed,small,medium,wide,round}.png
                                                      1200 × 2100, RGBA

{full_body,half_body}/base.png                        320 × 480, RGB
{full_body,half_body}/base.rgb565                     320 × 480, packed
{full_body,half_body}/eyes/eyes_{open,half,closed}.rgb565
{full_body,half_body}/mouth/mouth_{closed,small,medium,wide,round}.rgb565
{full_body,half_body}/manifest.json                   geometry, files, timing and bytes

previews/{full_body,half_body}_preview.png            320 × 480, RGB565 color preview
previews/eye_comparison.png                           632 × 445
previews/mouth_comparison.png                         1032 × 445
previews/{blinking,talking}_preview.gif               640 × 480, both modes side by side
previews/composites/eyes_{open,half,closed}.png       1200 × 2100, RGB
previews/composites/mouth_{closed,small,medium,wide,round}.png
                                                      1200 × 2100, RGB
scripts/prepare_assets.py
scripts/validate_assets.py
README.md
```

## Rebuild and verify

Run these commands from this package directory using Python with Pillow, NumPy and SciPy. The verified build used Pillow 12.3.0, NumPy 2.5.2 and SciPy 1.18.1. Preparation reads the packaged lossless source by default. An initial import can supply the original portrait using `--input`. To keep a second byte-order variant, write it to another directory.

```shell
python scripts/prepare_assets.py
python scripts/validate_assets.py
python scripts/prepare_assets.py --byte-order big --input source/original.webp --root ../character_big
python scripts/validate_assets.py --root ../character_big
```

Supply `--reference "path/to/supplied.png"` to the validator to compare the lossless source with the original upload. Validation checks the master resize, layer dimensions and transparency, mask confinement, neutral restoration, saved composites, unique RGB565 states, patch coordinates and blits, binary sizes, and GIF rest frames and timing. It also checks decoded GIF frames for changes outside their active patch rectangles.

Both byte-order builds passed. Every production patch reproduced the corresponding whole-screen render pixel for pixel after RGB565 quantization. Visual review covered the full master composites, enlarged face crops, both 320 × 480 layouts and decoded animation frames. The closed-lid shading and the GIF facial color palette were refined during review.

The full-body neutral mouth occupies 12 × 7 display pixels, so quiet speech is subtle at native size. The half-body view gives the mouth 29 × 15 pixels. Physical TFT color, refresh timing and speaker synchronization remain untested on hardware.

