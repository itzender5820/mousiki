#pragma once
#include <cstdint>

namespace muisc {
bool is_indic_codepoint(uint32_t cp);
int indic_codepoint_width(uint32_t cp);
}
