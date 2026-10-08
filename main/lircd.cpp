#include "lircd.hpp"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace stb_buddy {

namespace {

constexpr std::size_t kLineBytes = 256;

enum class ParseState {
    top,
    remote,
    codes,
};

bool fail(LircdParseError* error, const std::size_t line,
          const char* message) {
    if (error != nullptr) {
        error->line = line;
        std::snprintf(error->message.data(), error->message.size(), "%s",
                      message);
    }
    return false;
}

bool parse_u64(const char* text, std::uint64_t* value) {
    if (text == nullptr || value == nullptr || *text == '\0' || *text == '-') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = parsed;
    return true;
}

bool parse_u32(const char* text, std::uint32_t* value) {
    std::uint64_t parsed = 0;
    if (!parse_u64(text, &parsed) ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool copy_name(const char* source,
               std::array<char, kLircdNameBytes>* destination) {
    if (source == nullptr || destination == nullptr) {
        return false;
    }
    const auto length = std::strlen(source);
    if (length == 0 || length >= destination->size()) {
        return false;
    }
    std::memcpy(destination->data(), source, length + 1);
    return true;
}

std::size_t tokenize(char* line, char** tokens, const std::size_t maximum) {
    std::size_t count = 0;
    char* cursor = line;
    while (*cursor != '\0') {
        while (std::isspace(static_cast<unsigned char>(*cursor)) != 0) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        if (count == maximum) {
            return maximum + 1;
        }
        tokens[count++] = cursor;
        while (*cursor != '\0' &&
               std::isspace(static_cast<unsigned char>(*cursor)) == 0) {
            ++cursor;
        }
        if (*cursor != '\0') {
            *cursor++ = '\0';
        }
    }
    return count;
}

bool parse_timing(char** tokens, const std::size_t count,
                  LircdTiming* timing) {
    return count == 3 && parse_u32(tokens[1], &timing->mark) &&
           parse_u32(tokens[2], &timing->space) && timing->mark > 0 &&
           timing->space > 0 && timing->mark <= 32767 &&
           timing->space <= 32767;
}

bool parse_flags(char* value, LircdRemote* remote) {
    bool found_protocol = false;
    char* save = nullptr;
    for (char* flag = strtok_r(value, "|", &save); flag != nullptr;
         flag = strtok_r(nullptr, "|", &save)) {
        if (std::strcmp(flag, "SPACE_ENC") == 0) {
            if (found_protocol) {
                return false;
            }
            remote->protocol = LircdProtocol::space_enc;
            found_protocol = true;
        } else if (std::strcmp(flag, "RC5") == 0 ||
                   std::strcmp(flag, "SHIFT_ENC") == 0) {
            if (found_protocol) {
                return false;
            }
            remote->protocol = LircdProtocol::rc5;
            found_protocol = true;
        } else if (std::strcmp(flag, "CONST_LENGTH") == 0) {
            remote->const_length = true;
        } else if (std::strcmp(flag, "REPEAT_HEADER") == 0) {
            remote->repeat_header = true;
        } else {
            return false;
        }
    }
    return found_protocol;
}

std::uint64_t bit_mask(const unsigned bits) {
    return bits >= 64 ? std::numeric_limits<std::uint64_t>::max()
                      : ((std::uint64_t{1} << bits) - 1);
}

bool finish_remote(LircdRemote* remote, LircdParseError* error,
                   const std::size_t line) {
    if (remote->name[0] == '\0' || remote->protocol == LircdProtocol::none ||
        remote->bits == 0 || remote->one.mark == 0 ||
        remote->zero.mark == 0 || remote->gap == 0 ||
        remote->key_count == 0) {
        return fail(error, line, "incomplete remote block");
    }
    const unsigned all_bits = remote->pre_data_bits + remote->bits;
    if (all_bits > 64) {
        return fail(error, line, "pre_data_bits + bits exceeds 64");
    }
    if ((remote->pre_data & ~bit_mask(remote->pre_data_bits)) != 0) {
        return fail(error, line, "pre_data does not fit pre_data_bits");
    }
    if ((remote->toggle_bit_mask & ~bit_mask(all_bits)) != 0) {
        return fail(error, line, "toggle_bit_mask exceeds frame width");
    }
    if (remote->toggle_bit_mask != 0 &&
        (remote->toggle_bit_mask & (remote->toggle_bit_mask - 1)) != 0) {
        return fail(error, line, "multi-bit toggle masks are unsupported");
    }
    return true;
}

bool parse_remote_field(char** tokens, const std::size_t count,
                        LircdRemote* remote, unsigned* toggle_bit) {
    const char* key = tokens[0];
    if (std::strcmp(key, "name") == 0) {
        return count == 2 && copy_name(tokens[1], &remote->name);
    }
    if (std::strcmp(key, "flags") == 0) {
        return count == 2 && parse_flags(tokens[1], remote);
    }
    if (std::strcmp(key, "header") == 0) {
        return parse_timing(tokens, count, &remote->header);
    }
    if (std::strcmp(key, "one") == 0) {
        return parse_timing(tokens, count, &remote->one);
    }
    if (std::strcmp(key, "zero") == 0) {
        return parse_timing(tokens, count, &remote->zero);
    }
    if (std::strcmp(key, "repeat") == 0) {
        return parse_timing(tokens, count, &remote->repeat);
    }
    std::uint64_t value = 0;
    if (count != 2 || !parse_u64(tokens[1], &value)) {
        return false;
    }
    if (std::strcmp(key, "bits") == 0 && value >= 1 && value <= 64) {
        remote->bits = static_cast<std::uint8_t>(value);
    } else if (std::strcmp(key, "pre_data_bits") == 0 && value <= 64) {
        remote->pre_data_bits = static_cast<std::uint8_t>(value);
    } else if (std::strcmp(key, "pre_data") == 0) {
        remote->pre_data = value;
    } else if (std::strcmp(key, "plead") == 0 && value <= 32767) {
        remote->plead = static_cast<std::uint32_t>(value);
    } else if (std::strcmp(key, "ptrail") == 0 && value <= 32767) {
        remote->ptrail = static_cast<std::uint32_t>(value);
    } else if (std::strcmp(key, "gap") == 0 && value >= 1 &&
               value <= kLircdMaximumGapUs) {
        remote->gap = static_cast<std::uint32_t>(value);
    } else if (std::strcmp(key, "toggle_bit_mask") == 0) {
        remote->toggle_bit_mask = value;
    } else if (std::strcmp(key, "toggle_bit") == 0 && value <= 64) {
        *toggle_bit = static_cast<unsigned>(value);
    } else if (std::strcmp(key, "frequency") == 0 &&
               value >= kLircdMinimumCarrierHz &&
               value <= kLircdMaximumCarrierHz) {
        remote->frequency = static_cast<std::uint32_t>(value);
    } else if (std::strcmp(key, "duty_cycle") == 0 && value >= 1 &&
               value <= kLircdMaximumDutyPercent) {
        remote->duty_cycle = static_cast<std::uint8_t>(value);
    } else if (std::strcmp(key, "eps") == 0 ||
               std::strcmp(key, "aeps") == 0) {
        // Receive-only tolerances are accepted for file compatibility.
    } else {
        return false;
    }
    return true;
}

}  // namespace

bool LircdParser::parse(const std::string_view input, LircdDatabase* output,
                        LircdParseError* error) {
    if (output == nullptr || input.empty() ||
        input.size() > kLircdMaxConfigBytes) {
        return fail(error, 0, "configuration size is invalid");
    }
    output->remote_count = 0;
    output->key_count = 0;
    for (auto& remote_entry : output->remotes) {
        remote_entry = LircdRemote{};
    }
    for (auto& key_entry : output->keys) {
        key_entry = LircdKey{};
    }
    if (error != nullptr) {
        *error = LircdParseError{};
    }

    ParseState state = ParseState::top;
    LircdRemote* remote = nullptr;
    unsigned toggle_bit = 0;
    std::size_t position = 0;
    std::size_t line_number = 0;
    while (position < input.size()) {
        ++line_number;
        const auto newline = input.find('\n', position);
        const auto end = newline == std::string_view::npos ? input.size()
                                                           : newline;
        auto length = end - position;
        if (length > 0 && input[position + length - 1] == '\r') {
            --length;
        }
        if (length >= kLineBytes) {
            return fail(error, line_number, "line is too long");
        }
        char line[kLineBytes]{};
        std::memcpy(line, input.data() + position, length);
        position = newline == std::string_view::npos ? input.size()
                                                     : newline + 1;

        if (char* comment = std::strchr(line, '#'); comment != nullptr) {
            *comment = '\0';
        }
        char* tokens[4]{};
        const auto count = tokenize(line, tokens, std::size(tokens));
        if (count == 0) {
            continue;
        }
        if (count > std::size(tokens)) {
            return fail(error, line_number, "too many fields on line");
        }

        if (state == ParseState::top) {
            if (count != 2 || std::strcmp(tokens[0], "begin") != 0 ||
                std::strcmp(tokens[1], "remote") != 0) {
                return fail(error, line_number, "expected begin remote");
            }
            if (output->remote_count == output->remotes.size()) {
                return fail(error, line_number, "too many remote blocks");
            }
            remote = &output->remotes[output->remote_count];
            *remote = LircdRemote{};
            remote->first_key = static_cast<std::uint16_t>(output->key_count);
            toggle_bit = 0;
            state = ParseState::remote;
            continue;
        }

        if (state == ParseState::remote) {
            if (count == 2 && std::strcmp(tokens[0], "begin") == 0 &&
                std::strcmp(tokens[1], "codes") == 0) {
                state = ParseState::codes;
                continue;
            }
            if (count == 2 && std::strcmp(tokens[0], "end") == 0 &&
                std::strcmp(tokens[1], "remote") == 0) {
                const unsigned all_bits = remote->pre_data_bits + remote->bits;
                if (toggle_bit > 0) {
                    if (toggle_bit > all_bits ||
                        remote->toggle_bit_mask != 0) {
                        return fail(error, line_number,
                                    "invalid or duplicate toggle bit");
                    }
                    remote->toggle_bit_mask =
                        std::uint64_t{1} << (all_bits - toggle_bit);
                }
                if (!finish_remote(remote, error, line_number)) {
                    return false;
                }
                for (std::size_t i = remote->first_key;
                     i < remote->first_key + remote->key_count; ++i) {
                    if ((output->keys[i].code & ~bit_mask(remote->bits)) != 0) {
                        return fail(error, line_number,
                                    "key code does not fit bits");
                    }
                }
                ++output->remote_count;
                remote = nullptr;
                state = ParseState::top;
                continue;
            }
            if (!parse_remote_field(tokens, count, remote, &toggle_bit)) {
                return fail(error, line_number,
                            "unsupported or invalid remote field");
            }
            continue;
        }

        if (count == 2 && std::strcmp(tokens[0], "end") == 0 &&
            std::strcmp(tokens[1], "codes") == 0) {
            state = ParseState::remote;
            continue;
        }
        if (count != 2) {
            return fail(error, line_number, "invalid code entry");
        }
        if (output->key_count == output->keys.size()) {
            return fail(error, line_number, "too many code entries");
        }
        auto& key = output->keys[output->key_count];
        if (!copy_name(tokens[0], &key.name) ||
            !parse_u64(tokens[1], &key.code)) {
            return fail(error, line_number, "invalid code name or value");
        }
        ++output->key_count;
        ++remote->key_count;
    }

    if (state != ParseState::top) {
        return fail(error, line_number, "unterminated block");
    }
    if (output->remote_count == 0) {
        return fail(error, line_number, "no remote blocks found");
    }
    return true;
}

const char* protocol_name(const LircdProtocol protocol) {
    switch (protocol) {
        case LircdProtocol::space_enc:
            return "SPACE_ENC";
        case LircdProtocol::rc5:
            return "RC5";
        case LircdProtocol::none:
            break;
    }
    return "UNKNOWN";
}

}  // namespace stb_buddy
