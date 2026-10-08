#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace stb_buddy {

constexpr std::size_t kLircdMaxConfigBytes = 8192;
constexpr std::size_t kLircdMaxRemotes = 8;
constexpr std::size_t kLircdMaxKeys = 256;
constexpr std::size_t kLircdNameBytes = 48;
constexpr std::uint32_t kLircdMinimumCarrierHz = 20'000;
constexpr std::uint32_t kLircdMaximumCarrierHz = 60'000;
constexpr std::uint32_t kLircdMaximumGapUs = 1'000'000;
constexpr std::uint8_t kLircdMaximumDutyPercent = 99;

enum class LircdProtocol : std::uint8_t {
    none,
    space_enc,
    rc5,
};

struct LircdTiming {
    std::uint32_t mark{0};
    std::uint32_t space{0};
};

struct LircdRemote {
    std::array<char, kLircdNameBytes> name{};
    LircdProtocol protocol{LircdProtocol::none};
    bool const_length{false};
    bool repeat_header{false};
    std::uint8_t bits{0};
    std::uint8_t pre_data_bits{0};
    std::uint64_t pre_data{0};
    LircdTiming header{};
    LircdTiming one{};
    LircdTiming zero{};
    LircdTiming repeat{};
    std::uint32_t plead{0};
    std::uint32_t ptrail{0};
    std::uint32_t gap{0};
    std::uint32_t frequency{38000};
    std::uint8_t duty_cycle{33};
    std::uint64_t toggle_bit_mask{0};
    std::uint64_t toggle_state{0};
    std::uint16_t first_key{0};
    std::uint16_t key_count{0};
};

struct LircdKey {
    std::array<char, kLircdNameBytes> name{};
    std::uint64_t code{0};
};

struct LircdDatabase {
    std::array<LircdRemote, kLircdMaxRemotes> remotes{};
    std::array<LircdKey, kLircdMaxKeys> keys{};
    std::size_t remote_count{0};
    std::size_t key_count{0};
};

struct LircdParseError {
    std::size_t line{0};
    std::array<char, 96> message{};
};

class LircdParser final {
public:
    static bool parse(std::string_view input, LircdDatabase* output,
                      LircdParseError* error);
};

const char* protocol_name(LircdProtocol protocol);

}  // namespace stb_buddy
