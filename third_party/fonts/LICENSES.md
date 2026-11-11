# Fonts

The app's three typefaces, vendored unmodified. All three
are under the SIL Open Font License 1.1: free to use, embed and
redistribute, including in commercial software, as long as the licence
travels with the fonts and they are not sold on their own. None declares a
Reserved Font Name. Each folder holds the licence text as published with
the font, and the VPK carries the same files under `app0:fonts/`.

| Folder | Files | Used for | Copyright | Licence | Source |
|---|---|---|---|---|---|
| `chakra-petch/` | `ChakraPetch-SemiBold.ttf`, `ChakraPetch-Bold.ttf` | titles, labels (600); status words, tube text (700) | 2018 The Chakra Petch Project Authors | OFL 1.1 (`OFL.txt`) | google/fonts, `ofl/chakrapetch/`, fetched 2026-10-29 |
| `barlow-semi-condensed/` | `BarlowSemiCondensed-Regular.ttf` | body text (400) | 2017 The Barlow Project Authors | OFL 1.1 (`OFL.txt`) | google/fonts, `ofl/barlowsemicondensed/`, fetched 2026-10-29 |
| `martian-mono/` | `MartianMono-StdRg.ttf`, `MartianMono-StdMd.ttf` | addresses (400); values that are the point (500) | 2021 The Martian Mono Project Authors | OFL 1.1 (`OFL.txt`) | evilmartians/mono release v1.1.0, `martian-mono-1.1.0-ttf.zip`, standard width |

Martian Mono comes from its own release rather than Google Fonts because
Google Fonts ships only the variable font, and the app's text renderer
(stb_truetype) draws a variable font at its default instance only. The
"Std" files are the standard width, which is the variable font's default.

Only the weights the design uses are vendored. To add one, take the static
TTF from the same source and add a row here.

SHA-256:

```
45264de3204ddbd5fb3e14a2402acd5c630d16650ae5fc221d2c52da46a6734b  chakra-petch/ChakraPetch-SemiBold.ttf
65fbf76d95651697275e19db4d717c0e95a789ddd3476478b05292104db278a0  chakra-petch/ChakraPetch-Bold.ttf
e8c7242f612b13d026e18a94b63b0fd079e91a3faf009d1b6981dbb5f16d1153  barlow-semi-condensed/BarlowSemiCondensed-Regular.ttf
7ccf33cd01e123d9bf0e0da695440351550c70e572951455a84d22cf55e1d9b4  martian-mono/MartianMono-StdRg.ttf
d4f2b5f9c2a389d1e559ab90fc3b2a121aa3e43838e8129fed229f5c07dc6236  martian-mono/MartianMono-StdMd.ttf
```

## Host-only fallback fonts (not vendored, not shipped)

The screenshot tool (`app/`, `make fallback-fonts`) fetches these
into `app/build-host/fonts/fallback/` to draw scripts the three faces above
lack (Cyrillic, Japanese, Korean, Chinese). They are used for host
screenshots only; the console draws those characters with its own system
fonts, and the VPK does not carry them. `app/tools/fetch_fallback_fonts.sh`
pins each file by SHA-256.

| Files | Copyright | Licence | Source |
|---|---|---|---|
| `NotoSans-Regular.ttf`, `NotoSans-Bold.ttf` (unhinted) | 2022 The Noto Project Authors | OFL 1.1 | notofonts/notofonts.github.io at c1d3718e, `fonts/NotoSans/unhinted/ttf/`; licence from notofonts/latin-greek-cyrillic tag NotoSans-v2.015 |
| `NotoSans{JP,KR,SC,TC}-{Regular,Bold}.otf` (region subsets) | 2014-2021 Adobe (http://www.adobe.com/) | OFL 1.1 | notofonts/noto-cjk tag Sans2.004, `Sans/SubsetOTF/` and `LICENSE` |
