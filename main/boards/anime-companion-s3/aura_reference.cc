#include "aura_reference.h"
#include "reference_history.h"
#include "audio_codec.h"
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <mutex>
#include <new>
namespace aura_ref {
namespace {
constexpr int rate = 24000;
std::mutex mutex;
ReferenceHistory* ring = nullptr;
int64_t end_frame = 0;
int64_t now_frame() { return esp_timer_get_time() * rate / 1000000; }
}
bool Init() {
    std::lock_guard lock(mutex);
    if (!ring) {
        void* p = heap_caps_malloc(sizeof(ReferenceHistory), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p) ring = new (p) ReferenceHistory();
    }
    return ring != nullptr;
}
void Commit(const int32_t* pcm, size_t n, int output_rate) {
    if (output_rate != rate || !n) return;
    std::lock_guard lock(mutex);
    auto now = now_frame();
    constexpr int ahead = (AUDIO_CODEC_DMA_DESC_NUM - 1) * AUDIO_CODEC_DMA_FRAME_NUM;
    // The accepted block follows earlier writes, bounded by queued DMA capacity.
    auto first = std::clamp(end_frame, now, now + ahead);
    end_frame = first + n;
    if (ring) ring->append(first, pcm, n);
}
bool Drained() {
    std::lock_guard lock(mutex);
    return now_frame() >= end_frame + AUDIO_CODEC_DMA_FRAME_NUM;
}
void Interleave(std::vector<int16_t>& dst, const std::vector<int16_t>& mic) {
    dst.reserve(dst.size() + 2 * mic.size());
    std::lock_guard lock(mutex);
    int64_t half_frame = now_frame() * 2 - 3 * static_cast<int64_t>(mic.size());
    for (auto m : mic) {
        dst.push_back(m);
        dst.push_back(ring->mono16(half_frame));
        half_frame += 3;
    }
}
}
