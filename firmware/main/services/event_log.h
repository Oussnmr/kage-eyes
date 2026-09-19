#pragma once

#include <stddef.h>

void event_log_add(const char *format, ...);
void event_log_snapshot(char *buffer, size_t size);
