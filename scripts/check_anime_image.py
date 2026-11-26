"""Verify actual flashed binaries against the decoded partition table and merged image."""
import csv
import json
import sys
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
