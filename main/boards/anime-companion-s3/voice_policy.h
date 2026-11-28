#pragma once
namespace voice {
constexpr bool Monitor(bool wake, bool interrupt, bool music) {
    return music ? interrupt : wake;
}
constexpr bool Accept(int id, bool wake, bool interrupt, bool music) {
    if (id == 1) return wake && (!music || interrupt);
    return id >= 2 && id <= 4 && interrupt && music;
}
}
