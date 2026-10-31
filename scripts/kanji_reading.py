#!/usr/bin/env python3
"""Reads Japanese text (kanji and kana) in Latin letters with pykakasi, for the Latin letters display of Mousiki.

usage: kanji_reading.py <file with one text per line, UTF-8>

pykakasi is NOT part of Mousiki (it is licensed GPL-3.0-or-later, Mousiki is Apache-2.0): it is an optional package you install
yourself with `pip install pykakasi`; this script only calls it as a separate program, on your computer, nothing is sent anywhere.
One JSON line is printed:

  {"ok": true, "readings": {"夜に駆ける": "yoru ni kakeru", ...}}
  {"ok": false, "missing": true, "error": "pykakasi is not installed (pip install pykakasi)"}
  {"ok": false, "error": "..."}
"""
import json
import sys
import os
import re
import unicodedata

# the Windows installer's optional step puts pykakasi into <install folder>\pylib (scripts\install_kanji_reader.py)
_PYLIB = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "pylib")
if os.path.isdir(_PYLIB) and _PYLIB not in sys.path:
    sys.path.append(_PYLIB)


# the output is JSON with the characters themselves: UTF-8 whatever the console code page is (Windows pipes default to cp1252 / cp850)
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def has_cjk(s):
    for ch in s:
        o = ord(ch)
        if 0x3040 <= o <= 0x30FF or 0x3400 <= o <= 0x9FFF or 0xFF66 <= o <= 0xFF9F:
            return True
    return False


PARTICLES = {"\u306e", "\u306b", "\u3067", "\u3092", "\u306f", "\u304c", "\u3068", "\u3082", "\u3078", "\u3084", "\u304b\u3089", "\u307e\u3067", "\u3088\u308a", "\u3063\u3066"}


def hiragana(s):
    return bool(s) and all(0x3041 <= ord(ch) <= 0x309F for ch in s)


def has_kanji(s):
    return any(0x3400 <= ord(ch) <= 0x9FFF for ch in s)


def fix_sokuon(text):
    """A full-size \u3064 where a small \u3063 belongs (lyrics sites write \u3044\u3064\u3066 for \u3044\u3063\u3066): only before \u3066 / \u305f and after a kana,
    and not in \u304b\u3064\u3066 (formerly) -- the same rule as the built-in conversion (src/translit.cpp)."""
    out = []
    for i, ch in enumerate(text):
        if (ch in ("\u3064", "\u30c4") and 0 < i < len(text) - 1 and text[i + 1] in ("\u3066", "\u305f", "\u30c6", "\u30bf")
                and (0x3041 <= ord(text[i - 1]) <= 0x3093 or 0x30A1 <= ord(text[i - 1]) <= 0x30F3) and text[i - 1] not in ("\u3093", "\u30f3")
                and not (text[i - 1] in ("\u304b", "\u30ab") and text[i + 1] in ("\u3066", "\u30c6")
                         and (i < 2 or not (0x3041 <= ord(text[i - 2]) <= 0x30F3)))):
            out.append("\u3063" if ch == "\u3064" else "\u30c3")
        else:
            out.append(ch)
    return "".join(out)


# --- numbers next to Japanese text (1日, 3周, 2羽, 15ぴょこ): written out in kana before the readers see them ---------------------------
_ONES = ["", "いち", "に", "さん", "よん", "ご", "ろく", "なな", "はち", "きゅう"]
_HUND = {1: "ひゃく", 3: "さんびゃく", 6: "ろっぴゃく", 8: "はっぴゃく"}
_THOU = {1: "せん", 3: "さんぜん", 8: "はっせん"}
# counter -> (reading, what the last digit does: {digit: replacement of the whole last number word}, sokuon before it: 1 6 8 10)
_COUNTERS = {
    "日": ("にち", {}, False), "周": ("しゅう", {}, True), "羽": ("わ", {3: "ば", 6: "ぱ"}, False), "度": ("ど", {}, False),
    "回": ("かい", {}, True), "人": ("にん", {}, False), "年": ("ねん", {}, False), "月": ("がつ", {}, False), "時": ("じ", {}, False),
    "歳": ("さい", {}, True), "個": ("こ", {}, True), "枚": ("まい", {}, False), "番": ("ばん", {}, False),
}
_DAYS = {1: "いちにち", 2: "ふつか", 3: "みっか", 4: "よっか", 5: "いつか", 6: "むいか", 7: "なのか", 8: "ようか", 9: "ここのか", 10: "とおか",
         14: "じゅうよっか", 20: "はつか", 24: "にじゅうよっか"}


def num_kana(n):
    """0..9999 in kana (3 = さん, 15 = じゅうご, 2020 = にせんにじゅう)."""
    if n == 0:
        return "ぜろ"
    out = ""
    th, rest = divmod(n, 1000)
    hu, rest = divmod(rest, 100)
    te, on = divmod(rest, 10)
    if th:
        out += _THOU.get(th, _ONES[th] + "せん")
    if hu:
        out += _HUND.get(hu, _ONES[hu] + "ひゃく")
    if te:
        out += ("" if te == 1 else _ONES[te]) + "じゅう"
    if on:
        out += _ONES[on]
    return out


def counter_kana(n, counter):
    reading, special, sokuon = _COUNTERS[counter]
    if counter == "日" and n in _DAYS:
        return _DAYS[n] if n != 1 else "いちにち"
    if counter == "人" and n in (1, 2):
        return "ひとり" if n == 1 else "ふたり"
    base = num_kana(n)
    last = n % 10
    if counter in ("人", "年", "時") and last == 4:
        base = base[:-2] + "よ"
    if counter in ("月", "時") and last == 7:
        base = base[:-2] + "しち"
    if counter == "月" and last == 4:
        base = base[:-2] + "し"
    if counter in ("月", "時") and last == 9:
        base = base[:-3] + "く"
    if counter == "年" and last == 7:
        base = base[:-2] + "しち"
    if sokuon and (n % 10 in (1, 6, 8) or n % 10 == 0):
        for a, b in (("いち", "いっ"), ("ろく", "ろっ"), ("はち", "はっ"), ("じゅう", "じゅっ")):
            if base.endswith(a):
                base = base[:-len(a)] + b
                break
    if counter == "羽" and last in special:   # さんば ろっぱ (and じゅっぱ for 10)
        for a in ("さん", "ろく"):
            if base.endswith(a):
                base = base[:-len(a)] + ("さん" if a == "さん" else "ろっ")
                break
        return base + special[last]
    if counter == "羽" and n % 10 == 0 and n:
        return base[:-3] + "じゅっぱ"
    return base + reading


def fix_numbers(text):
    """1日 -> いちにち, 3周 -> さんしゅう, 2羽 -> にわ, 15ぴょこ -> じゅうごぴょこ: digits that stand against Japanese text (not F1, not 2020 among Latin words)."""
    def jp(ch):
        o = ord(ch)
        return 0x3040 <= o <= 0x30FF or 0x3400 <= o <= 0x9FFF

    def rep(m):
        digits = m.group(1)
        before = text[m.start() - 1] if m.start() else ""
        after = text[m.end()] if m.end() < len(text) else ""
        if len(digits) > 4 or (len(digits) > 1 and digits[0] == "0") or (before.isascii() and before.isalpha()):
            return m.group(0)
        return num_kana(int(digits)) if (jp(after) or (before and jp(before))) else m.group(0)

    # counters first (the counter character is consumed with the number)
    def rep_counter(m):
        digits = m.group(1)
        before = text[m.start() - 1] if m.start() else ""
        if len(digits) > 4 or (len(digits) > 1 and digits[0] == "0") or (before.isascii() and before.isalpha()):
            return m.group(0)
        return counter_kana(int(digits), m.group(2))

    text = text.translate({0xFF10 + i: ord("0") + i for i in range(10)})
    text = re.sub("([0-9]+)([" + "".join(_COUNTERS) + "])", rep_counter, text)
    return re.sub("([0-9]+)", rep, text)


_NUM_RE = re.compile("([0-9]+)([" + "".join(_COUNTERS) + "])?")


def _near_jp(ch):
    return bool(ch) and (0x3040 <= ord(ch) <= 0x30FF or 0x3400 <= ord(ch) <= 0x9FFF)


def split_numbers(text):
    """[(kind, str)]: 'text' pieces and 'kana' pieces (the written-out reading of a number that stands against Japanese text)."""
    text = text.translate({0xFF10 + i: ord("0") + i for i in range(10)})
    out, pos = [], 0
    for m in _NUM_RE.finditer(text):
        digits, counter = m.group(1), m.group(2)
        before = text[m.start() - 1] if m.start() else ""
        after = text[m.end()] if m.end() < len(text) else ""
        if len(digits) > 4 or (len(digits) > 1 and digits[0] == "0") or (before.isascii() and before.isalpha()):
            continue
        if not counter and not (_near_jp(after) or _near_jp(before)):
            continue
        if m.start() > pos:
            out.append(("text", text[pos:m.start()]))
        out.append(("kana", counter_kana(int(digits), counter) if counter else num_kana(int(digits))))
        pos = m.end()
    if pos < len(text):
        out.append(("text", text[pos:]))
    return out


# loanword sounds pykakasi spells wrongly (ティー = tii, not teii; ウィ = wi, not ui)
_DIGRAPHS = {"ティ": "ti", "ディ": "di", "トゥ": "tu", "ドゥ": "du", "デュ": "dyu", "テュ": "tyu", "ウィ": "wi", "ウェ": "we", "ウォ": "wo",
             "シェ": "she", "ジェ": "je", "スィ": "si", "ズィ": "zi", "ヴァ": "va", "ヴィ": "vi", "ヴェ": "ve", "ヴォ": "vo"}


def fix_text(text):
    """What the readers get wrong in plain kana, repaired before they see it: a full-size tsu for the small one (fix_sokuon) and
    \u30b3\u30fc\u30d2\u30fc\u306e\u3080 (drink coffee) that is cut as \u30b3\u30fc\u30d2\u30fc + \u306e + \u3080... (a hiragana \u306e\u3080 right after a katakana word is the verb \u98f2\u3080)."""
    text = fix_sokuon(text)
    text = re.sub("(?<![一二三四五六七八九十百千万0-9])一人(?![称前分])", "ひとり", text)   # 一人 / 二人: hitori / futari (the reader says ichinin / ninin)
    text = re.sub("(?<![一二三四五六七八九十百千万0-9])二人(?![称前分])", "ふたり", text)
    text = text.replace("頬づえ", "ほおづえ").replace("九龍", "クーロン")
    text = re.sub("(?<=[ァ-ヶー])ヘ(?=$|[\\s,.!?、。！？)」])", "へ", text)   # ニューヨークヘ: the particle へ typed as katakana
    return re.sub("(?<=[\u30a1-\u30f6\u30fc])\u306e\u3080(?![\u3041-\u3093]*\u3060\u3051)", "\u98f2\u3080", text)


def fix_romaji(s):
    """Spellings the word cutting splits that are one word: nandemo."""
    s = re.sub(r"\bnan de mo\b", "nandemo", s)
    s = s.replace("m\u00f4ichido", "m\u00f4 ichido").replace("h\u014d zue", "h\u014dzue")   # (the long-vowel markers are in the text here)
    return re.sub(r"\b([A-Z]) (\d)\b", r"\1\2", s)   # F1 (not F 1)


def read(kks, text):
    text = fix_numbers(fix_text(text))
    out = ""
    prev_cjk = False
    prev_kanji = False
    items = kks.convert(text)
    for n, item in enumerate(items):
        orig = item.get("orig", "")
        cjk = has_cjk(orig)
        seg = item.get("hepburn", "") if cjk else unicodedata.normalize("NFKC", orig)
        if cjk:
            seg = kana_to_hepburn(kks, orig) if "ー" in orig and not has_kanji(orig) else mark_long(seg)
        if orig in ("\u306f", "\u3078"):   # the particle は / へ on its own or right after a kanji (without janome there is no grammar): wa / e
            before = items[n - 1].get("orig", "") if n else ""
            after = items[n + 1].get("orig", "") if n + 1 < len(items) else ""
            if (not before or before.isspace() or has_kanji(before[-1:])) and (not after or after.isspace() or has_kanji(after[:1])):
                seg = "wa" if orig == "\u306f" else "e"
        seg = seg.replace("「", '"').replace("」", '"').replace("『", '"').replace("』", '"')
        if not seg:
            continue
        glue = cjk and prev_kanji and hiragana(orig) and orig not in PARTICLES   # 駆け|る: the ending of the word before
        if out and not out.endswith(" ") and not glue and ((cjk and out[-1].isalnum()) or (prev_cjk and seg[0].isalnum())):
            out += " "
        out += seg
        prev_cjk = cjk
        prev_kanji = has_kanji(orig) or (glue and prev_kanji)
    return fix_romaji(" ".join(out.split()))


# --- word spacing with janome (optional, Apache-2.0, pure Python, its dictionary is inside the package) -----------------------------
# Japanese is written without spaces, so romaji needs the words cut first: tokens by janome, the reading of each by pykakasi.
# Particles stand alone ("watashi wa anata ga suki desu"), endings and auxiliary verbs stay with their verb ("tabemashita").
# は / へ / を as particles are read wa / e / o.
COPULA = {"\u3067\u3059", "\u3060", "\u3067\u3057\u305f", "\u3067\u3057\u3087\u3046", "\u3067\u3057\u3087", "\u3060\u308d\u3046", "\u3060\u308d", "\u3060\u3063\u305f"}   # です だ でした でしょう だった: separate words


# Long vowels are written with marker letters, so the program can show them in the style chosen in the settings (Latin letters:
# macrons or not): ā ī ū ē ō = the vowel doubled (aa ii uu ee oo, also a prolonged sound mark), ô = o + u (ou).
# Macron style shows them as they are (ô as ō), the plain style writes aa / ii / uu / ee / oo and ou.
_LONG = {"a": "ā", "i": "ī", "u": "ū", "e": "ē", "o": "ō"}


def mark_long(hep):
    """The long vowels inside one word's romaji: ou -> ô, oo -> ō, uu -> ū (kana: おう, おお, うう)."""
    return hep.replace("ou", "ô").replace("oo", "ō").replace("uu", "ū")


def long_to_plain(s):
    """The marker letters as the plain style writes them (for tests and for readers of the cache without the program)."""
    for k, v in _LONG.items():
        s = s.replace(v, k + k)
    return s.replace("ô", "ou")


def long_to_macron(s):
    return s.replace("ô", "ō")


def kana_to_hepburn(kks, kana):
    out, buf, i = "", "", 0

    def flush():
        return mark_long("".join(it.get("hepburn", "") for it in kks.convert(buf))) if buf else ""
    while i < len(kana):
        if kana[i] == "ー" and i:   # the prolonged sound mark: the vowel before it, long
            body = out + flush()
            out, buf = "", ""
            i += 1
            if body and body[-1] in "aiueo":
                body = body[:-1] + _LONG[body[-1]]
            elif body and body[-1] in "".join(_LONG.values()) + "\u00f4":
                pass   # already long
            else:
                body += "-"
            out = body
            continue
        d = _DIGRAPHS.get(kana[i:i + 2])
        if d:
            out += flush()
            buf = ""
            i += 2
            if i < len(kana) and kana[i] == "ー":   # the long vowel after it
                d = d[:-1] + _LONG[d[-1]]
                i += 1
                while i < len(kana) and kana[i] == "ー":
                    i += 1
            out += d
        else:
            buf += kana[i]
            i += 1
    return out + flush()


def read_words(kks, tok, text):
    text = fix_text(text)
    pieces = []   # [romaji, glue_to_previous]
    prev_p0 = prev_p1 = prev_surf = ""
    kana_glue_next = False
    hold = False    # after ( or a hyphen: the next word is attached
    space = False   # a space in the lyrics: the next word starts a new word whatever its kind (でる らららら)
    tokens = []
    for kind, part in split_numbers(text):   # a number stands against Japanese text: its reading is a word of its own
        if kind == "kana":
            tokens.append((None, part))
        else:
            tokens.extend((t, None) for t in tok.tokenize(part))
    for t, num in tokens:
        if t is None:
            pieces.append([kana_to_hepburn(kks, num), False])
            prev_p0 = prev_p1 = prev_surf = ""
            continue
        surf = t.surface
        pos = t.part_of_speech.split(",")
        p0, p1 = pos[0], pos[1] if len(pos) > 1 else ""
        if surf in ("\u30fb", "\uff65"):   # the middle dot between words (サイボーグ・ペーパードライバー): a space
            space = True
            continue
        if not has_cjk(surf) and surf.strip() and not any(ch.isalnum() for ch in surf):   # ASCII punctuation (janome calls it a noun)
            if surf in (",", ".", "!", "?", ";", ":", ")", "]", "}", "%"):
                pieces.append([surf, "punct"])
            elif surf in ("-", "\u2013", "\u2014", "'", "/", "_"):    # wow-oh: no space on either side
                pieces.append([surf, "punct"])
                hold = True
            else:
                pieces.append([surf, False])   # ( [ { " ... : a word of its own, what follows is attached
                hold = surf in ("(", "[", "{", '"')
            space = False
            continue
        if not has_cjk(surf):
            seg = unicodedata.normalize("NFKC", surf)
            seg = seg.replace("\u300c", '"').replace("\u300d", '"').replace("\u300e", '"').replace("\u300f", '"')
            if not seg.strip():
                space = True
                hold = False
            if p0 == "\u8a18\u53f7" or not seg.strip():
                pieces.append([seg.strip(), "punct"])
            else:
                pieces.append([seg, "punct" if hold and not space else False])
            hold = False
            continue
        if p0 == "\u52a9\u8a5e" and surf in ("\u306f", "\u3078", "\u3092"):
            seg = {"\u306f": "wa", "\u3078": "e", "\u3092": "o"}[surf]
        else:
            rd = t.reading if t.reading and t.reading != "*" else ""
            # a small tsu at the end of a word doubles the next consonant (言っ|て = itte): pykakasi alone would write "itsu"
            geminate = rd[-1:] in ("\u30c3", "\u3063") and len(rd) > 1
            seg = kana_to_hepburn(kks, rd[:-1] if geminate else rd) if rd else ""
            if geminate and seg:
                seg += "~"
            if not seg:
                seg = kana_to_hepburn(kks, surf)
        if not seg:
            continue
        if surf in ("\u3069\u3093\u306a\u306b", "\u3053\u3093\u306a\u306b", "\u305d\u3093\u306a\u306b", "\u3042\u3093\u306a\u306b") and seg.endswith("nani"):
            seg = seg[:-2] + " ni"   # donna ni (not donnani)
        glue = ((p0 == "\u52a9\u52d5\u8a5e" and surf not in COPULA) or (p0 == "\u52d5\u8a5e" and p1 in ("\u975e\u81ea\u7acb", "\u63a5\u5c3e"))
                or (p0 == "\u540d\u8a5e" and p1 == "\u63a5\u5c3e") or (p0 == "\u52a9\u8a5e" and p1 == "\u63a5\u7d9a\u52a9\u8a5e")
                or surf in ("\u3063", "\u30c3"))
        if p0 == "\u52a9\u52d5\u8a5e" and surf == "\u306a" and prev_p0 in ("\u540d\u8a5e", "\u5f62\u5bb9\u8a5e", "\u526f\u8a5e"):
            glue = False   # oshare na, kirei na: the na of a na-adjective is a word of its own
        if surf in ("\u3051\u3069", "\u3051\u308c\u3069", "\u3051\u308c\u3069\u3082", "\u3051\u3069\u3082",
                    "\u304b\u3089", "\u306e\u3067", "\u306e\u306b", "\u3057", "\u304c"):
            glue = False   # ...shirenai kedo / dekiru kara / wakaranai node: the conjunction after a verb is a word of its own
        if surf == "\u3057\u308c" and prev_surf == "\u304b\u3082":
            glue = True    # kamoshirenai
        if surf in ("\u3063\u3066", "\u3063\u305f\u3063\u3066") and prev_surf in ("\u305f", "\u3060"):
            glue = True    # saretatte, not sareta tte
        # ...てはいけない: the verb after a particle (は, が, ...) is a word of its own, not an ending of the word before
        if glue and prev_p0 == "\u52a9\u8a5e" and prev_p1 != "\u63a5\u7d9a\u52a9\u8a5e" and p0 == "\u52d5\u8a5e":
            glue = False
        # ...は + ない: the particle then the adjective (nikumitaku wa nai), not an ending of the word before
        if p0 in ("\u52a9\u52d5\u8a5e", "\u5f62\u5bb9\u8a5e") and surf.startswith("\u306a") and prev_p0 == "\u52a9\u8a5e" and prev_p1 != "\u63a5\u7d9a\u52a9\u8a5e" and surf in ("\u306a\u3044", "\u306a\u304b\u3063", "\u306a\u304f", "\u306a\u3051\u308c"):
            glue = False
        # a noun ending after a verb ending is a noun of its own (tsubureta bun)
        if p0 == "\u540d\u8a5e" and p1 == "\u63a5\u5c3e" and prev_p0 in ("\u52a9\u52d5\u8a5e", "\u52d5\u8a5e") and surf in ("\u5206", "\u65b9", "\u6642", "\u9593"):
            glue = False
        if p1 == "\u6570" and prev_p1 == "\u6570":
            glue = True    # nijuuni: the figures of one number
        if hold and not space:
            glue = True    # after ( or a hyphen
        hold = False
        if space:
            glue = False
            space = False
        if pieces and pieces[-1][0].endswith("~"):
            glue = True   # the doubled consonant belongs to the next syllable
        if kana_glue_next and re.fullmatch("[\u30a1-\u30f6]+", surf) and not space:
            glue = True    # パ|ラッパ: a loanword cut after its first sound
        kana_glue_next = bool(re.fullmatch("[\u30a1-\u30f6]", surf)) and p1 != "\u56fa\u6709\u540d\u8a5e" or bool(re.fullmatch("\u30d1", surf))
        pieces.append([seg, glue])
        prev_p0, prev_p1, prev_surf = p0, p1, surf
    out = ""
    for seg, glue in pieces:
        if not seg:
            continue
        if out.endswith("~"):   # the doubled consonant: the first letter of what follows (ch -> t: itchi)
            first = seg[0] if seg[0].isalpha() else ""
            out = out[:-1] + ("t" if seg.startswith("ch") else first)
        if glue == "punct":
            out += seg
        elif out and not glue and not out.endswith(('"', "(")):
            out += " " + seg
        else:
            if glue and out and seg and out[-1] in "ou" and seg[0] in "ou" and (out[-1] == "o" or seg[0] == "u"):
                # a long vowel across the cut of the word (yaro|u, mi|you): ou -> ô, oo -> ō, uu -> ū
                out = out[:-1] + ("\u00f4" if (out[-1], seg[0]) == ("o", "u") else "\u014d" if out[-1] == "o" else "\u016b")
                seg = seg[1:]
            out += seg
    return fix_romaji(" ".join(out.replace("~", "").split()))


def main():
    if len(sys.argv) < 2:
        print(json.dumps({"ok": False, "error": "usage: kanji_reading.py <file>"}))
        return 2
    try:
        import pykakasi
    except Exception:
        print(json.dumps({"ok": False, "missing": True, "error": "pykakasi is not installed (pip install pykakasi)"}))
        return 0
    try:
        with open(sys.argv[1], encoding="utf-8") as f:
            texts = [line.rstrip("\r\n") for line in f if line.strip()]
        kks = pykakasi.kakasi()
        tok = None
        try:
            from janome.tokenizer import Tokenizer
            tok = Tokenizer()
        except Exception:
            tok = None
        res = {}
        for t in texts:
            r = ""
            if tok is not None:
                try:
                    r = read_words(kks, tok, t)
                except Exception:
                    r = ""
            if not r and has_kanji(t):   # (kana-only text without janome: nothing to add, the program's own romaji stays)
                r = read(kks, t)
            if r:
                res[t] = r
        print(json.dumps({"ok": True, "readings": res}, ensure_ascii=False))
    except Exception as e:
        print(json.dumps({"ok": False, "error": str(e)}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
