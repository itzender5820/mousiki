// Spectrogram and waterfall, image and braille: each look keeps its own values.
#include "test_framework.h"
#include "viz_prefs.h"

#include <sstream>

using namespace muisc;

TEST(spectro_profiles_keep_their_own_values) {
    VizPrefs v;
    v.spectro.style = 1;            // image
    v.spectro_sync();
    v.spectro.scheme = 1;           // classic
    v.spectro.gain = 33;
    v.spectro_sync();
    spectro_adjust(v.spectro, kSrView, +1);   // the waterfall, image
    v.spectro_sync();
    CHECK_EQ(v.spectro.view, 1);
    CHECK_EQ(v.spectro.style, 1);
    CHECK_EQ(v.spectro.scheme, 1);   // a look never used starts with a copy of the values in use
    v.spectro.scheme = 3;
    v.spectro.wf_graphs = 55;
    v.spectro_sync();
    spectro_adjust(v.spectro, kSrStyle, -1);  // the waterfall, braille: a copy again
    v.spectro_sync();
    CHECK_EQ(v.spectro.style, 0);
    CHECK_EQ(v.spectro.scheme, 3);
    v.spectro.scheme = 2;
    v.spectro_sync();
    spectro_adjust(v.spectro, kSrView, -1);   // back to the spectrogram, braille
    v.spectro_sync();
    CHECK_EQ(v.spectro.view, 0);
    CHECK_EQ(v.spectro.style, 0);
    spectro_adjust(v.spectro, kSrStyle, +1);  // the spectrogram, image: its old values
    v.spectro_sync();
    CHECK_EQ(v.spectro.scheme, 1);
    CHECK_EQ(v.spectro.gain, 33);
    spectro_adjust(v.spectro, kSrView, +1);   // the waterfall, image
    v.spectro_sync();
    CHECK_EQ(v.spectro.scheme, 3);
    CHECK_EQ(v.spectro.wf_graphs, 55);
}

TEST(spectro_profiles_survive_a_config_file) {
    VizPrefs v;
    v.spectro.style = 1;
    v.spectro_sync();
    v.spectro.scheme = 1;
    v.spectro_sync();
    spectro_adjust(v.spectro, kSrView, +1);
    v.spectro_sync();
    v.spectro.scheme = 3;
    v.spectro.wf_graphs = 70;
    v.spectro_sync();
    std::ostringstream out;
    spectro_config_write(out, v.spectro, "#");
    spectro_profiles_write(out, v, "#");
    VizPrefs w;
    std::istringstream in(out.str());
    for (std::string line; std::getline(in, line);) {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), val = line.substr(eq + 1);
        if (!spectro_profile_key(w, k, val)) spectro_config_key(w.spectro, k, val);
    }
    spectro_profiles_finish(w);
    CHECK_EQ(w.spectro.view, 1);
    CHECK_EQ(w.spectro.scheme, 3);
    CHECK_EQ(w.spectro.wf_graphs, 70);
    spectro_adjust(w.spectro, kSrView, -1);
    w.spectro_sync();
    CHECK_EQ(w.spectro.view, 0);
    CHECK_EQ(w.spectro.style, 1);
    CHECK_EQ(w.spectro.scheme, 1);   // the other look came back from the file
}
