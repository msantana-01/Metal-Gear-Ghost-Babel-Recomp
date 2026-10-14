# Makefile - the user's build.
#
# You should not need to touch this file: use ./build.sh. It exists so the
# build is predictable, and so there is an honest recipe for how the game is
# assembled (283 generated files + one library).

# Inputs (build.sh passes these; the defaults are for manual use)
ROM_SRC ?= build/project
OUT     ?= build/ghost-babel

# Tools prepared by bootstrap.sh
TOOLS_DIR := $(CURDIR)/.tools
ENGINE_DIR := $(TOOLS_DIR)/engine
LIB       := $(TOOLS_DIR)/libgbrt.a

# Runtime headers: gbrecomp embeds a complete copy of the runtime inside the
# generated project (3.4 MB), so whoever has the release tarball needs neither
# the network nor the engine clone to build. Whoever cloned the repo uses the
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

# No warnings by default. The generated code is 283 files and always produces
# warnings that mean nothing to someone using the tool - they flood the
# interface log and hide what matters. To debug:
#     make WARN="-Wall -Wextra" ...
WARN     ?=

# Silent by default: 283 lines of compiler command become a wall in the log.
# For debugging, build with V=1 and the lines come back.
V ?= 0
ifeq ($(V),1)
  Q :=
else
  Q := @
endif

GEN_SRC := $(wildcard $(ROM_SRC)/*.c)
GEN_OBJ := $(patsubst $(ROM_SRC)/%.c,$(WORK)/%.o,$(GEN_SRC))
GEN_TOTAL := $(words $(GEN_SRC))

# Parallelism: the bottleneck is CPU and there are ~283 files. Upstream also
# caps this at 8 concurrent compiles, because each cc holds a lot of memory
# over the generated code. Override with JOBS=16 if your machine takes it.
JOBS ?= $(shell n=$(nproc 2>/dev/null || echo 4); [ "$n" -gt 8 ] && echo 8 || echo $$n)

.PHONY: all clean distclean check-tools
all: check-tools $(OUT)

check-tools:
	@test -f "$(LIB)" || { \
	  echo "ERROR: $(LIB) does not exist. Run ./bootstrap.sh first."; exit 1; }
	@test -n "$(SDL_CFLAGS)" || { \
	  echo "ERROR: SDL2 headers not found. Install libsdl2-dev (or SDL2-devel)."; exit 1; }

$(WORK):
	@mkdir -p $(WORK)

# Parallel compile: the bottleneck is CPU, and there are ~283 files.
$(WORK)/%.o: $(ROM_SRC)/%.c | $(WORK)
	$(Q)$(CC) $(CFLAGS) $(WARN) $(DEFINES) $(SDL_CFLAGS) \
	  -I$(ROM_SRC) -I$(GBRT)/include -I$(GBRT)/vendor/imgui -c $< -o $@
	$(Q)echo "GBPROGRESS $$(ls $(WORK)/*.o 2>/dev/null | wc -l) $(GEN_TOTAL)"

# Linking uses the C++ driver because the library has C++ parts (SDL/ImGui)
# and depends on libstdc++.
$(OUT): $(GEN_OBJ) $(LIB)
	@echo "GBPHASE link"
	$(Q)$(CXX) $(GEN_OBJ) $(LIB) -o $@ $(SDL_LIBS) -lm -ldl -lpthread $(LDFLAGS)
	@strip $@ 2>/dev/null || true
	@echo "built: $@ ($$(du -h $@ | cut -f1))"

# Frees the ~600 MB of generated C and object files while KEEPING the game.
# The executable does not read any of it: it links only against system
# libraries. Regenerating costs one build (a few minutes).
clean:
	rm -rf build/work build/project
	@echo "removed the generated code; build/ghost-babel and your saves are still there"

# Removes everything build/ holds, including the game and its save files.
distclean:
	rm -rf build
	@echo "removed build/ entirely, including the game and its saves"