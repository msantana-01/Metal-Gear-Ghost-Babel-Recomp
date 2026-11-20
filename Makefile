# Makefile - the user's build.
#
# You should not need to touch this file: use ./build.sh. It exists so the
# build is predictable, and so there is an honest recipe for how the game is
# assembled (generated C files + runtime library, plus remaster presenter).

# Inputs (build.sh passes these; the defaults are for manual use)
ROM_SRC      ?= build/project
REMASTER_DIR ?= $(CURDIR)/remaster
BUILD_REMASTER ?= 0
OUT          ?= build/ghost-babel
OUT_AUTH     ?= build/ghost-babel-authentic
OUT_REMASTER ?= build/ghost-babel-remaster

# Tools prepared by bootstrap.sh
TOOLS_DIR  := $(CURDIR)/.tools
ENGINE_DIR := $(TOOLS_DIR)/engine
LIB        := $(TOOLS_DIR)/libgbrt.a

# Runtime headers: gbrecomp embeds a complete copy of the runtime inside the
# generated project, so whoever has the release tarball needs neither the
# network nor the engine clone to build. Whoever cloned the repo uses the
# checkout, if it is there.
GBRT ?= $(if $(wildcard $(ROM_SRC)/runtime/include/gbrt.h),\
             $(ROM_SRC)/runtime,\
             $(ENGINE_DIR)/runtime)

CC      ?= cc
CXX     ?= c++
# -DGB_HAS_SDL2 is MANDATORY. Without it the generated C does not even include
# the platform header: the binary links, but is born without video, input or
# save states. Upstream's CMake passes this define; here it is explicit.
DEFINES := -DGB_HAS_SDL2

# SDL2: resolved through pkg-config, with sdl2-config as a fallback.
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs 2>/dev/null)

# Compilation directory: next to the generated .c files, so that includes like
# "baserom.h" resolve without an extra path.
WORK := build/work

CFLAGS   ?= -std=gnu11 -O2 -ffunction-sections -fdata-sections
LDFLAGS  ?= -Wl,--gc-sections

# No warnings by default. The generated code has hundreds of files and always
# produces warnings that mean nothing to someone using the tool - they flood
# the interface log and hide what matters. To debug:
#     make WARN="-Wall -Wextra" ...
WARN     ?=

# Silent by default: hundreds of lines of compiler commands become a wall in the
# log. For debugging, build with V=1 and the lines come back.
V ?= 0
ifeq ($(V),1)
  Q :=
else
  Q := @
endif

GEN_SRC        := $(wildcard $(ROM_SRC)/*.c)
GEN_MAIN_SRC   := $(filter %_main.c,$(GEN_SRC))
GEN_COMMON_SRC := $(filter-out %_main.c %_seeded.c,$(GEN_SRC))
GEN_COMMON_OBJ := $(patsubst $(ROM_SRC)/%.c,$(WORK)/%.o,$(GEN_COMMON_SRC))
GEN_MAIN_OBJ   := $(patsubst $(ROM_SRC)/%.c,$(WORK)/%.o,$(GEN_MAIN_SRC))

REMASTER_SRC := $(wildcard $(REMASTER_DIR)/*.c)
REMASTER_HDR := $(wildcard $(REMASTER_DIR)/*.h)
REMASTER_OBJ := $(patsubst $(REMASTER_DIR)/%.c,$(WORK)/remaster_%.o,$(REMASTER_SRC))

ifeq ($(BUILD_REMASTER),1)
  GEN_TOTAL := $(words $(GEN_COMMON_SRC) $(GEN_MAIN_SRC) $(REMASTER_SRC))
else
  GEN_TOTAL := $(words $(GEN_COMMON_SRC) $(GEN_MAIN_SRC))
endif

# Parallelism: the bottleneck is CPU and there are hundreds of files. Upstream
# caps this at 8 concurrent compiles, because each cc holds a lot of memory
# over the generated code. Override with JOBS=16 if your machine takes it.
JOBS ?= $(shell n=$$(nproc 2>/dev/null || echo 4); [ "$$n" -gt 8 ] && echo 8 || echo "$$n")

.PHONY: all clean distclean check-tools check-remaster
ifeq ($(BUILD_REMASTER),1)
all: check-tools check-remaster $(OUT_AUTH) $(OUT_REMASTER)
else
all: check-tools $(OUT)
endif

check-tools:
	@test -f "$(LIB)" || { \
	  echo "ERROR: $(LIB) does not exist. Run ./bootstrap.sh first."; exit 1; }
	@test -n "$(SDL_CFLAGS)" || { \
	  echo "ERROR: SDL2 headers not found. Install libsdl2-dev (or SDL2-devel)."; exit 1; }

check-remaster:
	@test -n "$(REMASTER_SRC)" || { \
	  echo "ERROR: remaster/ sources not found in $(REMASTER_DIR)."; exit 1; }

$(WORK):
	@mkdir -p $(WORK)

# Parallel compile: generated ROM units are shared between both executables.
$(WORK)/%.o: $(ROM_SRC)/%.c | $(WORK)
	$(Q)$(CC) $(CFLAGS) $(WARN) $(DEFINES) $(SDL_CFLAGS) \
	  -I$(ROM_SRC) -I$(GBRT)/include -I$(GBRT)/vendor/imgui -c $< -o $@
	$(Q)echo "GBPROGRESS $$(ls $(WORK)/*.o 2>/dev/null | wc -l) $(GEN_TOTAL)"

# Widescreen remaster presenter (C99 read-only consumer of presentation ABI).
$(REMASTER_OBJ): $(REMASTER_HDR)

$(WORK)/remaster_%.o: $(REMASTER_DIR)/%.c | $(WORK)
	$(Q)$(CC) $(CFLAGS) $(WARN) $(DEFINES) $(SDL_CFLAGS) \
	  -I$(ROM_SRC) -I$(REMASTER_DIR) -I$(GBRT)/include -c $< -o $@
	$(Q)echo "GBPROGRESS $$(ls $(WORK)/*.o 2>/dev/null | wc -l) $(GEN_TOTAL)"

# Linking uses the C++ driver because the library has C++ parts (SDL/ImGui)
# and depends on libstdc++.
$(OUT): $(GEN_COMMON_OBJ) $(GEN_MAIN_OBJ) $(LIB)
	@echo "GBPHASE link"
	$(Q)$(CXX) $^ -o $@ $(SDL_LIBS) -lm -ldl -lpthread $(LDFLAGS)
	@strip $@ 2>/dev/null || true
	@echo "built: $@ ($$(du -h $@ | cut -f1))"

$(OUT_AUTH): $(GEN_COMMON_OBJ) $(GEN_MAIN_OBJ) $(LIB)
	@echo "GBPHASE link"
	$(Q)$(CXX) $^ -o $@ $(SDL_LIBS) -lm -ldl -lpthread $(LDFLAGS)
	@strip $@ 2>/dev/null || true
	@echo "built: $@ ($$(du -h $@ | cut -f1))"

$(OUT_REMASTER): $(GEN_COMMON_OBJ) $(REMASTER_OBJ) $(LIB)
	@echo "GBPHASE link"
	$(Q)$(CXX) $^ -o $@ $(SDL_LIBS) -lm -ldl -lpthread $(LDFLAGS)
	@strip $@ 2>/dev/null || true
	@echo "built: $@ ($$(du -h $@ | cut -f1))"

# Frees the ~600 MB of generated C and object files while KEEPING the game.
# The executables do not read any of it: they link only against system
# libraries. Regenerating costs one build (a few minutes).
clean:
	rm -rf build/work build/project
	@echo "removed generated code; build executables (ghost-babel-remaster, ghost-babel-authentic) and your saves are still there"

# Removes everything build/ holds, including the executables and save files.
distclean:
	rm -rf build
	@echo "removed build/ entirely, including executables and saves"
