#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "lircd.hpp"

namespace stb_buddy {

constexpr std::size_t kIrMaxDurations = 256;

struct IrWaveform {
    std::array<std::uint32_t, kIrMaxDurations> durations{};
    std::size_t count{0};
    std::uint32_t signal_us{0};
    std::uint32_t gap_us{0};
};

class IrWaveformBuilder final {
public:
    static bool build(const LircdRemote& remote, std::uint64_t code,
                      bool repeat_frame, IrWaveform* output,
                      const char** error);
};

}  // namespace stb_buddy
