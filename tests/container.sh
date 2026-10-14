#!/usr/bin/env bash
#
# tests/container.sh — testa o caminho do usuário numa distro limpa.
#
# A máquina de desenvolvimento não serve para isto: tem tudo instalado, e
# qualquer coisa que falte nela não aparece. Aqui o teste roda numa distro
# de verdade, sem nada do nosso lado dentro dela.
#
# Como: baixa o rootfs oficial da distro e entra nele com namespace de
# usuário + chroot (unshare -Ur). Sem sudo, sem daemon de container, sem
# mexer na máquina — é o mesmo mecanismo de um podman, sem o podman.
#
# O teste tem duas metades:
#   1. rodar o bootstrap numa distro SEM NADA e conferir que ele diz o que
#      falta e o comando exato para instalar (é o que o novato vê);
#   2. instalar o que ele pediu e rodar o caminho inteiro até o jogo rodar.
#
# Uso:
#   ./tests/container.sh                          # Ubuntu 24.04, ROM sintética
#   DISTRO=debian DISTRO_VER=12 ./tests/container.sh
#   ROM=/caminho/da/sua.gbc ./tests/container.sh  # compila uma ROM real
#
# Nada é escrito fora de $TMPDIR; o container é destruído ao final.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISTRO="${DISTRO:-ubuntu}"
DISTRO_VER="${DISTRO_VER:-24.04}"
ROM="${ROM:-}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/gbrecomp-distro.XXXXXX")"
ROOTFS="$WORK/rootfs"
REPO_IN="$WORK/repo"

PASS=0; FAIL=0
ok()    { printf '  \033[32mok\033[0m    %s\n' "$1"; PASS=$((PASS+1)); }
bad()   { printf '  \033[31mFALHA\033[0m  %s\n' "$1"; FAIL=$((FAIL+1)); }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }
cleanup() { [ "${KEEP_WORK:-0}" = "1" ] || rm -rf "$WORK"; }
trap cleanup EXIT

# ---------------------------------------------------------------- rootfs
case "$DISTRO" in
  ubuntu) BASE_URL="https://cdimage.ubuntu.com/ubuntu-base/releases/${DISTRO_VER}/release" ;;
  debian) BASE_URL="https://github.com/debuerreotype/docker-debian-artifacts/raw/dist-amd64-${DISTRO_VER%%.*}" ;;
  *) echo "DISTRO desconhecida: $DISTRO (use ubuntu ou debian)" >&2; exit 2 ;;
esac

head_ "1. Baixando a distro (${DISTRO} ${DISTRO_VER})"
if [ "$DISTRO" = "ubuntu" ]; then
  FILE="$(curl -s --max-time 30 "$BASE_URL/" \
          | grep -oE "ubuntu-base-${DISTRO_VER}[^\"<> ]*base-amd64\.tar\.gz" \
          | sort -V | tail -1)"
  [ -n "$FILE" ] || { echo "não achei o rootfs em $BASE_URL" >&2; exit 1; }
else
  FILE="docker-debian-artifacts-${DISTRO_VER}-rootfs.tar.xz"
fi
curl -fsSL --max-time 300 "$BASE_URL/$FILE" -o "$WORK/rootfs.tar.gz" \
  || { echo "falha ao baixar o rootfs" >&2; exit 1; }
ok "$(basename "$FILE") ($(du -h "$WORK/rootfs.tar.gz" | cut -f1))"

mkdir -p "$ROOTFS"
tar -xf "$WORK/rootfs.tar.gz" -C "$ROOTFS" 2>/dev/null \
  || tar -xf "$WORK/rootfs.tar.gz" -C "$ROOTFS" --warning=no-unknown-keyword 2>/dev/null \
  || { echo "não consegui descompactar o rootfs" >&2; exit 1; }
ok "rootfs extraído"

# ------------------------------------------------------ o repo, de leitura
cp -r "$REPO" "$REPO_IN"
rm -rf "$REPO_IN/.tools" "$REPO_IN/build" "$REPO_IN/packaging/dist" "$REPO_IN/.git"
chmod -R a+rwX "$REPO_IN"

# ROM: a real (copiada, só entra no teste) ou a sintética gerada aqui.
if [ -n "$ROM" ]; then
  [ -f "$ROM" ] || { echo "ROM não encontrada: $ROM" >&2; exit 2; }
  cp "$ROM" "$WORK/rom.gbc"
  ok "ROM real fornecida"
else
  python3 "$REPO/tests/make_test_rom.py" "$WORK/rom.gbc" >/dev/null
  ok "ROM sintética usada (nada de terceiros entra no teste)"
fi

# ------------------------------------------------- entrar na distro limpa
# unshare -Ur: root falso, próprio namespace de mount. Nada toca a máquina.
#
# O bind do /tmp da MÁQUINA inteiro seria mais simples, mas daria à distro
# limpa visão de tudo que existe em /tmp. Aqui entra só $WORK, que é o
# diretório que este teste controla — mais fiel a um container de verdade.
IN_DISTRO() { # IN_DISTRO <comando>; imprime a saída e devolve o status real
  # O rootfs oficial não tem /work: sem criá-lo, o mount falha em silêncio
  # e o erro aparece lá dentro como "no such file or directory", que
  # parece bug do bootstrap e não é.
  mkdir -p "$ROOTFS/work" "$ROOTFS/proc" "$ROOTFS/dev" "$ROOTFS/sys"
  # -p: sem um namespace de PID próprio o kernel recusa criar /proc dentro do
  # namespace de usuário, e o apt depende de /proc.
  # /dev e /sys precisam de --rbind: o --bind simples falha (EPERM) porque são
  # montagens com submontagens travadas.
  unshare -Urmpf bash -c '
    set -u
    ROOTFS="$1"; WORK="$2"; shift 2
    mount -t proc proc "$ROOTFS/proc" 2>/dev/null || echo "AVISO: sem /proc" >&2
    mount --rbind /dev "$ROOTFS/dev"  2>/dev/null || echo "AVISO: sem /dev" >&2
    mount --rbind /sys "$ROOTFS/sys"  2>/dev/null || echo "AVISO: sem /sys" >&2
    mount --bind "$WORK" "$ROOTFS/work" || { echo "ERRO: nao montei /work" >&2; exit 99; }
    cp /etc/resolv.conf "$ROOTFS/etc/resolv.conf" 2>/dev/null
    cd "$ROOTFS"
    exec chroot "$ROOTFS" /bin/bash -lc "$*"
  ' _ "$ROOTFS" "$WORK" "$1"
}

printf '  entrando em %s %s\n' "$DISTRO" "$DISTRO_VER"
IN_DISTRO 'uname -sr; id -u' | head -2 | sed 's/^/       /'
ok "dentro da distro"

# ------------------- 2. o que o bootstrap diz numa distro sem nada instalado
head_ "2. Bootstrap numa distro sem nada instalado"
OUT="$(IN_DISTRO 'cd /work/repo && ./bootstrap.sh' 2>&1)"
RC=$?

# O status real vale mais que o texto: um bootstrap que morre com erro de
# shell não escreve "FALTOU", e um grep amplo daria falso positivo.
if [ "$RC" -ne 0 ]; then
  ok "o bootstrap recusou seguir sem as dependências (saiu com status $RC)"
else
  bad "o bootstrap terminou com status 0 numa distro sem nada instalado"
fi

HINT="$(printf '%s' "$OUT" | grep -oE '(sudo )?(apt-get|dnf|pacman|zypper) [^"\\]*' | head -1)"
if [ -n "$HINT" ]; then
  ok "deu o comando exato de instalação:"
  printf '         %s\n' "$HINT"
else
  bad "não deu comando de instalação (mensagem inútil para o novato)"
  printf '%s\n' "$OUT" | head -12 | sed 's/^/         /'
fi

case "$HINT" in
  *apt-get*) MGR="apt-get" ;;
  *dnf*)     MGR="dnf" ;;
  *pacman*)  MGR="pacman" ;;
  *zypper*)  MGR="zypper" ;;
  *) MGR="" ;;
esac

if [ -n "$MGR" ]; then
  ok "gerenciador reconhecido: $MGR"
else
  bad "não reconheci o gerenciador a partir da dica"
fi

# --------------------------------- 3. instalar o que ele pediu e seguir em frente
head_ "3. Executando o comando que ele mandou (aqui já é root, então sem 'sudo')"
# Usa o comando LITERAL que o bootstrap mandou, só tirando o "sudo": se a
# instrução estiver errada ou incompleta, este teste descobre — foi assim que
# apareceu o `git`, que uma lista montada à mão teria esquecido.
INSTALL_CMD="$(printf '%s' "$HINT" | sed 's/sudo //g')"
if [ -n "$INSTALL_CMD" ]; then
  printf '         %s\n' "$INSTALL_CMD"
  # Workaround do sandbox, nao do projeto: dentro de um namespace de usuario
  # o apt nao pode trocar para o usuario _apt (setgroups e' EPERM). Vai num
  # arquivo de configuracao para valer em TODO comando apt desta execucao --
  # passar -o na linha so valeria para o ultimo comando do "&&".
  IN_DISTRO 'mkdir -p /etc/apt/apt.conf.d && printf "APT::Sandbox::User \"root\";\n" > /etc/apt/apt.conf.d/00sandbox'
  IN_DISTRO "export DEBIAN_FRONTEND=noninteractive; $INSTALL_CMD" >"$WORK/install.log" 2>&1
  rc_install=$?
  printf '         (executado o comando que o bootstrap mandou, palavra por palavra)\n'
  # O status do apt NÃO é a assertiva. Dentro de um namespace de usuario o
  # postinst do dpkg sempre falha no chown (/var/log/apt/term.log), então
  # o apt sai com 100 mesmo tendo instalado tudo. O que prova a instalação
  # é a ferramente presente — e quem consome isso é o bootstrap, logo é ele
  # que decide, no passo seguinte.
  if IN_DISTRO 'command -v cc >/dev/null && command -v git >/dev/null && pkg-config --exists sdl2'; then
    ok "compilador, git e SDL2 presentes"
    [ "$rc_install" -ne 0 ] && printf '         (o apt saiu com %d: postinst do dpkg faz chown, proibido aqui — não é falha do projeto)\n' "$rc_install"
  else
    bad "instalação não ficou completa (cc, git ou SDL2 faltando):"
    tail -6 "$WORK/install.log" | sed 's/^/         /'
  fi
fi

OUT2="$(IN_DISTRO 'cd /work/repo && ./bootstrap.sh' 2>&1)"
RC2=$?
if [ "$RC2" -ne 0 ]; then
  bad "o bootstrap falhou mesmo com as dependências instaladas (status $RC2):"
  printf '%s\n' "$OUT2" | tail -10 | sed 's/^/         /'
else
  ok "bootstrap completou (status 0):"
  printf '%s' "$OUT2" | grep -E 'ok ' | tail -5 | sed 's/^/         /'
fi

IN_DISTRO 'test -x /work/repo/.tools/gbrecomp' \
  && ok "gbrecomp disponível" || bad "gbrecomp ausente"

# ------------------------------------------------------ 4. compilar de fato
head_ "4. Compilando a ROM"
OUT3="$(IN_DISTRO 'cd /work/repo && ./build.sh /work/rom.gbc' 2>&1)"
RC3=$?
printf '%s' "$OUT3" | grep -E 'ok |FALTOU|aviso' | sed 's/^/         /'

if [ -z "$ROM" ]; then
  # ROM sintetica: este e' o caso que NAO deve passar. Ela nunca chega a
  # produzir frame, entao o smoke test tem que falhar -- e falhar rapido.
  # Se build.sh ficar pendurado aqui, este teste estoura no timeout e
  # denuncia o bug sozinho.
  if [ "$RC3" -eq 0 ]; then
    bad "a ROM sintetica passou no smoke test (nao deveria: ela nao produz frame)"
  else
    ok "build parou no smoke test, como esperado para a ROM sintetica"
  fi
  if printf '%s' "$OUT3" | grep -q 'SMOKE_TIMEOUT=1'; then
    ok "ofereceu o caminho para pular a verificacao"
  else
    bad "falhou sem explicar como seguir"
    printf '%s\n' "$OUT3" | tail -8 | sed 's/^/         /'
  fi
else
  if [ "$RC3" -eq 0 ]; then
    ok "build completo (validacao, geracao, compilacao e teste de execucao)"
  else
    bad "build falhou (status $RC3):"
    printf '%s\n' "$OUT3" | tail -12 | sed 's/^/         /'
  fi
fi

# ------------------------------------------------------ 5. o jogo executa?
head_ "5. O binário roda dentro da distro?"
if IN_DISTRO 'test -x /work/repo/build/ghost-babel'; then
  ok "binário gerado"
  IN_DISTRO 'cd /work/repo && ./build/ghost-babel --headless --limit 3000000' >/dev/null 2>&1
  check=$?
  [ "$check" -eq 0 ] && ok "o jogo executa o guest sem travar" \
                     || bad "o jogo travou ou quebrou (exit $check)"
else
  bad "binário não foi gerado"
fi

printf '\n'
if [ "$FAIL" -eq 0 ]; then
  printf '\033[32m%d verificações passaram em %s %s.\033[0m\n' "$PASS" "$DISTRO" "$DISTRO_VER"
  exit 0
fi
printf '\033[31m%d falharam\033[0m (de %d) em %s %s\n' "$FAIL" "$((PASS+FAIL))" "$DISTRO" "$DISTRO_VER"
printf 'Preservar logs: KEEP_WORK=1 ./tests/container.sh\n'
exit 1