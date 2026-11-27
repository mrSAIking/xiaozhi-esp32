import gzip
import unittest
from pathlib import Path
from PIL import Image
import numpy as np

WEB = Path(__file__).resolve().parents[2] / 'main/boards/anime-companion-s3/web'

class AuraPortalAssets(unittest.TestCase):
    def test_embedded_page_matches_source(self):
        self.assertEqual(gzip.decompress((WEB/'aura_portal.html.gz').read_bytes()), (WEB/'portal.html').read_text(encoding='utf-8').encode())

    def test_boot_pixels_and_preview_match(self):
        image = np.asarray(Image.open(WEB/'boot-preview.png'), dtype=np.uint16)
        self.assertEqual(image.shape, (480,320,3))
        words = ((image[:,:,0]>>3)<<11) | ((image[:,:,1]>>2)<<5) | (image[:,:,2]>>3)
        self.assertEqual(words.astype('<u2').tobytes(), (WEB/'aura_boot.rgb565').read_bytes())

