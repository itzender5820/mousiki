#!/usr/bin/env python3
"""Tests of the plain-kana repairs in scripts/kanji_reading.py (no pykakasi / janome needed): a full-size tsu where a small one belongs,
and the verb in コーヒーのむ.  usage: test_kanji_reading_text.py <scripts folder>"""
import importlib.util
import os
import sys

scripts = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts")
spec = importlib.util.spec_from_file_location("kanji_reading", os.path.join(scripts, "kanji_reading.py"))
k = importlib.util.module_from_spec(spec)
spec.loader.exec_module(k)

cases = [
    ("いつて", "いって"), ("わかつて", "わかって"), ("まつたく", "まったく"),   # lyrics sites write the full-size tsu
    ("かつて", "かつて"), ("つて", "つて"), ("あつ", "あつ"),                      # "formerly" and a word start stay
    ("いって", "いって"),
    ("コーヒーのむのよ", "コーヒー飲むのよ"), ("ビールのむ", "ビール飲む"),
    ("夜の向こう", "夜の向こう"), ("ののむ", "ののむ"),
]
bad = 0
for src, want in cases:
    got = k.fix_text(src)
    if got != want:
        print("FAIL: %r -> %r, wanted %r" % (src, got, want))
        bad += 1

# numbers against Japanese text are written out; Latin + digit (F1) and lone numbers stay
for src, want in [("1日", "いちにち"), ("3周", "さんしゅう"), ("2羽", "にわ"), ("3羽", "さんば"), ("6羽", "ろっぱ"), ("1回", "いっかい"),
                  ("3月4日", "さんがつよっか"), ("6時", "ろくじ"), ("4時", "よじ"), ("1人", "ひとり"), ("2020年", "にせんにじゅうねん"),
                  ("15ぴょこ", "じゅうごぴょこ"), ("3ぴょこ", "さんぴょこ"), ("F1ドライバー", "F1ドライバー"), ("abc 12 def", "abc 12 def"),
                  ("１日", "いちにち")]:
    got = k.fix_numbers(src.translate({0xFF10 + i: ord("0") + i for i in range(10)}))
    if got != want:
        print("FAIL: fix_numbers(%r) = %r, wanted %r" % (src, got, want))
        bad += 1
for src, want in [("ima wa nan de mo nai", "ima wa nandemo nai"), ("saigo no F 1 doraibaa", "saigo no F1 doraibaa"), ("nan de", "nan de")]:
    got = k.fix_romaji(src)
    if got != want:
        print("FAIL: fix_romaji(%r) = %r, wanted %r" % (src, got, want))
        bad += 1
try:   # with the optional packages: the word cutting and the loanword sounds
    import pykakasi
    from janome.tokenizer import Tokenizer
    kks, tok = pykakasi.kakasi(), Tokenizer()
    for src, want in [("いまはなんでもない1日が", "ima wa nandemo nai ichinichi ga"), ("まず3周まわって", "mazu sanshuu mawatte"),
                      ("やろうと思うことはいずれできるから", "yarou to omou koto wa izure dekiru kara"), ("分からないので", "wakaranai node"),
                      ("夜 街へでる らららら", "yoru machi e deru rararara"), ("パーティーでウィスキー", "paatii de wisukii"),
                      ("Tシャツを着る", "tiishatsu o kiru"),
                      ("パラッパ パラッパ (wow-oh)", "parappa parappa (wow-oh)"), ("Baby, love again 二人の愛", "Baby, love again futari no ai"),
                      ("せめてあなたをにくみたくはないの", "semete anata o nikumitaku wa nai no"), ("二十二世紀の愛", "nijuuni seiki no ai"),
                      ("潰れた分だけ", "tsubureta bun dake"), ("ニューヨークヘ", "nyuuyooku e"), ("サイボーグ・ペーパードライバー", "saiboogu peepaa doraibaa"),
                      ("小さな島一人きり", "chiisana shima hitori kiri"), ("もう一度だけ", "mou ichido dake"), ("頬づえつきながら", "hoozuetsukinagara")]:
        got = k.long_to_plain(k.read_words(kks, tok, src))   # (the markers of long vowels, written as the plain style does)
        if got != want:
            print("FAIL: read_words(%r) = %r, wanted %r" % (src, got, want))
            bad += 1
    for src, want in [("やろうと思うこと", "yarō to omō koto"), ("コーヒーのむのよ", "kōhī nomu no yo"), ("今日は大きな", "kyō wa ōkina"),
                      ("パーティーでウィスキー", "pātī de wisukī"), ("お母さん", "okaasan")]:
        got = k.long_to_macron(k.read_words(kks, tok, src))   # the macron style
        if got != want:
            print("FAIL: macron style of %r = %r, wanted %r" % (src, got, want))
            bad += 1
except ImportError:
    pass
print("all ok" if not bad else "%d failed" % bad)
sys.exit(1 if bad else 0)
