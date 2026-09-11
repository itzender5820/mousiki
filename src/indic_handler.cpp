#include "indic_handler.h"

namespace muisc {

bool is_indic_codepoint(uint32_t cp) {
    if (cp >= 0x0900 && cp <= 0x0D7F) return true;
    if (cp >= 0x1CD0 && cp <= 0x1CFF) return true;
    if (cp >= 0xA8E0 && cp <= 0xA8FF) return true;
    return false;
}

static bool is_indic_combining(uint32_t cp) {
    if (cp >= 0x0900 && cp <= 0x097F) {
        if (cp <= 0x0903) return true;
        if (cp >= 0x093A && cp <= 0x093C) return true;
        if (cp >= 0x093E && cp <= 0x094F) return true;
        if (cp >= 0x0951 && cp <= 0x0957) return true;
        if (cp >= 0x0962 && cp <= 0x0963) return true;
        return false;
    }
    if (cp >= 0x0980 && cp <= 0x09FF) {
        if (cp >= 0x0981 && cp <= 0x0983) return true;
        if (cp == 0x09BC) return true;
        if (cp >= 0x09BE && cp <= 0x09CD) return true;
        if (cp == 0x09D7) return true;
        if (cp >= 0x09E2 && cp <= 0x09E3) return true;
        if (cp == 0x09FE) return true;
        return false;
    }
    if (cp >= 0x0A00 && cp <= 0x0A7F) {
        if (cp >= 0x0A01 && cp <= 0x0A03) return true;
        if (cp == 0x0A3C) return true;
        if (cp >= 0x0A3E && cp <= 0x0A4D) return true;
        if (cp >= 0x0A70 && cp <= 0x0A71) return true;
        if (cp == 0x0A75) return true;
        return false;
    }
    if (cp >= 0x0A80 && cp <= 0x0AFF) {
        if (cp >= 0x0A81 && cp <= 0x0A83) return true;
        if (cp == 0x0ABC) return true;
        if (cp >= 0x0ABE && cp <= 0x0ACD) return true;
        if (cp >= 0x0AE2 && cp <= 0x0AE3) return true;
        if (cp >= 0x0AFA && cp <= 0x0AFF) return true;
        return false;
    }
    if (cp >= 0x0B00 && cp <= 0x0B7F) {
        if (cp >= 0x0B01 && cp <= 0x0B03) return true;
        if (cp == 0x0B3C) return true;
        if (cp >= 0x0B3E && cp <= 0x0B4D) return true;
        if (cp >= 0x0B55 && cp <= 0x0B57) return true;
        if (cp >= 0x0B62 && cp <= 0x0B63) return true;
        return false;
    }
    if (cp >= 0x0B80 && cp <= 0x0BFF) {
        if (cp == 0x0B82) return true;
        if (cp >= 0x0BBE && cp <= 0x0BCD) return true;
        if (cp == 0x0BD7) return true;
        return false;
    }
    if (cp >= 0x0C00 && cp <= 0x0C7F) {
        if (cp >= 0x0C00 && cp <= 0x0C04) return true;
        if (cp == 0x0C3C) return true;
        if (cp >= 0x0C3E && cp <= 0x0C4D) return true;
        if (cp >= 0x0C55 && cp <= 0x0C56) return true;
        if (cp >= 0x0C62 && cp <= 0x0C63) return true;
        return false;
    }
    if (cp >= 0x0C80 && cp <= 0x0CFF) {
        if (cp >= 0x0C81 && cp <= 0x0C83) return true;
        if (cp == 0x0CBC) return true;
        if (cp >= 0x0CBE && cp <= 0x0CCD) return true;
        if (cp >= 0x0CD5 && cp <= 0x0CD6) return true;
        if (cp >= 0x0CE2 && cp <= 0x0CE3) return true;
        return false;
    }
    if (cp >= 0x0D00 && cp <= 0x0D7F) {
        if (cp >= 0x0D00 && cp <= 0x0D03) return true;
        if (cp == 0x0D3B || cp == 0x0D3C) return true;
        if (cp >= 0x0D3E && cp <= 0x0D4D) return true;
        if (cp == 0x0D57) return true;
        if (cp >= 0x0D62 && cp <= 0x0D63) return true;
        return false;
    }
    if (cp >= 0x1CD0 && cp <= 0x1CFF) return true;
    if (cp >= 0xA8E0 && cp <= 0xA8FF) return true;
    return false;
}

int indic_codepoint_width(uint32_t cp) {
    return is_indic_combining(cp) ? 0 : 1;
}

}
