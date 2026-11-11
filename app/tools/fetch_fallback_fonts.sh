#!/bin/sh
# Fetch the fallback fonts the screenshot tool uses for scripts the app's
# own fonts lack (Cyrillic, Japanese, Korean, Chinese) into <dir>, checked
# against pinned SHA-256 sums. Screenshots only: the console uses its system
# fonts and the VPK does not carry these. Licences: ../third_party/fonts/LICENSES.md
#
#   tools/fetch_fallback_fonts.sh build-host/fonts/fallback
set -eu

DIR="${1:?usage: $0 <dir>}"
NOTO_SANS="https://github.com/notofonts/notofonts.github.io/raw/c1d3718e2a0eb25c2567a694ee017061fdaa5f51/fonts/NotoSans/unhinted/ttf"
NOTO_CJK="https://github.com/notofonts/noto-cjk/raw/Sans2.004"

mkdir -p "$DIR"
if command -v sha256sum >/dev/null 2>&1; then SUM="sha256sum"; else SUM="shasum -a 256"; fi

fetch() {   # url file sha256
	if [ -f "$DIR/$2" ] && [ "$($SUM "$DIR/$2" | cut -d' ' -f1)" = "$3" ]; then
		return
	fi
	echo "  fetching $2"
	curl -sSfL -o "$DIR/$2.part" "$1"
	if [ "$($SUM "$DIR/$2.part" | cut -d' ' -f1)" != "$3" ]; then
		echo "$2: checksum mismatch" >&2
		rm -f "$DIR/$2.part"
		exit 1
	fi
	mv "$DIR/$2.part" "$DIR/$2"
}

fetch "$NOTO_SANS/NotoSans-Regular.ttf" NotoSans-Regular.ttf f3961a9cde016d41a4879aecda1474d3a36d6bf54fa0e4643de029cc2248b0e8
fetch "$NOTO_SANS/NotoSans-Bold.ttf"    NotoSans-Bold.ttf    87cb2d84472a7d66da659ee47b6cdb9552326e8c128245231f191b6ac72529d9
fetch "$NOTO_CJK/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf" NotoSansJP-Regular.otf dff723ba59d57d136764a04b9b2d03205544f7cd785a711442d6d2d085ac5073
fetch "$NOTO_CJK/Sans/SubsetOTF/JP/NotoSansJP-Bold.otf"    NotoSansJP-Bold.otf    1b0edfb500b73a4fa8a4fcaae1bbbd403994e08e73e3e0da37e70d3853f42c5f
fetch "$NOTO_CJK/Sans/SubsetOTF/KR/NotoSansKR-Regular.otf" NotoSansKR-Regular.otf 69975a0ac8472717870aefeab0a4d52739308d90856b9955313b2ad5e0148d68
fetch "$NOTO_CJK/Sans/SubsetOTF/KR/NotoSansKR-Bold.otf"    NotoSansKR-Bold.otf    5a6ceb287ed2fc6cfc6213144ebea68cbd94b20fc9eb873d8486493bf02d9bda
fetch "$NOTO_CJK/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf" NotoSansSC-Regular.otf faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9
fetch "$NOTO_CJK/Sans/SubsetOTF/SC/NotoSansSC-Bold.otf"    NotoSansSC-Bold.otf    c6cb5a93abaa9edc8ee7463b7ebb7f42d618d40e6ed2f7a5371c97b0b64767c0
fetch "$NOTO_CJK/Sans/SubsetOTF/TC/NotoSansTC-Regular.otf" NotoSansTC-Regular.otf 5bab0cb3c1cf89dde07c4a95a4054b195afbcfe784d69d75c340780712237537
fetch "$NOTO_CJK/Sans/SubsetOTF/TC/NotoSansTC-Bold.otf"    NotoSansTC-Bold.otf    55420b259eb119bf5f2a0aadba10cf9d736c12d64ab93e78546d69ef5f43558b
fetch "https://github.com/notofonts/latin-greek-cyrillic/raw/NotoSans-v2.015/OFL.txt" OFL-NotoSans.txt cee9892f9f0cc8fe882c9e9537ee6a89621d86ee7ceaf70b02e2b2b1c25c061a
fetch "$NOTO_CJK/LICENSE" OFL-NotoSansCJK.txt 6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2
