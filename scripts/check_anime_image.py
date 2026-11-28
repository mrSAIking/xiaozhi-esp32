"""Verify actual flashed binaries against the decoded partition table and merged image."""
import csv
import json
import sys
import struct
from build_default_assets import read_assets_entries
from pathlib import Path


def number(value):
    value = value.strip().upper()
    if value.endswith(("K", "M")):
        return int(value[:-1], 0) * {"K": 1024, "M": 1024**2}[value[-1]]
    return int(value, 0)


def check(build, table):
    limit = 16 * 1024**2
    slots = {}
    end = 0
    for row in csv.reader(table.read_text().splitlines(), skipinitialspace=True):
        if not row or row[0].startswith("#"):
            continue
        name = row[0].strip()
        start, size = map(number, row[3:5])
        if start < end or start + size > limit:
            raise ValueError(f"Invalid partition {name}")
        slots[name] = (start, size)
        end = start + size

    merged = (build / "merged-binary.bin").read_bytes()
    flashed = json.loads((build / "flasher_args.json").read_text())["flash_files"]
    if len(merged) > limit:
        raise ValueError("Merged firmware exceeds 16 MiB")
    by_offset = {}
    for offset, path in flashed.items():
        start = int(offset, 0)
        data = (build / path).read_bytes()
        if not data or merged[start:start+len(data)] != data:
            raise ValueError(f"Merged firmware is missing {path} at {offset}")
        by_offset[start] = len(data)

    asset_path = next(build / path for offset, path in flashed.items()
                      if int(offset, 0) == slots["assets"][0])
    entries = read_assets_entries(asset_path)
    index = json.loads(entries["index.json"][0])
    bank = entries[index["srmodels"]][0]
    count, = struct.unpack_from("<I", bank)
    names = set()
    offset = 4
    for _ in range(count):
        model, files = struct.unpack_from("<32sI", bank, offset)
        names.add(model.rstrip(b"\0").decode("ascii"))
        offset += 36
        for _ in range(files):
            _, start, length = struct.unpack_from("<32sII", bank, offset)
            if not length or start + length > len(bank):
                raise ValueError("Invalid speech model payload")
            offset += 40
    if names != {"mn6_en", "fst"}:
        raise ValueError(f"Unexpected voice recognition model bank: {names}")
    print(f"- English MultiNet6 and FST: {len(bank):,} bytes")
    app = by_offset[slots["ota_0"][0]]
    assets = by_offset[slots["assets"][0]]
    for name, used in (("ota_0", app), ("ota_1", app), ("assets", assets)):
        start, size = slots[name]
        if used > size:
            raise ValueError(f"{name} overflow")
        print(f"- {name} at 0x{start:x}: {used:,} / {size:,} bytes, {size-used:,} spare")
    print(f"- Merged image: {len(merged):,} bytes within {limit:,} bytes of flash")


if __name__ == "__main__":
    check(Path(sys.argv[1]), Path(sys.argv[2]))
