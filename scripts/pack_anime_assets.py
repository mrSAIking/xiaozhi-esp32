"""Pack existing little-endian RGB565 files and emit their C++ manifest index."""
import json
import sys
from pathlib import Path


def initializer(values):
    return "{" + ", ".join(map(str, values)) + "}"


def bank(source):
    pixels = bytearray()
    modes = []
    for name in ("full_body", "half_body"):
        folder = source / name
        spec = json.loads((folder / "manifest.json").read_text())
        if (spec["canvas"], spec["pixel_format"], spec["byte_order"]) != ([320, 480], "RGB565", "little"):
            raise ValueError(f"Unsupported pixel layout in {name}")

        def append(filename, rect):
            x, y, w, h = rect
            if not (0 <= x < x+w <= 320 and 0 <= y < y+h <= 480):
                raise ValueError(f"Rectangle outside canvas: {rect}")
            path = folder / filename
            data = path.read_bytes()
            if path.suffix != ".rgb565" or len(data) != w*h*2:
                raise ValueError(f"Invalid RGB565 data: {path}")
            offset = len(pixels)
            pixels.extend(data)
            return offset

        base = append(spec["base_file"], (0, 0, 320, 480))
        fields = [base]
        for group, states in (("eyes", ("open", "half", "closed")),
                              ("mouth", ("closed", "small", "medium", "wide", "round"))):
            offsets = []
            geometry = None
            for state in states:
                f = spec["frames"][group][state]
                rect = tuple(f[k] for k in ("x", "y", "width", "height"))
                if geometry is not None and geometry != rect:
                    raise ValueError(f"Inconsistent {group} rectangle")
                geometry = rect
                offsets.append(append(f["file"], rect))
            fields.extend((initializer(geometry), initializer(offsets)))
        blink = spec["blink"]
        speech = spec["speech"]
        if blink["sequence"] != ["open", "half", "closed", "half", "open"] or (
            speech["states_by_increasing_level"] != ["closed", "small", "medium", "wide"]
        ):
            raise ValueError("Unsupported facial sequence")
        if len(pixels) - base != spec["production_bytes"]:
            raise ValueError(f"Byte count mismatch for {name}")
        fields.extend((initializer(spec["breathing_region_xywh"]), initializer(blink["hold_ms"]),
                       initializer(blink["interval_ms"]), initializer(f"{v}f" for v in speech["thresholds"]),
                       speech["update_ms"]))
        modes.append(initializer(fields))

    return pixels, modes


def pack(source, output):
    pixels, modes = bank(source)
    white, white_modes = bank(source.parent)
    if modes != white_modes:
        raise ValueError("Background banks must share all animation offsets and geometry")
    stride = len(pixels)
    pixels.extend(white)
    output.mkdir(parents=True, exist_ok=True)
    (output / "companion_pixels.bin").write_bytes(pixels)
    (output / "companion_index.h").write_text(
        '#pragma once\n#include "avatar_math.h"\nnamespace avatar {\n'
        + f'inline constexpr int background_stride = {stride};\n'
        + 'inline constexpr Mode modes[] = {\n' + ',\n'.join(modes) + '\n};\n}\n'
    )


if __name__ == "__main__":
    pack(Path(sys.argv[1]), Path(sys.argv[2]))
