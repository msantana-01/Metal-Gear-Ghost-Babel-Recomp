# Makefile — build do usuário final.
#
# Você não deveria precisar mexer neste arquivo: use ./build.sh.
# Ele existe para que o build seja previsível e para dar uma receita
# honesta de como o jogo é montado (283 arquivos gerados + 1 biblioteca).

# Entradas (build.sh passa isso; os defaults servem para uso manual)
ROM_SRC ?= build/project
OUT     ?= build/ghost-babel

# Ferramentas preparadas pelo bootstrap.sh
TOOLS_DIR := $(CURDIR)/.tools
ENGINE_DIR := $(TOOLS_DIR)/engine
LIB       := $(TOOLS_DIR)/libgbrt.a

# Headers do runtime: o gbrecomp embute uma cópia completa do runtime dentro
# do projeto gerado (3,4 MB), então quem tem o tarball do release não precisa
# de rede nem do clone do engine para compilar. Quem clonou o repo usa o
# checkout, se existir.
GBRT ?= $(if $(wildcard $(ROM_SRC)/runtime/include/gbrt.h),\
             $(ROM_SRC)/runtime,\
             $(ENGINE_DIR)/runtime)

# O diretório de trabalho da compilação: ao lado dos .c gerados, para que
# os #include "baserom.h" etc. resolvam sem caminho extra.
WORK := build/work

CC      ?= cc
CXX     ?= c++
# -DGB_HAS_SDL2 é OBRIGATÓRIO. Sem ele o C gerado nem inclui o cabeçalho
# da plataforma: o binário linka, mas nasce sem vídeo, sem input e sem
# salvar progresso. O CMake do upstream passa essa define; aqui é explícita.
DEFINES := -DGB_HAS_SDL2

# SDL2: resolvido pelo pkg-config, com sdl2-config como reserva.
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null || sdl2-config --cflags 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null || sdl2-config --libs 2>/dev/null)

CFLAGS   ?= -std=gnu11 -O2 -ffunction-sections -fdata-sections
LDFLAGS  ?= -Wl,--gc-sections

# Sem warnings por padrão. O código gerado tem 283 arquivos e sempre produz
# avisos que não significam nada para quem está usando a ferramenta — eles
# entulham o log da interface e escondem o que importa. Para depurar:
#     make WARN="-Wall -Wextra" ...
WARN     ?=

GEN_SRC := $(wildcard $(ROM_SRC)/*.c)
GEN_OBJ := $(patsubst $(ROM_SRC)/%.c,$(WORK)/%.o,$(GEN_SRC))
GEN_TOTAL := $(words $(GEN_SRC))

# Silencioso por padrão: 283 linhas de comando de compilador viram muro no
# log da interface. Para depurar, rode com V=1 e as linhas voltam.
V ?= 0
ifeq ($(V),1)
  Q :=
else
  Q := @
endif

# Paralelismo: o gargalo é CPU e são ~283 arquivos. O upstream também limita
# a 8 compilações simultâneas, porque cada cc segura bastante memória sobre o
# C gerado. Sobrescreva com JOBS=16 se sua máquina aguentar.
JOBS ?= $(shell n=$(nproc 2>/dev/null || echo 4); [ "$n" -gt 8 ] && echo 8 || echo $$n)

.PHONY: all clean check-tools
all: check-tools $(OUT)

check-tools:
	@test -f "$(LIB)" || { \
	  echo "ERRO: $(LIB) nao existe. Rode ./bootstrap.sh primeiro."; exit 1; }
	@test -n "$(SDL_CFLAGS)" || { \
	  echo "ERRO: headers do SDL2 nao encontrados. Instale libsdl2-dev (ou SDL2-devel)."; exit 1; }

$(WORK):
	@mkdir -p $(WORK)

# Compilação paralela: o gargalo é CPU, e são ~283 arquivos.
$(WORK)/%.o: $(ROM_SRC)/%.c | $(WORK)
	$(Q)$(CC) $(CFLAGS) $(WARN) $(DEFINES) $(SDL_CFLAGS) \
	  -I$(ROM_SRC) -I$(GBRT)/include -I$(GBRT)/vendor/imgui -c $< -o $@
	$(Q)echo "GBPROGRESS $$(ls $(WORK)/*.o 2>/dev/null | wc -l) $(GEN_TOTAL)"

# O link usa o driver C++ porque a biblioteca tem partes C++ (SDL/ImGui)
# e depende de libstdc++.
$(OUT): $(GEN_OBJ) $(LIB)
	@echo "GBPHASE link"
	$(Q)$(CXX) $(GEN_OBJ) $(LIB) -o $@ $(SDL_LIBS) -lm -ldl -lpthread $(LDFLAGS)
	@strip $@ 2>/dev/null || true
	@echo "gerado: $@ ($$(du -h $@ | cut -f1))"

clean:
	rm -rf $(WORK) $(OUT)