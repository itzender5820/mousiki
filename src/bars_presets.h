// The "Behavior" presets of the spectrum bars: fluidity / degradation / viscosity as one pick. Both styles (Classic and Hi-Fi) keep
// their own three values and use the same presets. Shared by the player and the radio.
#pragma once

namespace muisc {

struct BarsPreset { const char* name; int fluidity, degradation, viscosity; };
inline constexpr BarsPreset kBarsPresets[4] = {
    {"lively", 2, 9, 1},     // snappy rise, quick fall, bars independent
    {"default", 10, 8, 3},   // what the bars did before presets existed
    {"smooth", 10, 4, 6},    // soft rise, slower fall, neighbours blended
    {"slow", 10, 2, 8},      // floaty and wobbly
};
constexpr int kBarsCustom = 4;   // the three values match no preset

inline int bars_preset_of(int fluidity, int degradation, int viscosity) {
    for (int i = 0; i < 4; ++i)
        if (kBarsPresets[i].fluidity == fluidity && kBarsPresets[i].degradation == degradation && kBarsPresets[i].viscosity == viscosity) return i;
    return kBarsCustom;
}
inline const char* bars_preset_name(int i) { return i >= 0 && i < 4 ? kBarsPresets[i].name : "custom"; }

}  // namespace muisc
