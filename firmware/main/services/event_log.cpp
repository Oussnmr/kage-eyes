#include "event_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace {
constexpr int ENTRY_COUNT = 12;
constexpr int ENTRY_SIZE = 96;

static char s_entries[ENTRY_COUNT][ENTRY_SIZE] = {};
static int s_next;
static int s_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void copy_entry(char *destination, const char *source) {
    size_t length = std::strlen(source);
    if (length >= ENTRY_SIZE) length = ENTRY_SIZE - 1;
    std::memcpy(destination, source, length);
    destination[length] = 0;
}
}

void event_log_add(const char *format, ...) {
    char message[72] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    char line[ENTRY_SIZE] = {};
    const unsigned long seconds =
        static_cast<unsigned long>(esp_timer_get_time() / 1000000ULL);
    std::snprintf(line, sizeof(line), "[%lus] %s", seconds, message);

    portENTER_CRITICAL(&s_lock);
    copy_entry(s_entries[s_next], line);
    s_next = (s_next + 1) % ENTRY_COUNT;
    if (s_count < ENTRY_COUNT) ++s_count;
    portEXIT_CRITICAL(&s_lock);
}

void event_log_snapshot(char *buffer, size_t size) {
    if (!buffer || size == 0) return;
    buffer[0] = 0;

    char entries[ENTRY_COUNT][ENTRY_SIZE] = {};
    int count = 0;
    int start = 0;

    portENTER_CRITICAL(&s_lock);
    count = s_count;
    start = (s_next - s_count + ENTRY_COUNT) % ENTRY_COUNT;
    for (int i = 0; i < count; ++i) {
        copy_entry(entries[i], s_entries[(start + i) % ENTRY_COUNT]);
    }
    portEXIT_CRITICAL(&s_lock);

    size_t used = 0;
    for (int i = 0; i < count; ++i) {
        const int written = std::snprintf(buffer + used, size - used, "%s%s",
                                          i ? "\n" : "", entries[i]);
        if (written < 0) break;
        if (static_cast<size_t>(written) >= size - used) {
            buffer[size - 1] = 0;
            break;
        }
        used += static_cast<size_t>(written);
    }
}
