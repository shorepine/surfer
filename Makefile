# surfer — desktop build & test loop (see CLAUDE.md)

# Build on every core. Nothing here is serial by nature and this is a
# laptop with ten of them; MAKEFLAGS so a sub-make gets the jobserver
# too, and so `make -j2` on the command line still wins.
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(JOBS)

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Iinclude -Itools

CORE_SRCS   := $(wildcard src/core/*.c) $(wildcard src/text/*.c)
WIDGET_SRCS := $(wildcard src/widgets/*.c)
SDL_SRCS    := $(wildcard src/hal/sdl/*.c)
HDRS        := include/surfer.h src/core/surf_internal.h src/hal/sdl/hal_sdl.h

SDL_CFLAGS ?= $(shell sdl2-config --cflags)
SDL_LIBS   ?= $(shell sdl2-config --libs)
AR         ?= ar

GEN_DIR := build/gen

.PHONY: sdl test test-sdl clean

sdl: build/surfer_demo build/surfer_settings build/surfer_type build/surfer_editor \
	build/surfer_bounce build/surfer_fonts build/surfer_emoji

test: build/surfer_test
	./build/surfer_test

# the runtime-baked strips' REFERENCE, for the test build only (see
# tools/gen_widget_assets.py ref()); nothing that ships includes it
$(GEN_DIR)/widget_art_ref.h: tools/gen_widget_assets.py
	@mkdir -p $(GEN_DIR)
	python3 tools/gen_widget_assets.py --ref > $@

$(GEN_DIR)/bounce_assets.h: tools/gen_demo_assets.py
	@mkdir -p $(GEN_DIR)
	python3 tools/gen_demo_assets.py > $@

$(GEN_DIR)/widget_assets.h: tools/gen_widget_assets.py
	@mkdir -p $(GEN_DIR)
	python3 tools/gen_widget_assets.py > $@

# fontbake is a host tool, so FreeType here is a BUILD dependency only —
# nothing links it at runtime, the device still blits the baked A8 atlas.
# Optional on purpose: without it the build still works, it just bakes
# unhinted (fuzzier) outlines and says so.
FT_CFLAGS := $(shell pkg-config --cflags freetype2 2>/dev/null)
FT_LIBS   := $(shell pkg-config --libs freetype2 2>/dev/null)
ifneq ($(FT_LIBS),)
FT_DEF := -DSURF_FONTBAKE_FT
# Full autohinting, not "light": light grid-fits vertically only, and the
# problem at UI sizes is horizontal — a stem narrower than a pixel spread
# over two columns at 30-50% each. Roboto's 'l' at 15 ppem goes from
# `220 128` to `68 255 24` under full.
HINT := FONTBAKE_HINT=full
else
HINT :=
endif

build/tools/fontbake: tools/fontbake.c tools/stb/stb_truetype.h
	@mkdir -p build/tools
	$(CC) -O2 -Itools $(FT_DEF) $(FT_CFLAGS) -o $@ tools/fontbake.c $(FT_LIBS) -lm
ifeq ($(FT_LIBS),)
	@echo "NOTE: no freetype2 via pkg-config — baking unhinted. Small text"
	@echo "      will be fuzzy; 'brew install freetype' and rebuild to fix."
endif

# Emoji are baked by their own tool: a colour picture set shares none of
# fontbake's rasterizers, hinting, kerning or A1 packing (see the header
# of tools/emojibake.c). stb_image is already vendored for build tools.
build/tools/emojibake: tools/emojibake.c tools/stb/stb_image.h
	@mkdir -p build/tools
	$(CC) -O2 -Itools -Iinclude -o $@ tools/emojibake.c -lm

# TWO SIZES, and the number is a FLASH decision rather than a design
# one. An emoji is ARGB — 4 bytes a pixel, against A8's 1 and A1's 1/8 —
# so the set costs 284 KB at 12px, 474 at 16 and 998 at 24, against
# 1.25 MiB for all 45 text faces put together. There is no packing trick:
# the A1 saving fontbake gets is measured on a mask and cannot apply to
# a picture.
#
# There WAS a 24, and it is the right size for the display ramp — the
# registry gives each face the largest set that fits its line box, so
# ui23 and up wore a 24 and looked right. It came out because tulip5's
# P4X app partition is 7 MiB and the image reached 99% of it: 77 KB
# spare is one font away from a build that does not link, and the 16 MiB
# flash is fully allocated, so growing the partition reformats the
# board. Dropping it bought 998 KB back.
#
# What that costs is visible rather than subtle: every face above ui16
# now wears a 16px emoji, which beside 28px text reads as a slightly
# small picture. fontshow's emoji tab shows exactly this and says
# so. The other lever is assets/emoji/set.txt, which scales both sizes
# together — the top 100 emoji are ~82% of all use.
EMOJI_NAMES := emoji12 emoji16
EMOJI_GEN := $(addprefix $(GEN_DIR)/font_,$(addsuffix .h,$(EMOJI_NAMES)))
EMOJI_SRC := assets/emoji/set.txt

# emoji16 is the one that also emits the NAME table, because the names
# belong to the SET and not to any one size — see emojibake.c.
$(GEN_DIR)/font_emoji16.h $(GEN_DIR)/emoji_names.h: build/tools/emojibake $(EMOJI_SRC)
	@mkdir -p $(GEN_DIR)
	build/tools/emojibake emoji16 16 $(EMOJI_SRC) assets/emoji/png \
		$(GEN_DIR)/font_emoji16.h $(GEN_DIR)/emoji_names.h

$(GEN_DIR)/font_emoji%.h: build/tools/emojibake $(EMOJI_SRC)
	@mkdir -p $(GEN_DIR)
	build/tools/emojibake emoji$* $* $(EMOJI_SRC) assets/emoji/png $@

# ---- the UI ramp. fontbake's SIZE is ppem, so these names finally mean
# what they say; ui16 is ~10pt at the desktop window's 110-140dpi (which
# varies with the display-scaling setting) and ui23 is the same PHYSICAL
# size on the P4's 1024x600 7" panel (169dpi), which is why both exist
# rather than one being scaled to the other. ----
$(GEN_DIR)/font_ui16.h: build/tools/fontbake assets/fonts/Roboto-Regular.ttf
	@mkdir -p $(GEN_DIR)
	$(HINT) build/tools/fontbake ui16 16 assets/fonts/Roboto-Regular.ttf $@

$(GEN_DIR)/font_ui23.h: build/tools/fontbake assets/fonts/Roboto-Regular.ttf
	@mkdir -p $(GEN_DIR)
	$(HINT) build/tools/fontbake ui23 23 assets/fonts/Roboto-Regular.ttf $@

$(GEN_DIR)/font_ui28.h: build/tools/fontbake assets/fonts/Roboto-Regular.ttf
	@mkdir -p $(GEN_DIR)
	$(HINT) build/tools/fontbake ui28 28 assets/fonts/Roboto-Regular.ttf $@

# Roboto at display sizes. Plain AA, no threshold: at 36 and 48 px the
# curves carry enough pixels that partial coverage reads as a smooth edge
# rather than the lumpiness thresholding an off-grid outline produces —
# the opposite of the small-size case (see ui16b). fontbake's gray% on
# these is expected to be high and is not a warning here.
$(GEN_DIR)/font_ui36.h: build/tools/fontbake assets/fonts/Roboto-Regular.ttf
	@mkdir -p $(GEN_DIR)
	$(HINT) build/tools/fontbake ui36 36 assets/fonts/Roboto-Regular.ttf $@

$(GEN_DIR)/font_ui48.h: build/tools/fontbake assets/fonts/Roboto-Regular.ttf
	@mkdir -p $(GEN_DIR)
	$(HINT) build/tools/fontbake ui48 48 assets/fonts/Roboto-Regular.ttf $@

# THE HOUSE MONO MUST CARRY CP437, and JetBrains Mono does not. It is
# missing 21 of SURF_RANGE_MONO — the smileys, all four card suits, both
# music notes, the sun, the gender signs, the inverse bullets — i.e.
# exactly the half of the codepage a terminal face is wanted for. The
# range was applied to it regardless, so the bake emitted .notdef and
# `font.codepoints()` reported 256 while drawing tofu; friend's little
# note animation came back off the bench as two boxes.
#
# DejaVu Sans Mono covers the whole range, and at 0.602 em advance
# against JetBrains' 0.600 the CELL WIDTH is unchanged — so every column
# count computed from cell_w (the console, the editor, friend, tracker)
# stays where it was. It is shorter, so 16 ppem yields more ROWS than
# before; that is a strict improvement for a console and costs no layout,
# because everything here sizes itself from the cell rather than
# hardcoding it.
$(GEN_DIR)/font_mono16.h: build/tools/fontbake assets/fonts/DejaVuSansMono.ttf
	@mkdir -p $(GEN_DIR)
	$(HINT) build/tools/fontbake mono16 16 assets/fonts/DejaVuSansMono.ttf $@ mono

# ---- font-specimen bakes (demos/fonts.c): the same faces through the
# knobs that matter — plain AA, gamma-boosted AA, 1-bit, and a true
# bitmap face at its design size. These deliberately stay UNHINTED: each
# one exists to show what its knob does to a raw outline, and hinting
# them would change what the specimen demonstrates. ui12 is the exception
# — it is a real UI size (dense chrome), not a specimen. ----
$(GEN_DIR)/font_ui12.h: build/tools/fontbake assets/fonts/Roboto-Regular.ttf
	@mkdir -p $(GEN_DIR)
	$(HINT) build/tools/fontbake ui12 12 assets/fonts/Roboto-Regular.ttf $@

$(GEN_DIR)/font_ui16b.h: build/tools/fontbake assets/fonts/Roboto-Regular.ttf
	@mkdir -p $(GEN_DIR)
	FONTBAKE_THRESHOLD=1 build/tools/fontbake ui16b 16 \
		assets/fonts/Roboto-Regular.ttf $@

$(GEN_DIR)/font_bigblue12.h: build/tools/fontbake assets/fonts/BigBlue_TerminalPlus.ttf
	@mkdir -p $(GEN_DIR)
	FONTBAKE_THRESHOLD=1 build/tools/fontbake bigblue12 12 \
		assets/fonts/BigBlue_TerminalPlus.ttf $@ mono

# Pixel-designed proportional faces (Kenney, CC0). These are drawn on a
# pixel grid defined in em units, so only an exact ppem lands stems on
# whole pixels — which is now what SIZE means, so there is no flag to
# set. (This case is why the default flipped: these bakes had to opt in
# to the sane sizing while everything else silently got the other one.)
# At these sizes fontbake reports gray 0.0% — no partial coverage at all,
# which is what "actually a bitmap font" means. Off-grid sizes (12, 20)
# come out 40-70% gray and look lumpy thresholded. Never hinted: the
# grid-fit they want is the one they were drawn on.
KPIX_BAKE = FONTBAKE_THRESHOLD=1 build/tools/fontbake

$(GEN_DIR)/font_kmini16.h: build/tools/fontbake assets/fonts/KenneyMini.ttf
	@mkdir -p $(GEN_DIR)
	$(KPIX_BAKE) kmini16 16 assets/fonts/KenneyMini.ttf $@

$(GEN_DIR)/font_kmini32.h: build/tools/fontbake assets/fonts/KenneyMini.ttf
	@mkdir -p $(GEN_DIR)
	$(KPIX_BAKE) kmini32 32 assets/fonts/KenneyMini.ttf $@

$(GEN_DIR)/font_khigh32.h: build/tools/fontbake assets/fonts/KenneyHigh.ttf
	@mkdir -p $(GEN_DIR)
	$(KPIX_BAKE) khigh32 32 assets/fonts/KenneyHigh.ttf $@

$(GEN_DIR)/font_kblocks16.h: build/tools/fontbake assets/fonts/KenneyBlocks.ttf
	@mkdir -p $(GEN_DIR)
	$(KPIX_BAKE) kblocks16 16 assets/fonts/KenneyBlocks.ttf $@

# Kenney Pixel size ramp: multiples of its em16 grid, all exact
$(GEN_DIR)/font_kpixel16.h: build/tools/fontbake assets/fonts/KenneyPixel.ttf
	@mkdir -p $(GEN_DIR)
	$(KPIX_BAKE) kpixel16 16 assets/fonts/KenneyPixel.ttf $@

$(GEN_DIR)/font_kpixel32.h: build/tools/fontbake assets/fonts/KenneyPixel.ttf
	@mkdir -p $(GEN_DIR)
	$(KPIX_BAKE) kpixel32 32 assets/fonts/KenneyPixel.ttf $@

$(GEN_DIR)/font_kpixel48.h: build/tools/fontbake assets/fonts/KenneyPixel.ttf
	@mkdir -p $(GEN_DIR)
	$(KPIX_BAKE) kpixel48 48 assets/fonts/KenneyPixel.ttf $@

$(GEN_DIR)/font_kpixel64.h: build/tools/fontbake assets/fonts/KenneyPixel.ttf
	@mkdir -p $(GEN_DIR)
	$(KPIX_BAKE) kpixel64 64 assets/fonts/KenneyPixel.ttf $@

# ---- the oldschool PC faces (VileR's Ultimate Oldschool PC Font
# Pack v2.2, CC BY-SA 4.0 -- assets/fonts/oldschool/). Every one is a
# TEXT-MODE ROM face traced onto its own pixel grid, so it is baked
# at ppem = unitsPerEm/100 and NOT hinted: at that exact size it is
# 0.0% gray and lands on the 1-bit path, and FreeType's autohinter
# would only smear a face that is already on the grid.
# `mono` is ASCII + CP437 -- what these were drawn for. ----

$(GEN_DIR)/font_portfolio6x8.h: build/tools/fontbake assets/fonts/oldschool/Px437_Portfolio_6x8.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake portfolio6x8 8 assets/fonts/oldschool/Px437_Portfolio_6x8.ttf $@ mono

$(GEN_DIR)/font_apricot8x10.h: build/tools/fontbake assets/fonts/oldschool/Px437_Apricot_256L.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake apricot8x10 12 assets/fonts/oldschool/Px437_Apricot_256L.ttf $@ mono

$(GEN_DIR)/font_phoenix8x8.h: build/tools/fontbake assets/fonts/oldschool/Px437_PhoenixVGA_8x8.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake phoenix8x8 8 assets/fonts/oldschool/Px437_PhoenixVGA_8x8.ttf $@ mono

$(GEN_DIR)/font_xga7x15.h: build/tools/fontbake assets/fonts/oldschool/Px437_IBM_XGA-AI_7x15.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake xga7x15 16 assets/fonts/oldschool/Px437_IBM_XGA-AI_7x15.ttf $@ mono

$(GEN_DIR)/font_robotron8x16.h: build/tools/fontbake assets/fonts/oldschool/Px437_Robotron_A7100.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake robotron8x16 16 assets/fonts/oldschool/Px437_Robotron_A7100.ttf $@ mono

$(GEN_DIR)/font_fmtowns8x16.h: build/tools/fontbake assets/fonts/oldschool/Px437_FMTowns_re_8x16.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake fmtowns8x16 16 assets/fonts/oldschool/Px437_FMTowns_re_8x16.ttf $@ mono

$(GEN_DIR)/font_toshiba9x16.h: build/tools/fontbake assets/fonts/oldschool/PxPlus_ToshibaSat_9x16.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake toshiba9x16 16 assets/fonts/oldschool/PxPlus_ToshibaSat_9x16.ttf $@ mono

$(GEN_DIR)/font_dosv12x24.h: build/tools/fontbake assets/fonts/oldschool/Px437_DOS-V_re_JPN24.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake dosv12x24 24 assets/fonts/oldschool/Px437_DOS-V_re_JPN24.ttf $@ mono

$(GEN_DIR)/font_cordata16x26.h: build/tools/fontbake assets/fonts/oldschool/PxPlus_Cordata_PPC-21.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake cordata16x26 32 assets/fonts/oldschool/PxPlus_Cordata_PPC-21.ttf $@ mono

$(GEN_DIR)/font_wyse16x32.h: build/tools/fontbake assets/fonts/oldschool/Px437_Wyse700a-2y.ttf
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake wyse16x32 32 assets/fonts/oldschool/Px437_Wyse700a-2y.ttf $@ mono

# helvR12 are separately drawn faces, not one outline scaled, which is the
# whole reason they read better than a thresholded outline at small sizes.
# Adobe X11 bitmap fonts (BDF): fontbake copies these pixel-for-pixel, so
# the SIZE argument is ignored — a BDF *is* one designed size. helvR10 and
# helvR12 are separately drawn faces, not one outline scaled, which is the
# whole reason they read better than a thresholded outline at small sizes.
# Pattern rule: explicit rules above win for the TTF bakes.
# The Adobe BDFs get `base` only. They are ISO10646-1 and carry Latin-1
# comfortably, but not CP437's box drawing — courR came out 232 glyphs of
# a requested 299, i.e. a third of what a terminal wants simply absent.
# These are desktop-only (the device bakes no BDF), so this costs no
# flash either way; the mono terminal faces are bigblue and JetBrains.
$(GEN_DIR)/font_%.h: assets/fonts/bdf/%.bdf build/tools/fontbake
	@mkdir -p $(GEN_DIR)
	build/tools/fontbake $* 0 $< $@ base

# all 24 Adobe X11 BDFs (4 families x 6 designed sizes)
ADOBE_NAMES := $(foreach f,helvR helvB ncenR,\
	$(foreach s,08 10 12 14 18 24,$(f)$(s)))
ADOBE_GEN := $(addprefix $(GEN_DIR)/font_,$(addsuffix .h,$(ADOBE_NAMES)))

# every font this build ships, by fontbake name. The registry
# (surf_font_builtin) is generated from exactly this list.
TTF_NAMES := ui12 ui16 ui16b ui23 ui28 ui36 ui48 \
	kpixel16 kpixel32 kpixel48 kpixel64 kmini16 kmini32 khigh32 kblocks16 \
	mono16 bigblue12 \
	portfolio6x8 apricot8x10 phoenix8x8 xga7x15 robotron8x16 fmtowns8x16 toshiba9x16 dosv12x24 cordata16x26 wyse16x32
FONT_NAMES := $(TTF_NAMES) $(ADOBE_NAMES) $(EMOJI_NAMES)

FONTLAB_GEN := $(GEN_DIR)/font_ui12.h $(GEN_DIR)/font_ui16.h \
	$(GEN_DIR)/font_ui16b.h $(GEN_DIR)/font_ui23.h $(GEN_DIR)/font_ui28.h \
	$(GEN_DIR)/font_ui36.h $(GEN_DIR)/font_ui48.h \
	$(GEN_DIR)/font_mono16.h $(GEN_DIR)/font_bigblue12.h \
	$(GEN_DIR)/font_portfolio6x8.h $(GEN_DIR)/font_apricot8x10.h \
	$(GEN_DIR)/font_phoenix8x8.h $(GEN_DIR)/font_xga7x15.h \
	$(GEN_DIR)/font_robotron8x16.h $(GEN_DIR)/font_fmtowns8x16.h \
	$(GEN_DIR)/font_toshiba9x16.h $(GEN_DIR)/font_dosv12x24.h \
	$(GEN_DIR)/font_cordata16x26.h $(GEN_DIR)/font_wyse16x32.h \
	$(GEN_DIR)/font_kpixel16.h $(GEN_DIR)/font_kpixel32.h \
	$(GEN_DIR)/font_kpixel48.h $(GEN_DIR)/font_kpixel64.h \
	$(GEN_DIR)/font_kmini16.h $(GEN_DIR)/font_kmini32.h \
	$(GEN_DIR)/font_khigh32.h $(GEN_DIR)/font_kblocks16.h \
	$(ADOBE_GEN) $(EMOJI_GEN) $(GEN_DIR)/emoji_names.h

# NOTE this rule must come AFTER FONTLAB_GEN is defined: make expands a
# rule's prerequisites when it parses the rule, so with the definition
# below it $(FONTLAB_GEN) was empty and the registry never rebuilt when
# the font list changed — adding a name to TTF_NAMES baked the new atlas
# and then silently kept the old registry.
#
# ...and the MAKEFILE itself is a prerequisite, which is the other half
# of the same bug. Depending on the baked headers catches a name being
# ADDED, because that bakes a new file; it cannot catch one being
# REMOVED, since nothing the registry depends on is touched. Taking
# emoji24 out of EMOJI_NAMES left a registry still including and
# registering a header the build no longer makes — which fails at the
# NEXT clean build rather than this one, i.e. on somebody else's
# machine.
$(GEN_DIR)/font_registry.c: tools/gen_font_registry.py Makefile $(FONTLAB_GEN)
	@mkdir -p $(GEN_DIR)
	python3 tools/gen_font_registry.py $(FONT_NAMES) > $@

build/surfer_emoji: $(CORE_SRCS) $(SDL_SRCS) demos/emoji.c \
		$(GEN_DIR)/font_registry.c $(FONTLAB_GEN) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(SDL_SRCS) demos/emoji.c \
		$(GEN_DIR)/font_registry.c $(SDL_LIBS) -lm

# the scene lives in demos/fonts_scene.c — the P4 firmware builds the very
# same page from the same source (ports/esp32p4/main/app_main.c)
build/surfer_fonts: $(CORE_SRCS) $(SDL_SRCS) demos/fonts.c demos/fonts_scene.c \
		demos/fonts_scene.h $(GEN_DIR)/font_registry.c $(FONTLAB_GEN) $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) -Idemos \
		-o $@ $(CORE_SRCS) $(SDL_SRCS) demos/fonts.c demos/fonts_scene.c \
		$(GEN_DIR)/font_registry.c $(SDL_LIBS) -lm

# the "desktop demo" tracks the current milestone: mixer (M1) + text labels (M3)
build/surfer_demo: $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) demos/mixer.c \
		$(GEN_DIR)/widget_assets.h $(GEN_DIR)/font_ui16.h $(GEN_DIR)/font_ui28.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) demos/mixer.c $(SDL_LIBS) -lm

build/surfer_type: $(CORE_SRCS) $(SDL_SRCS) demos/type.c \
		$(GEN_DIR)/font_ui16.h $(GEN_DIR)/font_ui28.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(SDL_SRCS) demos/type.c $(SDL_LIBS) -lm

build/surfer_settings: $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) demos/settings.c \
		$(GEN_DIR)/widget_assets.h $(GEN_DIR)/font_ui16.h $(GEN_DIR)/font_ui28.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) demos/settings.c $(SDL_LIBS) -lm

build/surfer_editor: $(CORE_SRCS) $(SDL_SRCS) demos/editor.c \
		$(GEN_DIR)/font_mono16.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(SDL_SRCS) demos/editor.c $(SDL_LIBS) -lm

build/surfer_bounce: $(CORE_SRCS) $(SDL_SRCS) demos/bounce.c \
		$(GEN_DIR)/bounce_assets.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(SDL_SRCS) demos/bounce.c $(SDL_LIBS)

# the SDL tests open a real window and link SDL; the unit suite is the
# mock hal only, so they are named sdl_* and filtered out here
TEST_SRCS := $(filter-out tests/sdl_%.c,$(wildcard tests/*.c))

build/surfer_test: $(CORE_SRCS) $(WIDGET_SRCS) $(TEST_SRCS) tests/mock_hal.h $(HDRS) \
		$(GEN_DIR)/widget_art_ref.h
	@mkdir -p build
	$(CC) $(CFLAGS) -Isrc/core -Itests -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(WIDGET_SRCS) $(TEST_SRCS) -lm

# present-coherence regression (fb vs presented texture after fast
# scroll). Opens a real SDL window, so it's not part of plain `make test`.
test-sdl: build/surfer_present_test
	./build/surfer_present_test

build/surfer_aspect_test: $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) \
		tests/sdl_aspect_test.c $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) tests/sdl_aspect_test.c \
		$(SDL_LIBS) -lm

.PHONY: test-aspect
test-aspect: build/surfer_aspect_test
	./build/surfer_aspect_test

build/surfer_present_test: $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) \
		tests/sdl_present_test.c $(GEN_DIR)/font_mono16.h $(HDRS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -I$(GEN_DIR) \
		-o $@ $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) tests/sdl_present_test.c \
		$(SDL_LIBS) -lm

# static lib + generated headers for the MicroPython binding
LIB_SRCS := $(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) $(GEN_DIR)/font_registry.c
LIB_OBJS := $(patsubst %.c,build/obj/%.o,$(LIB_SRCS))

build/obj/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Isrc/core -Isrc/hal/sdl -c $< -o $@

build/libsurfer.a: gen $(LIB_OBJS)
	$(AR) rcs $@ $(LIB_OBJS)

gen: $(GEN_DIR)/widget_assets.h $(GEN_DIR)/font_registry.c $(FONTLAB_GEN)

# ---- web (M6): the sdl backend compiled with emscripten ----
# DESIGN.md's "zero new code" bet: demos compile unchanged; ASYNCIFY +
# the pacing yield in surf_hal_sdl_pump turn the desktop main loop into
# a browser-friendly one.

EMCC ?= emcc
WEB_DIR := build/web
WEB_CFLAGS := -O2 -std=gnu11 -Wall -Wextra -Iinclude -Itools -Isrc/core -Isrc/hal/sdl \
	-I$(GEN_DIR) -sUSE_SDL=2
WEB_LDFLAGS := -sASYNCIFY -sALLOW_MEMORY_GROWTH

web: gen
	@mkdir -p $(WEB_DIR)
	$(EMCC) $(WEB_CFLAGS) $(WEB_LDFLAGS) -o $(WEB_DIR)/mixer.html \
		$(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) demos/mixer.c
	$(EMCC) $(WEB_CFLAGS) $(WEB_LDFLAGS) -o $(WEB_DIR)/settings.html \
		$(CORE_SRCS) $(WIDGET_SRCS) $(SDL_SRCS) demos/settings.c
	@echo "→ $(WEB_DIR)/  (serve the directory; .html loads the .wasm)"

# tulip mode in the browser: micropython webassembly port + the surfer
# binding, via the "web" variant in bindings/surfer/web/. `import tulip`
# and gamma9001 are frozen in; index.html kicks it off.
EMAR ?= emar
# no SDL_SRCS here: hal_sdl.c holds an EM_ASYNC_JS whose JS body emcc
# drops when linked out of an archive — the binding compiles it directly
# (bindings/surfer/web/hal_sdl_web.c)
WEB_OBJS := $(patsubst %.c,build/webobj/%.o,$(CORE_SRCS) $(WIDGET_SRCS) $(GEN_DIR)/font_registry.c)

build/webobj/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(EMCC) $(WEB_CFLAGS) -c $< -o $@

build/libsurfer-web.a: gen $(WEB_OBJS)
	$(EMAR) rcs $@ $(WEB_OBJS)

mpy-web: build/libsurfer-web.a
	$(MAKE) -C $(MPY_DIR)/ports/webassembly \
		VARIANT_DIR=$(abspath bindings/surfer/web) \
		USER_C_MODULES=$(abspath bindings) \
		SURFER_DIR=$(abspath .)
	@mkdir -p $(WEB_DIR)
	cp $(MPY_DIR)/ports/webassembly/build-web/micropython.mjs \
	   $(MPY_DIR)/ports/webassembly/build-web/micropython.wasm \
	   bindings/surfer/web/index.html $(WEB_DIR)/
	@echo "→ $(WEB_DIR)/index.html  (serve $(WEB_DIR) and open it)"

.PHONY: web mpy-web

MPY_DIR ?= $(HOME)/micropython

mpy: build/libsurfer.a gen
	@# MP only knows libsurfer.a as a linker flag, not a dependency —
	@# drop the binary so a changed lib always relinks
	rm -f $(MPY_DIR)/ports/unix/build-standard/micropython
	$(MAKE) -C $(MPY_DIR)/ports/unix USER_C_MODULES=$(abspath bindings) \
		SURFER_DIR=$(abspath .) \
		CFLAGS_EXTRA="-Wno-gnu-folding-constant"  # newer clang vs MP v1.26
	@echo "→ $(MPY_DIR)/ports/unix/build-standard/micropython"

# MicroPython esp32 port for the P4 (tulip mode on device).
# Needs micropython v1.28.x (has ESP32-P4 support) + IDF v5.5.1 (MP's
# recommended version; its P4 code expects 5.5 APIs). The surfer-native
# firmware in ports/esp32p4/ stays on 5.4.1 independently.
MPY_P4_DIR ?= $(HOME)/micropython-1.28
IDF_EXPORT ?= $(HOME)/esp/esp-idf-v5.5.1/export.sh


.PHONY: gen mpy

clean:
	rm -rf build
