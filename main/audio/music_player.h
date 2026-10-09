
#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include "esp_audio_simple_dec.h"

class MusicPlayer {
public:
    MusicPlayer();
    ~MusicPlayer();

    // Prepare the MP3 decoder.
    bool Initialize();

    // Release decoder resources.
    void Deinitialize();

    bool IsReady() const;

private:
    esp_audio_simple_dec_handle_t decoder_ = nullptr;
};

#endif // MUSIC_PLAYER_H
