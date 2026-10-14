#pragma once
#include <algorithm>
#include <cstdint>

// Pure C++ state machine used by the LVGL browser simulator and, later, ESP-IDF.
// Only typing activity is passed; no characters are recorded or transmitted.
class TypingTimeline {
public:
    enum Phase { Raised, Striking };
    void reset() { head_ = tail_ = count_ = 0; phase_ = Raised; elapsed_ = 0; strokes_ = 0; }
    void press(int region) {
        // Every press triggers a raised hand, including presses during a strike.
        if (count_ < 64) {
            regions_[tail_] = static_cast<uint8_t>(std::clamp(region, 0, 4));
            tail_ = (tail_ + 1) % 64;
            ++count_;
        } else {
            // Under extreme input, prefer recent keys without growing memory.
            regions_[(tail_ + 63) % 64] = static_cast<uint8_t>(std::clamp(region, 0, 4));
        }
        phase_ = Raised;
        elapsed_ = 0;
    }
    void tick(int delta_ms, int raised_ms, int strike_ms) {
        int dt = std::clamp(delta_ms, 0, 100);
        elapsed_ += dt;
        const int up = std::max(20, raised_ms);
        const int down = std::max(35, strike_ms);
        for (int guard = 0; guard < 4; ++guard) {
            if (phase_ == Raised && count_ > 0 && elapsed_ >= up) {
                elapsed_ -= up;
                region_ = regions_[head_];
                head_ = (head_ + 1) % 64;
                --count_;
                phase_ = Striking;
                ++strokes_;
            } else if (phase_ == Striking && elapsed_ >= down) {
                elapsed_ -= down;
                phase_ = Raised;
            } else break;
        }
        if (phase_ == Raised && count_ == 0) elapsed_ = 0;
    }
    Phase phase() const { return phase_; }
    int region() const { return region_; }
    int pending() const { return count_; }
    unsigned strokes() const { return strokes_; }
private:
    uint8_t regions_[64] = {};
    int head_ = 0, tail_ = 0, count_ = 0;
    int region_ = 0;
    int elapsed_ = 0;
    Phase phase_ = Raised;
    unsigned strokes_ = 0;
};
