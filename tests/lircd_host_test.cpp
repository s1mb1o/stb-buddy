#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include "ir_waveform.hpp"
#include "lircd.hpp"

namespace {

std::string bounded_remote(const std::uint32_t frequency,
                           const unsigned duty_cycle,
                           const std::uint32_t gap) {
    return "begin remote\n"
           "  name BOUNDS\n"
           "  bits 8\n"
           "  flags SPACE_ENC\n"
           "  frequency " +
           std::to_string(frequency) + "\n  duty_cycle " +
           std::to_string(duty_cycle) + "\n  one 560 1690\n"
           "  zero 560 560\n  gap " +
           std::to_string(gap) +
           "\n  begin codes\n    KEY_TEST 0x01\n"
           "  end codes\nend remote\n";
}

bool expect_parse(const std::uint32_t frequency, const unsigned duty_cycle,
                  const std::uint32_t gap, const bool expected,
                  const std::string_view label) {
    const auto input = bounded_remote(frequency, duty_cycle, gap);
    stb_buddy::LircdDatabase database{};
    stb_buddy::LircdParseError error{};
    const bool parsed = stb_buddy::LircdParser::parse(input, &database, &error);
    if (parsed == expected) {
        return true;
    }
    std::cerr << "boundary test " << label << " unexpectedly "
              << (parsed ? "passed" : "failed") << " at line " << error.line
              << ": " << error.message.data() << '\n';
    return false;
}

bool boundary_tests() {
    using namespace stb_buddy;
    return expect_parse(kLircdMinimumCarrierHz, 1, 1, true,
                        "minimum values") &&
           expect_parse(kLircdMaximumCarrierHz,
                        kLircdMaximumDutyPercent, kLircdMaximumGapUs, true,
                        "maximum values") &&
           expect_parse(kLircdMinimumCarrierHz - 1, 33, 100000, false,
                        "carrier below minimum") &&
           expect_parse(kLircdMaximumCarrierHz + 1, 33, 100000, false,
                        "carrier above maximum") &&
           expect_parse(38000, 0, 100000, false, "zero duty cycle") &&
           expect_parse(38000, kLircdMaximumDutyPercent + 1, 100000, false,
                        "duty cycle above maximum") &&
           expect_parse(38000, 33, 0, false, "zero gap") &&
           expect_parse(38000, 33, kLircdMaximumGapUs + 1, false,
                        "gap above maximum");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: lircd_host_test <lircd.conf>...\n";
        return 2;
    }
    if (!boundary_tests()) {
        return 1;
    }

    std::size_t total_remotes = 0;
    std::size_t total_keys = 0;
    for (int argument = 1; argument < argc; ++argument) {
        std::ifstream stream(argv[argument], std::ios::binary);
        const std::string input{std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>()};
        if (!stream.good() && !stream.eof()) {
            std::cerr << argv[argument] << ": cannot read file\n";
            return 1;
        }

        stb_buddy::LircdDatabase database{};
        stb_buddy::LircdParseError parse_error{};
        if (!stb_buddy::LircdParser::parse(input, &database, &parse_error)) {
            std::cerr << argv[argument] << ':' << parse_error.line << ": "
                      << parse_error.message.data() << '\n';
            return 1;
        }

        for (std::size_t remote_index = 0;
             remote_index < database.remote_count; ++remote_index) {
            auto remote = database.remotes[remote_index];
            remote.toggle_state = remote.toggle_bit_mask;
            for (std::size_t key_offset = 0;
                 key_offset < remote.key_count; ++key_offset) {
                const auto& key =
                    database.keys[remote.first_key + key_offset];
                stb_buddy::IrWaveform waveform{};
                const char* waveform_error = nullptr;
                if (!stb_buddy::IrWaveformBuilder::build(
                        remote, key.code, false, &waveform,
                        &waveform_error)) {
                    std::cerr << argv[argument] << ": remote "
                              << remote.name.data() << ", key "
                              << key.name.data() << ": " << waveform_error
                              << '\n';
                    return 1;
                }
                if (remote.repeat.mark != 0 &&
                    !stb_buddy::IrWaveformBuilder::build(
                        remote, key.code, true, &waveform,
                        &waveform_error)) {
                    std::cerr << argv[argument] << ": repeat frame: "
                              << waveform_error << '\n';
                    return 1;
                }
            }
        }
        std::cout << argv[argument] << ": " << database.remote_count
                  << " remote(s), " << database.key_count << " key(s)\n";
        total_remotes += database.remote_count;
        total_keys += database.key_count;
    }

    std::cout << "validated bounds, " << total_remotes << " remotes and "
              << total_keys << " keys\n";
    return 0;
}
