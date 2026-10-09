
#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "esp_audio_simple_dec.h"
#include "esp_ae_rate_cvt.h"

class AudioService;

class MusicPlayer {
public:
    MusicPlayer();
    ~MusicPlayer();

    // Initialize the MP3 decoder.
    bool Initialize();

    // Reset before decoding a new MP3 song.
    bool ResetForNewSong();

    // Feed one chunk of MP3 bytes to the decoder.
    // The decoder keeps its state between calls.
    //
    // Audio output:
    // MP3 -> PCM -> mono -> 24000 Hz -> AudioService
    //
    // Call this from a dedicated playback worker,
    // not directly from the main application task.
    bool DecodeChunk(
        const uint8_t* data,
        size_t length,
        AudioService& audio_service,
        bool end_of_stream = false
    );

    void Deinitialize();

    bool IsReady() const;

private:
    static constexpr int kSpeakerSampleRate = 24000;

    esp_audio_simple_dec_handle_t decoder_ = nullptr;
    esp_ae_rate_cvt_handle_t resampler_ = nullptr;

    int resampler_input_rate_ = 0;

    bool PrepareResampler(int source_rate);

    bool ProcessDecodedPcm(
        const uint8_t* data,
        size_t length,
        AudioService& audio_service
    );

    void CloseResampler();
};

#endif // MUSIC_PLAYER_H
