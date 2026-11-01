#pragma once
// Latin spelling of Japanese, Korean and Chinese text -- for the "Latin letters" display toggle (HKeyTranslit, default SHIFT+') and for
// searching such titles with Latin letters.
//   Japanese kana (hiragana, katakana)  ->  Hepburn romaji (shi, tsu, kya, a doubled consonant for the small tsu, a macron for the long
//                                           vowel mark: "ラーメン" -> "rāmen"; "ん" before a vowel gets an apostrophe)
//   Korean Hangul                       ->  Revised Romanization ("한국어" -> "hangugeo"; the sounds are read as written, the
//                                           pronunciation changes between neighbouring consonants are not applied)
//   Chinese characters                  ->  Hanyu Pinyin with tone marks, one syllable per character ("你好" -> "nǐ hǎo"; a character
//                                           with several readings gets its most common one)
// Chinese characters need to be told apart: in Japanese text their reading depends on the word and needs a dictionary (東京 = tōkyō,
// 東 alone = higashi), so they are LEFT AS THEY ARE there; in Chinese text they become pinyin. Which one a text is: kana in it (or in
// another tag of the same track) = Japanese; characters only the Japanese or only the Chinese writing uses (桜 楽 変 / 们 这 乐) say so;
// the other tags of the track and the artist's other tracks say so (translit_learn); otherwise the characters stay (or become pinyin,
// when LatinHanziDefault=chinese). A reading dictionary (built in, plus the user's kanji_readings.txt) names whole words
// (米津玄師 = Yonezu Kenshi) and wins over all of that. A name written as "Yonezu Kenshi (米津玄師)" is shown as its Latin part.
// Everything else (Latin letters, digits, punctuation) stays; full-width forms become normal ones.
#include <string>
#include <vector>

namespace muisc {

// Quick test: might the text contain anything transliterate() changes? (Looks at the bytes only.)
bool translit_possible(const std::string& utf8);

// The Latin spelling; the text itself when there is nothing to convert.
std::string transliterate(const std::string& utf8);

// For searching: the text, and behind it (space-separated) its Latin spelling when that differs.
std::string with_latin_spelling(const std::string& utf8);

enum class Script { Unknown, Japanese, Chinese };

// Which language the characters of the text alone point to (kana, or characters only Japanese / only Chinese uses).
Script script_of(const std::string& utf8);

// A track's tags (call it when they are read): decides the track's script from all of them (and from what is known about its artist)
// and remembers it for every text of the track that holds Chinese characters; also learns the reading of a name that is written
// both ways ("Kenshi Yonezu - 米津玄師").
// `path` (the file's path, UTF-8; may be empty) also teaches: the folder names and the part of the file name before " - " are Latin
// candidates for an artist written in Chinese characters / kana; the one that most tracks of the artist agree on (3 or more) is taken.
void translit_learn(const std::string& title, const std::string& artist, const std::string& album, const std::string& path = std::string());

// Readings learned (from the tags, the folders, the MusicBrainz lookup) are kept in this file and read again at the next start;
// call it BEFORE translit_load_readings() so that the user's own file wins.
void translit_set_learned_file(const std::string& path);

// Names of artists written with Chinese characters that have no reading yet (for the MusicBrainz lookup): up to `max`, each once.
std::vector<std::string> translit_take_pending(size_t max);
// The answer of the lookup: the Latin name found (saved in the learned file), or "nothing there" (remembered, not asked again).
void translit_lookup_found(const std::string& cjk, const std::string& latin);
void translit_lookup_none(const std::string& cjk);
void translit_lookup_retry(const std::string& cjk);   // a temporary problem: back to the queue

// Texts in Japanese context whose kanji stayed (so a reader like pykakasi could name them): up to `max`, each once; the readings that came
// back are kept in `file` (lines "text<TAB>reading", read again at the next start). A text the reader could not read is not offered again.
// `rules_id` names the rules the readings were made with (the app passes a fingerprint of scripts/kanji_reading.py): the first line of
// the file is "#rules <id>"; a file with another id (or none: made by an older version) is emptied and starts again, so a change of the
// reader's rules refreshes the cache by itself. An empty id keeps whatever the file holds.
void translit_set_kanji_cache_file(const std::string& path, const std::string& rules_id = std::string());
std::vector<std::string> translit_take_kanji_pending(size_t max);
void translit_kanji_found(const std::string& text, const std::string& reading);
void translit_kanji_failed(const std::string& text);   // not read for good (the reader gave no reading)
// How many of `texts` the reader has been asked for (they are queued now if they were not) and has not answered yet.
size_t translit_kanji_waiting(const std::vector<std::string>& texts);
void translit_kanji_retry(const std::string& text);    // the reader itself failed: offered again later

// The user's reading dictionary: lines "米津玄師 = Yonezu Kenshi"; "chinese: 周杰倫" / "japanese: 周杰倫" say which language a text is.
void translit_load_readings(const std::string& path);

// Chinese characters without any hint: false = leave them (the default), true = read them as Chinese.
void translit_set_default_chinese(bool chinese);
// LatinHanziDefault: 0 = leave Chinese characters of unknown language alone, 1 = read them as Chinese (pinyin), 2 = read them as Japanese
// (they stay as they are unless the optional kanji reader knows them).
void translit_set_default_hanzi(int mode);
// Spelling of Japanese: `macrons` = long vowels with a macron (ō ū ī, kōhī), else spelled out (ou, uu, koohii);
// `wo` = the particle を as "wo", else "o". Defaults: both false (the easiest to read for English / German readers).
void translit_set_style(bool macrons, bool wo);

// The display switch (HKeyTranslit). apply_font_map() and the lyrics views ask it.
bool translit_enabled();
void set_translit_enabled(bool on);

// transliterate() when the switch is on, else the text.
std::string shown_text(const std::string& utf8);

}  // namespace muisc
