// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
extern "C" {
using Ft8Callback = void (*)(const char*, int, int, float, float, float, float, int, void*);
void ft8_decode_slot(const std::int16_t* samples, int low, int high, int depth,
                     Ft8Callback callback, void* context);
}
