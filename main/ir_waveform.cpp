#include "ir_waveform.hpp"

#include <limits>

namespace stb_buddy {

namespace {

class Builder final {
public:
    Builder(const LircdRemote& remote, IrWaveform* output)
        : remote_(remote), output_(output) {}

    bool build(const std::uint64_t code, const bool repeat_frame,
               const char** error) {
        *output_ = IrWaveform{};
        if (repeat_frame && remote_.repeat.mark != 0) {
            if (remote_.repeat_header && !timing(remote_.header)) {
                return set_error(error, "invalid repeat header");
            }
            if (!mark(remote_.plead) || !timing(remote_.repeat) ||
                !mark(remote_.ptrail)) {
                return set_error(error, "repeat waveform is too large");
            }
        } else {
            if (!timing(remote_.header) || !mark(remote_.plead) ||
                !bits(remote_.pre_data, remote_.pre_data_bits, 0) ||
                !bits(code, remote_.bits, remote_.pre_data_bits) ||
                !mark(remote_.ptrail)) {
                return set_error(error, "key waveform is too large");
            }
        }
        if (output_->count > 0 && output_->count % 2 == 0) {
            output_->signal_us -= output_->durations[--output_->count];
        }
        if (output_->count == 0) {
            return set_error(error, "empty waveform");
        }
        if (remote_.const_length) {
            if (remote_.gap <= output_->signal_us) {
                return set_error(error, "CONST_LENGTH gap is shorter than signal");
            }
            output_->gap_us = remote_.gap - output_->signal_us;
        } else {
            output_->gap_us = remote_.gap;
        }
        return true;
    }

private:
    static bool set_error(const char** error, const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    }

    bool append(const bool is_mark, const std::uint32_t duration) {
        if (duration == 0) {
            return true;
        }
        if (!is_mark && output_->count == 0) {
            return true;
        }
        const bool last_is_mark = output_->count % 2 == 1;
        if (output_->count > 0 && last_is_mark == is_mark) {
            auto& last = output_->durations[output_->count - 1];
            if (duration > 32767 - last) {
                return false;
            }
            last += duration;
        } else {
            if (output_->count == output_->durations.size() ||
                duration > 32767) {
                return false;
            }
            output_->durations[output_->count++] = duration;
        }
        if (duration > std::numeric_limits<std::uint32_t>::max() -
                           output_->signal_us) {
            return false;
        }
        output_->signal_us += duration;
        return true;
    }

    bool mark(const std::uint32_t duration) { return append(true, duration); }
    bool space(const std::uint32_t duration) {
        return append(false, duration);
    }

    bool timing(const LircdTiming& value) {
        if (value.mark == 0 && value.space == 0) {
            return true;
        }
        return mark(value.mark) && space(value.space);
    }

    bool bits(const std::uint64_t value, const unsigned count,
              const unsigned already_done) {
        const unsigned all_bits = remote_.pre_data_bits + remote_.bits;
        for (unsigned index = 0; index < count; ++index) {
            const unsigned source_bit = count - 1 - index;
            const unsigned frame_bit = all_bits - 1 - already_done - index;
            bool one = ((value >> source_bit) & 1U) != 0;
            const auto frame_mask = std::uint64_t{1} << frame_bit;
            if ((remote_.toggle_bit_mask & frame_mask) != 0) {
                one = (remote_.toggle_state & frame_mask) != 0;
            }
            if (remote_.protocol == LircdProtocol::rc5 && one) {
                if (!space(remote_.one.space) || !mark(remote_.one.mark)) {
                    return false;
                }
            } else {
                const auto& timing_value = one ? remote_.one : remote_.zero;
                if (!mark(timing_value.mark) || !space(timing_value.space)) {
                    return false;
                }
            }
        }
        return true;
    }

    const LircdRemote& remote_;
    IrWaveform* output_;
};

}  // namespace

bool IrWaveformBuilder::build(const LircdRemote& remote,
                              const std::uint64_t code,
                              const bool repeat_frame, IrWaveform* output,
                              const char** error) {
    if (output == nullptr || remote.protocol == LircdProtocol::none ||
        remote.bits == 0) {
        if (error != nullptr) {
            *error = "invalid waveform arguments";
        }
        return false;
    }
    if (error != nullptr) {
        *error = nullptr;
    }
    Builder builder(remote, output);
    return builder.build(code, repeat_frame, error);
}

}  // namespace stb_buddy
