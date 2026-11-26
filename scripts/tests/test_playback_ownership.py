import os
import shlex
import shutil
import subprocess
import tempfile
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

    def test_decode_checks_vendor_output_before_queueing_pcm(self):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not shutil.which(compiler[0]):
            self.skipTest("requires a host C++ compiler")
        start = SOURCE.index("bool AudioService::DecodePacket(")
        end = SOURCE.index("void AudioService::PushTaskToEncodeQueue", start)
        driver = r"""
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
#define ESP_LOGE(...) ((void)0)
constexpr int ESP_AUDIO_ERR_OK = 0, ESP_AE_ERR_OK = 0, ESP_AUDIO_DEC_RECOVERY_NONE = 0;
using esp_ae_sample_t = void*;
struct AudioStreamPacket { int sample_rate, frame_duration; std::vector<uint8_t> payload; };
struct esp_audio_dec_in_raw_t { uint8_t* buffer; uint32_t len, consumed; int frame_recover; };
struct esp_audio_dec_out_frame_t { uint8_t* buffer; uint32_t len, decoded_size; };
struct esp_audio_dec_info_t {};
struct AudioService {
    bool DecodePacket(AudioStreamPacket&, std::vector<int16_t>&);
    bool SetDecodeSampleRate(int rate, int duration) { return rate == 16000 && duration == 60; }
    int decoder_frame_size_ = 960;
    void* opus_decoder_ = this;
    void* output_resampler_ = this;
};
uint32_t decoded_bytes = 1920, capacity = 1440, produced = 1440;
int sizing_error = 0, processing_error = 0;
int esp_opus_dec_decode(void*, esp_audio_dec_in_raw_t*, esp_audio_dec_out_frame_t* out,
                        esp_audio_dec_info_t*) {
    assert(out->len == 1920);
    out->decoded_size = decoded_bytes;
    return 0;
}
int esp_ae_rate_cvt_get_max_out_sample_num(void*, uint32_t in, uint32_t* out) {
    assert(in == 960);
    *out = capacity;
    return sizing_error;
}
int esp_ae_rate_cvt_process(void*, void*, uint32_t in, void*, uint32_t* out) {
    assert(in == 960 && *out == capacity);
    *out = produced;
    return processing_error;
}
""" + SOURCE[start:end] + r"""
int main() {
    AudioService service;
    AudioStreamPacket packet{16000, 60, {1}};
    std::vector<int16_t> pcm;
    assert(service.DecodePacket(packet, pcm) && pcm.size() == 1440);
    produced = 1410;
    assert(service.DecodePacket(packet, pcm) && pcm.size() == 1410);
    for (auto bytes : {0U, 1919U, 1922U}) {
        decoded_bytes = bytes;
        assert(!service.DecodePacket(packet, pcm));
    }
    decoded_bytes = 1920;
    sizing_error = -1;
    assert(!service.DecodePacket(packet, pcm));
    sizing_error = 0;
    capacity = 0;
    assert(!service.DecodePacket(packet, pcm));
    capacity = 1440;
    processing_error = -1;
    assert(!service.DecodePacket(packet, pcm));
    processing_error = 0;
    produced = 1441;
    assert(!service.DecodePacket(packet, pcm));
    service.output_resampler_ = nullptr;
    assert(service.DecodePacket(packet, pcm) && pcm.size() == 960);
}
"""
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            source = path / "decode_test.cc"
            exe = path / "decode_test"
            source.write_text(driver, encoding="utf-8")
            subprocess.run([*compiler, "-std=c++20", str(source), "-o", str(exe)], check=True)
            subprocess.run([exe], check=True)
