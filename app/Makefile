# vita-turbo-vpn app: host targets (macOS or Linux, any C99 compiler).
#
#   make shots     render every screen with the mock back end to build-host/shots/
#   make shots SHOTS_LANG=ja   the same in another language (src/lang/<code>.def),
#                  to build-host/shots-ja/; Russian, Japanese, Korean and
#                  Chinese need the fallback fonts first
#   make shots-all render every language, listing layout and glyph problems
#   make fallback-fonts  fetch the Noto fonts the host uses where the app's
#                  fonts have no glyph (screenshots only; not in the VPK)
#   make check-strings   check every language against English
#   make shots BRAND=B  the same with a brand-colour option (ui.h BRAND_LEVEL):
#                  none, A, B, C, or D (A with red as the trouble colour), to
#                  build-host/shots-brand-<x>/
#   make shots-files  also render the Profiles screens from real files: a
#                  made-up profiles tree (tools/make_fixture.py) read through
#                  the same code as on the console
#   make livearea  regenerate the LiveArea images in sce_sys/
#   make vita      build the VPK in the pinned VitaSDK Docker image (build.sh)
#   make clean

CC      ?= cc
CFLAGS  ?= -O2
QUIRC   := ../third_party/quirc/lib
MINIZ   := third_party/miniz
CONF    := ../conf
# miniz as the app uses it: reading zips in memory only
MZ_DEFS := -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_DEFLATE_APIS -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES
CFLAGS  += -std=c99 -Wall -Wextra -DHOST_BUILD -Isrc -Ithird_party/stb -I$(MINIZ) -I$(CONF) \
           -I$(QUIRC) -I../qr $(MZ_DEFS)
OUT     := build-host
FONTS   := ../third_party/fonts

SRC := src/app.c src/screens.c src/ui.c src/draw.c src/draw_host.c src/strings.c \
       src/backend_mock.c src/profiles.c src/qr.c src/camera_host.c src/stb_impl.c src/main_host.c \
       $(CONF)/vtconf.c $(CONF)/vtconf_util.c
HDR := $(wildcard src/*.h) src/strings_en.def $(wildcard src/lang/*.def) $(CONF)/vtconf.h
LANGS := ja fr es de it nl pt ru ko zht zhs fi sv da no pl ptbr engb tr
SHOTS_LANG ?=
BRAND ?=
SHOTS_DIR := $(OUT)/shots$(if $(SHOTS_LANG),-$(SHOTS_LANG))$(if $(BRAND),-brand-$(BRAND))
SHOTS_BIN := $(OUT)/vpnturbo-shots$(if $(BRAND),-brand-$(BRAND))

# Brand-colour options (ui.h): how far Signal red reaches
BRAND_none := -DBRAND_LEVEL=0
BRAND_A    := -DBRAND_LEVEL=1
BRAND_B    := -DBRAND_LEVEL=2
BRAND_C    := -DBRAND_LEVEL=3
BRAND_D    := -DBRAND_LEVEL=1 -DPAL_BAD=0xdc1c2c -DPAL_BAD_EDGE=0x5c1218

# Third-party code, built as it comes: only its known benign warnings are off
TP_FLAGS := -std=c99 -O2 -I$(QUIRC) -Wno-sign-compare -Wno-unused-parameter -Wno-type-limits
TP_OBJ := $(OUT)/tp/decode.o $(OUT)/tp/identify.o $(OUT)/tp/quirc.o $(OUT)/tp/version_db.o \
          $(OUT)/tp/identify_relaxed.o $(OUT)/tp/miniz.o

SHOTS := main profiles migrate leak disconnected

.PHONY: shots shots-all shots-files fallback-fonts check-strings livearea vita release clean

$(OUT)/vpnturbo-shots: $(SRC) $(HDR) $(TP_OBJ) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(TP_OBJ) -lm

$(OUT)/vpnturbo-shots-brand-%: $(SRC) $(HDR) $(TP_OBJ) | $(OUT)
	$(CC) $(CFLAGS) $(BRAND_$*) -o $@ $(SRC) $(TP_OBJ) -lm

$(OUT)/tp/%.o: $(QUIRC)/%.c | $(OUT)/tp
	$(CC) $(TP_FLAGS) -c -o $@ $<

# quirc's relaxed finder grouping (../qr/quirc_relaxed.h), generated from the
# vendored identify.c as the console build does
$(OUT)/tp/identify_relaxed.c: $(QUIRC)/identify.c | $(OUT)/tp
	grep -q 'squareness < (quirc_float_t)0.2)' $< || { echo "quirc identify.c changed: update the relaxed patch"; exit 1; }
	sed 's/squareness < (quirc_float_t)0\.2)/squareness < (quirc_float_t)0.6)/' $< > $@

$(OUT)/tp/identify_relaxed.o: $(OUT)/tp/identify_relaxed.c
	$(CC) $(TP_FLAGS) -Dquirc_begin=quirc_begin_relaxed -Dquirc_end=quirc_end_relaxed \
		-Dquirc_extract=quirc_extract_relaxed -c -o $@ $<

$(OUT)/tp/miniz.o: $(MINIZ)/miniz.c | $(OUT)/tp
	$(CC) -std=c99 -O2 $(MZ_DEFS) -w -c -o $@ $<

$(OUT)/tp:
	mkdir -p $@

# the font files flat in one folder, as on the console (app0:fonts/)
$(OUT)/fonts/.stamp: $(wildcard $(FONTS)/*/*.ttf) | $(OUT)
	mkdir -p $(OUT)/fonts
	cp $(FONTS)/*/*.ttf $(OUT)/fonts/
	touch $@

$(OUT):
	mkdir -p $(OUT)

shots: $(SHOTS_BIN) $(OUT)/fonts/.stamp
	rm -rf $(SHOTS_DIR) && mkdir -p $(SHOTS_DIR)
	$(SHOTS_BIN) $(if $(SHOTS_LANG),--lang $(SHOTS_LANG)) $(OUT)/fonts/ $(SHOTS_DIR)

# A made-up profiles tree (loose files, a folder, a zip, a name on two
# storages, an invalid file, one with ignored lines, one that clashes),
# made afresh each time since the import shots write into it
shots-files: $(OUT)/vpnturbo-shots $(OUT)/fonts/.stamp
	rm -rf $(OUT)/fixture $(OUT)/shots-files && mkdir -p $(OUT)/shots-files
	python3 tools/make_fixture.py $(OUT)/fixture
	$(OUT)/vpnturbo-shots --profiles $(OUT)/fixture $(OUT)/fonts/ $(OUT)/shots-files


shots-all: $(OUT)/vpnturbo-shots $(OUT)/fonts/.stamp
	@fail=0; for l in en $(LANGS); do \
		rm -rf $(OUT)/shots-$$l && mkdir -p $(OUT)/shots-$$l; \
		$(OUT)/vpnturbo-shots --lang $$l $(OUT)/fonts/ $(OUT)/shots-$$l > $(OUT)/shots-$$l.log || fail=1; \
		grep -v '^  $(OUT)/' $(OUT)/shots-$$l.log || true; \
	done; exit $$fail

fallback-fonts:
	tools/fetch_fallback_fonts.sh $(OUT)/fonts/fallback

check-strings:
	python3 tools/check_strings.py


livearea: $(OUT)/vpnturbo-shots $(OUT)/fonts/.stamp
	$(OUT)/vpnturbo-shots $(OUT)/fonts/ --livearea sce_sys

vita:
	./build.sh

release:
	./build.sh release

clean:
	rm -rf $(OUT) build
