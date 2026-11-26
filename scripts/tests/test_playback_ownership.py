import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")


class PlaybackOwnershipTest(unittest.TestCase):
    def test_reset_does_not_access_codec_task_handles(self):
        reset = SOURCE.split("void AudioService::ResetDecoder() {", 1)[1].split("\n}", 1)[0]
        self.assertIn("++playback_generation_", reset)
        self.assertNotIn("opus_decoder_", reset)
        self.assertNotIn("output_resampler_", reset)

    def test_generation_change_clears_decoder_before_decoding_next_packet(self):
        worker = SOURCE.split("void AudioService::OpusCodecTask() {", 1)[1].split("\n}", 1)[0]
        change = worker.index("generation != decoder_generation")
        clear = worker.index("ClosePlaybackDecoder()", change)
        decode = worker.index("DecodePacket(", clear)
        self.assertLess(clear, decode)
