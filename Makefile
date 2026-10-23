# Usage:
#   make                    build all (uses pre-generated code in generated/)
#   make regen              re-run the recompiler on all ROMs
#   make run GAME=sml       run a game (drmario / sml / sml2)
#   make win                cross-compile Windows exe with llvm-mingw
#
# The ROM files are expected at:
#   roms/dr_mario.gb  roms/sml.gb  roms/sml2.gb
#
ROM_DRMARIO ?= roms/dr_mario.gb
ROM_SML     ?= roms/sml.gb
ROM_SML2    ?= roms/sml2.gb

GEN         ?= generated
BUILD       ?= build
PYTHON      ?= python3

# ---------- native build -------------------------------------------------
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wno-format-truncation
DEFS    ?=

SDL_CFLAGS ?= $(shell sdl2-config --cflags 2>/dev/null)
SDL_LIBS   ?= $(shell sdl2-config --libs   2>/dev/null)
ifneq ($(strip $(SDL_LIBS)),)
  DEFS += -DUSE_SDL
endif

RT_SRC := $(wildcard runtime/*.c)

# Per-game generated sources
DRMARIO_SRC := $(GEN)/drmario/game.c $(GEN)/drmario/interp.c
SML_SRC     := $(GEN)/sml/game_banked.c \
               $(GEN)/sml/game_bank0.c \
               $(GEN)/sml/game_bank1.c \
               $(GEN)/sml/game_bank2.c \
               $(GEN)/sml/game_bank3.c \
               $(GEN)/sml/interp.c
SML2_SRC    := $(GEN)/sml2/game_banked.c \
               $(patsubst %,$(GEN)/sml2/game_bank%.c,\
                 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 \
                 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31) \
               $(GEN)/sml2/interp.c

GEN_SRC := $(DRMARIO_SRC) $(SML_SRC) $(SML2_SRC) $(GEN)/dispatch.c

# game_info.h: only one at a time is #include'd; we pick drmario's as the
# master (runtime doesn't actually use ROM_CRC32 from here — it uses the
# per-game crc in games[]).  The include path lists all three dirs so
# each game's headers are found.
INC := -Iruntime -I$(GEN)/drmario -I$(GEN)/sml -I$(GEN)/sml2

all: $(BUILD)/gb

# Bank files are huge switch statements; -O2 takes minutes on them. -O1 is fast
# and still produces good code for the tight inner loops.
BANK_CFLAGS := -O1 -Wall -Wno-format-truncation -Wno-unused-label

# Compile each bank file to its own .o so we can use different flags.
$(BUILD)/bank_%.o: $(GEN)/sml/game_bank%.c | $(BUILD)
	$(CC) $(BANK_CFLAGS) $(DEFS) $(INC) -c $< -o $@

$(BUILD)/sml2_bank_%.o: $(GEN)/sml2/game_bank%.c | $(BUILD)
	$(CC) $(BANK_CFLAGS) $(DEFS) $(INC) -c $< -o $@

SML_BANK_OBJS  := $(patsubst %,$(BUILD)/bank_%.o, 0 1 2 3)
SML2_BANK_OBJS := $(patsubst %,$(BUILD)/sml2_bank_%.o, \
                    0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 \
                    16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31)

# Non-bank generated sources (compiled at normal -O2)
NON_BANK_GEN := $(GEN)/drmario/game.c $(GEN)/drmario/interp.c \
                $(GEN)/sml/game_banked.c $(GEN)/sml/interp.c \
                $(GEN)/sml2/game_banked.c $(GEN)/sml2/interp.c \
                $(GEN)/dispatch.c

$(BUILD):
	mkdir -p $@

$(BUILD)/gb: $(RT_SRC) $(wildcard runtime/*.h) $(NON_BANK_GEN) $(SML_BANK_OBJS) $(SML2_BANK_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) $(DEFS) $(SDL_CFLAGS) $(INC) \
	    $(RT_SRC) $(NON_BANK_GEN) $(SML_BANK_OBJS) $(SML2_BANK_OBJS) \
	    -o $@ $(SDL_LIBS) -lm $(EXTRA_LIBS)

# ---------- regenerate from ROM files ------------------------------------
regen: regen-drmario regen-sml regen-sml2

regen-drmario: $(ROM_DRMARIO)
	$(PYTHON) tools/recomp.py $(ROM_DRMARIO) -o $(GEN)/drmario --prefix drmario --listing

regen-sml: $(ROM_SML)
	$(PYTHON) tools/recomp.py $(ROM_SML) -o $(GEN)/sml --prefix sml --listing

regen-sml2: $(ROM_SML2)
	$(PYTHON) tools/recomp.py $(ROM_SML2) -o $(GEN)/sml2 --prefix sml2

# ---------- Windows cross-compile ----------------------------------------
# Requires llvm-mingw on PATH and SDL2-2.30.0 mingw dev package at $(SDL_WIN)
SDL_WIN ?= /opt/SDL2-mingw
WIN_CC  := x86_64-w64-mingw32-clang
WIN_INC := -I$(SDL_WIN)/x86_64-w64-mingw32/include/SDL2
WIN_LIB := -L$(SDL_WIN)/x86_64-w64-mingw32/lib -lSDL2 -lSDL2main
WIN_SYS := -lshell32 -lole32 -lcomdlg32 -luuid -lwinmm -lhid -lsetupapi

WIN_BANK_OBJS  := $(patsubst %,$(BUILD)/win_bank_%.o, 0 1 2 3)
WIN_SML2_OBJS  := $(patsubst %,$(BUILD)/win_sml2_%.o, \
                    0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 \
                    16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31)

$(BUILD)/win_bank_%.o: $(GEN)/sml/game_bank%.c | $(BUILD)
	$(WIN_CC) $(BANK_CFLAGS) -DUSE_SDL $(WIN_INC) $(INC) -c $< -o $@

$(BUILD)/win_sml2_%.o: $(GEN)/sml2/game_bank%.c | $(BUILD)
	$(WIN_CC) $(BANK_CFLAGS) -DUSE_SDL $(WIN_INC) $(INC) -c $< -o $@

win: $(NON_BANK_GEN) $(WIN_BANK_OBJS) $(WIN_SML2_OBJS) | $(BUILD)
	$(WIN_CC) -O2 -Wall -Wno-format-truncation -DUSE_SDL \
	    $(WIN_INC) $(INC) \
	    $(RT_SRC) $(NON_BANK_GEN) $(WIN_BANK_OBJS) $(WIN_SML2_OBJS) \
	    -o $(BUILD)/DrMarioLauncher.exe \
	    -mwindows $(WIN_LIB) $(WIN_SYS) -lm

run: $(BUILD)/gb
	$(BUILD)/gb $(if $(GAME),--$(GAME))

clean:
	rm -rf $(BUILD)

clean-gen:
	rm -rf $(GEN)/sml $(GEN)/sml2

.PHONY: all regen regen-drmario regen-sml regen-sml2 win run clean clean-gen
