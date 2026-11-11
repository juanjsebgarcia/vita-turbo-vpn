#!/usr/bin/env python3
"""check_strings.py - checks every language table (src/lang/<code>.def)
against English (src/strings_en.def):

  - every id exists in English and appears once;
  - the placeholders {0} and {1} are the same set as in English;
  - the text is valid UTF-8, written as characters rather than \\x escapes
    (an escape followed by a hex digit would swallow it);
  - no leading or trailing spaces unless English has them.

It prints, per language, how many strings are translated. Exit status 1 on
any error.

    python3 tools/check_strings.py         (from app/)
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
LANGS = ["ja", "fr", "es", "de", "it", "nl", "pt", "ru", "ko", "zht", "zhs",
         "fi", "sv", "da", "no", "pl", "ptbr", "engb", "tr"]
# not text (names, file names, URLs, symbols, the mock's scenario names) or
# usually the same in every language; a language may leave them out
FIXED = {"S_APP_DISPLAY_NAME", "S_TOAST_SCENARIO", "S_SEG_WIFI", "S_JOIN", "S_DNS", "S_ROW_DNS", "S_ROW_MTU", "S_PROJECT_NAME", "S_VERSION", "S_MODULE_FILE", "S_PLUGIN_FILE",
         "S_DASH", "S_ELLIPSIS", "S_KEY_L", "S_KEY_R", "S_KEY_START",
         "S_GROUP_TOP"}

ENTRY = re.compile(r'X\(\s*(S_[A-Z0-9_]+)\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)', re.S)
LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')


def unescape(lit):
    out = bytearray()
    i = 0
    raw = lit.encode("utf-8")
    while i < len(raw):
        c = raw[i]
        if c == 0x5C:  # backslash
            n = raw[i + 1]
            if n == ord("x"):
                j = i + 2
                while j < len(raw) and chr(raw[j]) in "0123456789abcdefABCDEF":
                    j += 1
                out.append(int(raw[i + 2:j], 16) & 0xFF)
                i = j
                continue
            out.append({ord("n"): 10, ord("t"): 9}.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return bytes(out)


def parse(path):
    text = open(path, encoding="utf-8").read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    entries = []
    for m in ENTRY.finditer(text):
        lits = LIT.findall(m.group(2))
        entries.append((m.group(1), lits, unescape("".join(lits))))
    return entries


def placeholders(s):
    return sorted(re.findall(r"\{[01]\}", s))


def main():
    en = {k: b.decode("utf-8") for k, _, b in parse(os.path.join(SRC, "strings_en.def"))}
    errors = 0
    for lang in LANGS:
        path = os.path.join(SRC, "lang", lang + ".def")
        if not os.path.exists(path):
            print(f"{lang}: missing {path}")
            errors += 1
            continue
        seen = set()
        for k, lits, raw in parse(path):
            where = f"{lang}: {k}"
            if k not in en:
                print(f"{where}: no such id in English"); errors += 1; continue
            if k in seen:
                print(f"{where}: appears twice"); errors += 1
            seen.add(k)
            try:
                s = raw.decode("utf-8")
            except UnicodeDecodeError:
                print(f"{where}: not valid UTF-8"); errors += 1; continue
            for lit in lits:
                if re.search(r"\\x[0-9A-Fa-f]{2}[0-9A-Fa-f]", lit):
                    print(f"{where}: a \\x escape runs into a hex digit"); errors += 1
            if placeholders(s) != placeholders(en[k]):
                print(f"{where}: placeholders {placeholders(s)} but English has {placeholders(en[k])}")
                errors += 1
            if s != s.strip() and en[k] == en[k].strip():
                print(f"{where}: leading or trailing space"); errors += 1
            if not s:
                print(f"{where}: empty"); errors += 1
        todo = [k for k in en if k not in seen and k not in FIXED]
        print(f"{lang}: {len(seen)} strings, {len(todo)} left in English"
              + (f" ({', '.join(todo[:6])}{'...' if len(todo) > 6 else ''})" if todo and lang != 'engb' else ""))
    print(f"English: {len(en)} strings ({len(en) - len(FIXED)} to translate)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
