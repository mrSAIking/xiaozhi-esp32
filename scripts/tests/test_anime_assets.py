import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class AvatarAssetsTest(unittest.TestCase):
    def test_pack_matches_both_manifests_byte_for_byte(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            subprocess.run([sys.executable, str(ROOT / "scripts/pack_anime_assets.py"),
                            str(ROOT / "character" / "aura"), str(out)], check=True)
            expected = bytearray()
            for mode in ("full_body", "half_body"):
                directory = ROOT / "character" / "aura" / mode
                manifest = json.loads((directory / "manifest.json").read_text())
                expected.extend((directory / manifest["base_file"]).read_bytes())
                for group in ("eyes", "mouth"):
                    for frame in manifest["frames"][group].values():
                        expected.extend((directory / frame["file"]).read_bytes())
            self.assertEqual(len(expected), 647310)
            white = bytearray()
            for mode in ("full_body", "half_body"):
                directory = ROOT / "character" / mode
                manifest = json.loads((directory / "manifest.json").read_text())
                white.extend((directory / manifest["base_file"]).read_bytes())
                for group in ("eyes", "mouth"):
                    for frame in manifest["frames"][group].values():
                        white.extend((directory / frame["file"]).read_bytes())
            self.assertEqual(len(white), len(expected))
            self.assertEqual((out / "companion_pixels.bin").read_bytes(), expected + white)
