// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/sn76489.hpp"

namespace cotd {

namespace {

constexpr unsigned kFeedback = 0x8000;   // the NCR 8496's noise register: 16 bits
constexpr unsigned kTap1 = 0x02, kTap2 = 0x20;

}  // namespace

Sn76489::Sn76489() {
    double v = 32767.0 / 4;              // four voices share the range
    for (int i = 0; i < 15; ++i) {
        table_[static_cast<std::size_t>(i)] = static_cast<int>(v);
        v /= 1.258925412;                // 2 dB a step
    }
    table_[15] = 0;
    // At reset: every register 0, the voices silent (volume words 0), the
    // periods and counts 0, the last register 3, the noise register full.
    last_ = 3;
}

void Sn76489::write(std::uint8_t data) {
    int r;
    if (data & 0x80) {
        r = (data & 0x70) >> 4;
        last_ = r;
        if (r == 6 && (data & 0x04) != (reg_[6] & 0x04)) rng_ = kFeedback;   // a new noise mode
        reg_[static_cast<std::size_t>(r)] = (reg_[static_cast<std::size_t>(r)] & 0x3F0) | (data & 0x0F);
    } else {
        r = last_;
        if ((r & 1) || r == 6) return;   // the NCR 8496 ignores these
    }
    const std::size_t c = static_cast<std::size_t>(r >> 1);
    switch (r) {
        case 0:
        case 2:
        case 4:
            if (!(data & 0x80)) reg_[static_cast<std::size_t>(r)] = (reg_[static_cast<std::size_t>(r)] & 0x0F) | ((data & 0x3F) << 4);
            period_[c] = reg_[static_cast<std::size_t>(r)] ? reg_[static_cast<std::size_t>(r)] : 0x400;   // 0 is 0x400
            if (r == 4 && (reg_[6] & 3) == 3) period_[3] = period_[2] << 1;
            break;
        case 1:
        case 3:
        case 5:
        case 7:
            vol_[c] = table_[static_cast<std::size_t>(data & 0x0F)];
            break;
        case 6:
            period_[3] = (reg_[6] & 3) == 3 ? period_[2] << 1 : 1 << (5 + (reg_[6] & 3));
            break;
        default:
            break;
    }
}

void Sn76489::step() {
    for (std::size_t i = 0; i < 3; ++i)
        if (--count_[i] <= 0) {
            out_[i] ^= 1;
            count_[i] = period_[i];
        }
    if (--count_[3] <= 0) {
        // White noise (mode bit 2): bit 1 XOR bit 5; periodic: bit 1 alone.
        const bool t1 = (rng_ & kTap1) != 0;
        const bool t2 = ((rng_ & kTap2) != kTap2) && (reg_[6] & 4);
        rng_ = (rng_ >> 1) | (t1 != t2 ? kFeedback : 0);
        out_[3] = static_cast<int>(rng_ & 1);
        count_[3] = period_[3];
    }
}

int Sn76489::sample(double rate, bool point) {
    pos_ += kClock / kDivider / rate;
    long sum = 0;
    int n = 0, last = 0;
    while (pos_ >= 1.0) {
        pos_ -= 1.0;
        step();
        last = 0;
        for (std::size_t i = 0; i < 4; ++i)
            if (out_[i]) last += vol_[i];
        sum += last;
        ++n;
    }
    const int v = point || n == 0 ? last : static_cast<int>(sum / n);
    return -v;
}

}  // namespace cotd
