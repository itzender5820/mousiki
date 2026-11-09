// Two different songs with the same title must not share one downloaded file (the cache file is named after the title).
#include "test_framework.h"
#include "cache_manager.h"
#include <filesystem>

using namespace muisc;
namespace fs = std::filesystem;

TEST(cache_key_separates_songs_with_the_same_title) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_cache_key_test";
    fs::remove_all(dir);
    CacheManager c;
    c.set_download_dir(dir);
    CHECK(c.cache_key("Intro", "AAAAAAAAAAA") == "Intro");        // nothing recorded: the plain title
    c.record_audio_download("Intro", "AAAAAAAAAAA");
    CHECK(c.cache_key("Intro", "AAAAAAAAAAA") == "Intro");        // the same video finds its own file again
    CHECK(c.cache_key("Intro", "BBBBBBBBBBB") == "Intro [BBBBBBBBBBB]");   // another video: a file of its own
    c.record_audio_download("Intro [BBBBBBBBBBB]", "BBBBBBBBBBB");
    CHECK(c.cache_key("Intro", "BBBBBBBBBBB") == "Intro [BBBBBBBBBBB]");
    CHECK(c.path_for("Intro [BBBBBBBBBBB]") != c.path_for("Intro"));
    CHECK(c.cache_key("Intro", "https://x/y") == "Intro");        // SoundCloud / Bandcamp pages keep the old naming
    fs::remove_all(dir);
}
