
#ifndef MUSIC_HTTP_STREAMER_H
#define MUSIC_HTTP_STREAMER_H

#include <string>

class MusicPlayer;
class AudioService;

class MusicHttpStreamer {
public:
    // Download and play a song from:
    // https://github.com/mrSAIking/music
    //
    // Example:
    // StreamSong("funksong", player, audio_service)
    //
    // This function BLOCKS while streaming.
    // It must run inside a dedicated FreeRTOS worker task,
    // not the main application or MCP callback task.
    static bool StreamSong(
        const std::string& song_name,
        MusicPlayer& player,
        AudioService& audio_service
    );
};

#endif // MUSIC_HTTP_STREAMER_H
