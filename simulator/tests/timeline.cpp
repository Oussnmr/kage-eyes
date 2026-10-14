#include "../typing_timeline.hpp"
#include <cassert>
#include <cstdio>

int main() {
    TypingTimeline t;
    t.press(0);
    assert(t.phase() == TypingTimeline::Raised);
    t.tick(55,55,85);
    assert(t.phase() == TypingTimeline::Striking);
    assert(t.region() == 0);
    t.press(2); // an immediate new press MUST lift even while the hand is down
    assert(t.phase() == TypingTimeline::Raised);
    t.tick(55,55,85);
    assert(t.phase() == TypingTimeline::Striking && t.region() == 2);
    assert(t.strokes() == 2);
    t.reset();
    // Repeated keystrokes are queued, not silently coalesced.
    for(int i = 0; i < 8; ++i) t.press(i%3);
    for(int i = 0; i < 100; ++i) t.tick(40,55,85);
    assert(t.strokes() == 8);
    assert(t.pending() == 0);
    assert(t.phase() == TypingTimeline::Raised);
    std::puts("typing timeline OK");
}
