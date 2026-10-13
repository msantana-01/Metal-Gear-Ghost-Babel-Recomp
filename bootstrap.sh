#!/usr/bin/env bash
#
# bootstrap.sh — prepara tudo que o build precisa. Rode UMA vez.
#
#   ./bootstrap.sh
#
# O script é escritos para nunca encerrar com uma mensagem que não diga
# o que fazer. Se algo faltar, ele diz o que falta e o comando exato
# que resolve, para a sua distribuição.

set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$REPO_DIR/.tools"
LOG_FILE="$REPO_DIR/.bootstrap.log"

# shellcheck disable=SC1091
[ -f "$REPO_DIR/pins.lock" ] || { echo "ERRO: pins.lock não encontrado. Repo clonado errado?"; exit 1; }

pin() { # pin <secao> <chave>
  sed -n "/^\[$1\]/,/^\[/s/^$2[[:space:]]*=[[:space:]]*//p" "$REPO_DIR/pins.lock" | head -1
}

ENGINE_URL="$(pin engine url)"
ENGINE_COMMIT="$(pin engine commit)"
ENGINE_PATCH="$(pin engine patch)"
ENGINE_PATCH_SHA="$(pin engine patch_sha256)"
ENGINE_TREE="$(pin engine tree_after)"
SDL2_MIN="$(pin build sdl2_min)"
TOOLS_VERSION="$(pin tools version)"
RELEASE_BASE="$(pin tools release_base)"

C_RESET=$'\033[0m'; C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_HDR=$'\033[1;36m'
[ -t 1 ] || { C_RESET=; C_OK=; C_WARN=; C_ERR=; C_HDR=; }

step()  { printf '\n%s==> %s%s\n' "$C_HDR" "$1" "$C_RESET"; }
ok()    { printf '    %sok%s   %s\n' "$C_OK" "$C_RESET" "$1"; }
info()  { printf '    -    %s\n' "$1"; }
warn()  { printf '    %saviso%s %s\n' "$C_WARN" "$C_RESET" "$1"; }
log()   { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$1" >>"$LOG_FILE" 2>/dev/null || true; }

die() { # die <o que faltou> <comando que resolve>
  printf '\n%sFALTOU: %s%s\n' "$C_ERR" "$1" "$C_RESET" >&2
  [ $# -ge 2 ] && printf '\n  Resolva assim:\n\n    %s\n\n' "$2" >&2
  printf '  Detalhes técnicos: %s\n' "$LOG_FILE" >&2
  exit 1
}

mkdir -p "$TOOLS_DIR"
: > "$LOG_FILE"

# ---------------------------------------------------------------- distro
DISTRO="$(. /etc/os-release 2>/dev/null && echo "${ID:-unknown}")"
DISTRO_LIKE="$(. /etc/os-release 2>/dev/null && echo "${ID_LIKE:-}")"

install_hint() { # install_hint <pacotes...>
  case "$DISTRO" in
    arch|manjaro|endeavours|cachyos|garuda)
      printf 'sudo pacman -S --needed base-devel pkgconf sdl2 %s\n' "${1:-}" ;;
    debian|ubuntu|linuxmint|pop|elementary|zorin)
      printf 'sudo apt-get install -y build-essential pkg-config libsdl2-dev %s\n' "${1:-}" ;;
    fedora)
      printf 'sudo dnf install -y gcc-c++ make pkgconf-pkg-config SDL2-devel %s\n' "${1:-}" ;;
    opensuse*|sles)
      printf 'sudo zypper install -y gcc-c++ make pkg-config libSDL2-devel %s\n' "${1:-}" ;;
    *)
      printf '# instale: compilador C/C++, make, pkg-config e os headers de desenvolvimento do SDL2\n' ;;
  esac
}

have() { command -v "$1" >/dev/null 2>&1; }

# --------------------------------------------------- 0.5 ferramentas já prontas
#
# Quem baixou o release recebe as ferramentas compiladas (bin/ e lib/) e não
# precisa de cmake, ninja nem rede. Ajustamos o formato para o mesmo que o
# build usa e seguimos como se o bootstrap tivesse compilado tudo.
if [ -f "$REPO_DIR/bin/gbrecomp" ] && [ -f "$REPO_DIR/lib/libgbrt.a" ] \
   && [ ! -x "$TOOLS_DIR/gbrecomp" ]; then
  mkdir -p "$TOOLS_DIR"
  cp "$REPO_DIR/bin/gbrecomp" "$TOOLS_DIR/gbrecomp"
  cp "$REPO_DIR/lib/libgbrt.a" "$TOOLS_DIR/libgbrt.a"
  chmod +x "$TOOLS_DIR/gbrecomp"
  info "ferramentas do release detectadas (não precisam ser compiladas)"
  RELEASE_MODE=1
fi

# ------------------------------------------------------- 0. checagem base
step "Checando o essencial"

if [ "$(uname -s)" != "Linux" ]; then
  warn "Este projeto ainda só funciona no Linux (você está em $(uname -s))."
  warn "Windows e Android estão planejados."
fi

if ! have git; then die "git não encontrado." "$(install_hint git)"; fi
ok "git $(git --version | awk '{print $3}')"

CC_BIN=""
for c in cc gcc clang; do if have "$c"; then CC_BIN="$c"; break; fi; done
[ -n "$CC_BIN" ] || die "nenhum compilador C encontrado." "$(install_hint)"
CXX_BIN=""
for c in c++ g++ clang++; do if have "$c"; then CXX_BIN="$c"; break; fi; done
[ -n "$CXX_BIN" ] || die "nenhum compilador C++ encontrado." "$(install_hint)"
ok "compilador: $CC_BIN / $CXX_BIN"

if ! have make; then
  warn "make não encontrado — só é necessário se você compilar a partir do fonte."
else
  ok "make $(make --version | head -1 | awk '{print $3}')"
fi

# ------------------------------------------------------- 1. SDL2 headers
step "Checando o SDL2"

SDL_CFLAGS=""; SDL_LIBS=""; SDL_VER=""
if have pkg-config && pkg-config --exists sdl2 2>/dev/null; then
  SDL_CFLAGS="$(pkg-config --cflags sdl2)"
  SDL_LIBS="$(pkg-config --libs sdl2)"
  SDL_VER="$(pkg-config --modversion sdl2)"
elif have sdl2-config; then
  SDL_CFLAGS="$(sdl2-config --cflags)"
  SDL_LIBS="$(sdl2-config --libs)"
  SDL_VER="$(sdl2-config --version | awk '{print $1}')"
fi

if [ -z "$SDL_VER" ]; then
  die "headers de desenvolvimento do SDL2 não encontrados.
É o único pacote extra: ele traz os arquivos .h usados na compilação." \
      "$(install_hint)"
fi

# compara versões sem depender de "sort -V" (nem sempre presente)
ver_ge() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V 2>/dev/null | head -1)" = "$2" ]; }
if ! ver_ge "$SDL_VER" "$SDL2_MIN"; then
  die "SDL2 $SDL_VER encontrado, mas este projeto precisa de $SDL2_MIN ou superior." \
      "$(install_hint)"
fi
ok "SDL2 $SDL_VER (mínimo $SDL2_MIN)"
printf '%s\n' "SDL_CFLAGS=$SDL_CFLAGS" >> "$TOOLS_DIR/env"

# ------------------------------------------------- 2. engine (patch pinado)
#
# Quem baixou o tarball do release não precisa do clone: o gbrecomp embute o
# runtime no projeto gerado, e o Makefile usa os headers de lá. O clone só é
# necessário para COMPILAR as ferramentas — e nesse caso já vieram prontas.
ENGINE_DIR="$TOOLS_DIR/engine"
if [ -n "$RELEASE_MODE" ]; then
  step "Ferramentas do release"
  ok "gbrecomp e runtime já vêm prontos: nada a compilar, nada a baixar"
  ok "os headers do runtime vêm junto com o C gerado"
  mkdir -p "$TOOLS_DIR"
  printf '%s\n' "GBRECOMP=$TOOLS_DIR/gbrecomp" >> "$TOOLS_DIR/env"
  printf '\n%s%sPronto!%s\n' "$C_OK" "" "$C_RESET"
  printf '\n  Próximo passo:\n\n      ./build.sh /caminho/para/sua/rom.gbc\n\n'
  exit 0
fi

step "Baixando o motor (versão fixada)"

if [ -d "$ENGINE_DIR/.git" ]; then
  ok "já está em .tools/engine"
else
  info "clonando $ENGINE_URL"
  log "clone $ENGINE_URL"
  rm -rf "$ENGINE_DIR"
  have git || die "git não encontrado."
  git clone -q --filter=blob:none "$ENGINE_URL" "$ENGINE_DIR" \
    || die "não consegui baixar o motor. Sem internet?" "git clone $ENGINE_URL $ENGINE_DIR"
  log "checkout $ENGINE_COMMIT"
  git -C "$ENGINE_DIR" checkout -q "$ENGINE_COMMIT" \
    || die "não achei a versão fixada do motor ($ENGINE_COMMIT)." \
       "Isso normalmente significa que o upstream removeu o commit. Reporte no GitHub."
  ok "commit $ENGINE_COMMIT"
fi

# integridade do patch
PATCH_FILE="$REPO_DIR/$ENGINE_PATCH"
[ -f "$PATCH_FILE" ] || die "patch do motor ausente: $ENGINE_PATCH" "git clone <este-repo> novamente"
if have sha256sum; then
  GOT="$(sha256sum "$PATCH_FILE" | awk '{print $1}')"
  [ "$GOT" = "$ENGINE_PATCH_SHA" ] \
    || die "o patch do motor não confere com o pin (sha256 diferente).
Isso normalmente significa arquivo corrompido na clonagem." "rm -rf .tools && git clone <este-repo> novamente"
fi

if [ -d "$ENGINE_DIR/.git/rebase-apply" ] || [ -d "$ENGINE_DIR/.git/rebase-merge" ]; then
  ok "patch já aplicado"
else
  info "aplicando o patch do motor"
  log "git am $ENGINE_PATCH"
  git -C "$ENGINE_DIR" am -q "$PATCH_FILE" \
    || die "o patch do motor não aplicou limpo.
Isso é um defeito nosso, não seu — por favor reporte no GitHub com a saída acima." \
       "cd .tools/engine && git am --abort"
  ok "patch aplicado"
fi

TREE_NOW="$(git -C "$ENGINE_DIR" rev-parse HEAD^{tree} 2>/dev/null)"
if [ -n "$ENGINE_TREE" ] && [ "$TREE_NOW" != "$ENGINE_TREE" ]; then
  die "o motor ficou diferente do esperado após o patch.
Esperava a árvore $ENGINE_TREE, obtive $TREE_NOW." \
      "cd .tools && rm -rf engine && ../bootstrap.sh"
fi
ok "integridade do motor conferida"

# --------------------------------------------- 3. ferramentas (gbrecomp)
step "Preparando as ferramentas"

GBRECOMP="$TOOLS_DIR/gbrecomp"
if [ -x "$GBRECOMP" ]; then
  ok "gbrecomp já pronto"
elif [ -n "$RELEASE_BASE" ]; then
  info "baixando ferramentas pré-compiladas (v$TOOLS_VERSION)"
  ARCH="$(uname -m)"
  case "$ARCH" in x86_64) PLAT=linux-x86_64 ;; aarch64|arm64) PLAT=linux-arm64 ;;
    *) PLAT="linux-$ARCH" ;; esac
  TARBALL="$RELEASE_BASE/ghost-babel-tools-$PLAT.tar.gz"
  if have curl; then curl -fsSL "$TARBALL" -o "$TOOLS_DIR/tools.tar.gz"
  elif have wget; then wget -q "$TARBALL" -O "$TOOLS_DIR/tools.tar.gz"
  else die "preciso de curl ou wget para baixar as ferramentas." "$(install_hint curl)"
  fi
  tar -xzf "$TOOLS_DIR/tools.tar.gz" -C "$TOOLS_DIR" \
    || die "não consegui descompactar as ferramentas baixadas." "rm -f .tools/tools.tar.gz && ./bootstrap.sh"
  ok "ferramentas prontas"
else
  warn "ainda não há release com ferramentas prontas — vou compilar a partir do fonte."
  warn "isso precisa de cmake e ninja (é uma etapa só, ~2 minutos)."
  have cmake || die "para compilar as ferramentas do zero é preciso cmake." "$(install_hint cmake ninja-build)"
  have ninja || warn "sem ninja: o cmake vai usar make (mais lento)."
  CMAKE_G="Ninja" ; have ninja || CMAKE_G="Unix Makefiles"
  log "cmake -S $ENGINE_DIR -B $ENGINE_DIR/build -G $CMAKE_G"
  cmake -G "$CMAKE_G" -S "$ENGINE_DIR" -B "$ENGINE_DIR/build" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF >/dev/null \
    || die "cmake falhou ao configurar o motor." "cmake -S .tools/engine -B .tools/engine/build"
  cmake --build "$ENGINE_DIR/build" --target gbrecomp -j "$(nproc 2>/dev/null || echo 4)" >/dev/null \
    || die "cmake falhou ao compilar o gbrecomp." "cmake --build .tools/engine/build --target gbrecomp"
  cp "$ENGINE_DIR/build/bin/gbrecomp" "$GBRECOMP" 2>/dev/null \
    || find "$ENGINE_DIR/build" -name gbrecomp -type f -exec cp {} "$GBRECOMP" \; \
    || die "compilei mas não achei o binário do gbrecomp." "ls .tools/engine/build/bin"
  ok "gbrecomp compilado do fonte"
fi
printf '%s\n' "GBRECOMP=$GBRECOMP" >> "$TOOLS_DIR/env"

# --------------------------------------------- 4. runtime pré-compilado
step "Preparando o runtime (a biblioteca que faz o jogo rodar)"

LIB="$TOOLS_DIR/libgbrt.a"
RT="$ENGINE_DIR/runtime"

if [ -f "$LIB" ] && [ "$LIB" -nt "$ENGINE_DIR/.git/HEAD" ] 2>/dev/null; then
  ok "libgbrt.a já pronta"
else
  log "build libgbrt.a"
  OBJ="$TOOLS_DIR/.obj"; rm -rf "$OBJ"; mkdir -p "$OBJ"
  # C runtime
  for f in "$RT"/src/*.c; do
    b="$(basename "${f%.c}")"
    [ "$b" = "gbrt_gameplay_mutation" ] && continue
    [ "$b" = "gbrt_native_patch" ] && continue
    $CC_BIN -std=gnu11 -O3 -DGB_HAS_SDL2 -DGB_PLATFORM_LINUX \
      -I"$RT/include" $SDL_CFLAGS -c "$f" -o "$OBJ/$b.o" \
      || die "falha ao compilar $b.c" "$CC_BIN --version"
  done
  # C++ runtime (SDL + Dear ImGui)
  CXXF="-std=c++20 -O3 -DGB_HAS_SDL2 -DGB_PLATFORM_LINUX -I$RT/include -I$RT/vendor/imgui -I$RT/vendor/imgui/backends $SDL_CFLAGS"
  for f in "$RT/src/platform_sdl.cpp" \
           "$RT"/vendor/imgui/imgui.cpp "$RT"/vendor/imgui/imgui_draw.cpp \
           "$RT"/vendor/imgui/imgui_tables.cpp "$RT"/vendor/imgui/imgui_widgets.cpp \
           "$RT"/vendor/imgui/backends/imgui_impl_sdl2.cpp \
           "$RT"/vendor/imgui/backends/imgui_impl_sdlrenderer2.cpp; do
    b="$(basename "${f%.*}")"
    $CXX_BIN $CXXF -c "$f" -o "$OBJ/$b.o" || die "falha ao compilar $b.cpp" "$CXX_BIN --version"
  done
  ar rcs "$LIB" "$OBJ"/*.o || die "falha ao montar a biblioteca estática." "ar --version"
  rm -rf "$OBJ"
  ok "libgbrt.a ($(du -h "$LIB" | cut -f1))"
fi

# --------------------------------------------------- 5. checagem real
step "Conferindo que dá para rodar"

if [ -x "$TOOLS_DIR/selftest" ]; then
  "$TOOLS_DIR/selftest" >/dev/null 2>&1 \
    && ok "runtime responde" \
    || die "o runtime foi compilado mas não responde." "$LOG_FILE"
else
  ok "runtime pronto (o teste de rodar vem no primeiro build)"
fi

printf '\n%s%s%s\n' "$C_OK" "Tudo pronto." "$C_RESET"
cat <<EOF

  Próximo passo:

      ./build.sh /caminho/para/sua/rom.gbc

  (ou rode ./run.sh --gui para escolher a ROM por uma janela)

  Log desta execução: $LOG_FILE

EOF