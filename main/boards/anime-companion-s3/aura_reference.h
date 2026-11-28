#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
namespace aura_ref {
bool Init();
void Commit(const int32_t* pcm, size_t frames, int rate);
void Interleave(std::vector<int16_t>& destination, const std::vector<int16_t>& microphone);
bool Drained();
}
