#!/usr/bin/env bash
#
# tests/container.sh - runs the user's path on a clean distribution.
#
# The development machine cannot stand in for this: it has everything
# installed, so anything missing there never shows up. Here the test runs on a
# real distribution, with nothing of ours inside it.
#
# How: downloads the official distribution rootfs and enters it with a user
# namespace plus chroot (unshare -Urmpf). No sudo, no container daemon, and
# the machine is never touched. It is the same mechanism as podman, without
# podman.
#
# The test has two halves:
#   1. run bootstrap on a distribution with nothing installed and check it
#      says what is missing and the exact command that installs it (this is
#      what a newcomer sees);
#   2. install what it asked for and run the whole path until the game runs.
#
# Usage:
#   ./tests/container.sh                          # Ubuntu 24.04, synthetic ROM
#   DISTRO=debian DISTRO_VER=12 ./tests/container.sh
#   ROM=/path/to/your.gbc ./tests/container.sh    # compile a real ROM
#
# Only the container is touched. The repository and any outside ROM are
# read-only.

set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DISTRO="${DISTRO:-ubuntu}"
DISTRO_VER="${DISTRO_VER:-24.04}"
ROM="${ROM:-}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/gbrecomp-distro.XXXXXX")"
ROOTFS="$WORK/rootfs"

PASS=0; FAIL=0
ok()    { printf '  \033[32mok\033[0m    %s\n' "$1"; PASS=$((PASS+1)); }
bad()   { printf '  \033[31mFAILED\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }
head_() { printf '\n\033[1m%s\033[0m\n' "$1"; }
cleanup() { [ "${KEEP_WORK:-0}" = "1" ] || rm -rf "$WORK"; }
trap cleanup EXIT

# ---------------------------------------------------------------- rootfs
case "$DISTRO" in
  ubuntu) BASE_URL="https://cdimage.ubuntu.com/ubuntu-base/releases/${DISTRO_VER}/release" ;;
  debian) BASE_URL="https://github.com/debuerreotype/docker-debian-artifacts/raw/dist-amd64-${DISTRO_VER%%.*}" ;;
  *) echo "unknown DISTRO: $DISTRO (use ubuntu or debian)" >&2; exit 2 ;;
esac

head_ "1. Downloading the distribution (${DISTRO} ${DISTRO_VER})"
if [ "$DISTRO" = "ubuntu" ]; then
  FILE="$(curl -s --max-time 30 "$BASE_URL/" \
          | grep -oE "ubuntu-base-${DISTRO_VER}[^\"<> ]*base-amd64\.tar\.gz" \
          | sort -V | tail -1)"
  [ -n "$FILE" ] || { echo "could not find the rootfs at $BASE_URL" >&2; exit 1; }
else
  FILE="docker-debian-artifacts-${DISTRO_VER}-rootfs.tar.xz"
fi
curl -fsSL --max-time 300 "$BASE_URL/$FILE" -o "$WORK/rootfs.tar.gz" \
  || { echo "failed to download the rootfs" >&2; exit 1; }
ok "$(basename "$FILE") ($(du -h "$WORK/rootfs.tar.gz" | cut -f1))"

mkdir -p "$ROOTFS"
tar -xf "$WORK/rootfs.tar.gz" -C "$ROOTFS" 2>/dev/null \
  || { echo "could not unpack the rootfs" >&2; exit 1; }
ok "rootfs unpacked"

# ------------------------------------------------------- the repo, read-only
cp -r "$REPO" "$WORK/repo"
rm -rf "$WORK/repo/.tools" "$WORK/repo/build" "$WORK/repo/packaging/dist" \
       "$WORK/repo/.git" "$WORK/repo/runtime"
chmod -R a+rwX "$WORK/repo"

# ROM: the real one (copied in, it only exists for this test) or the synthetic
# one generated here.
if [ -n "$ROM" ]; then
  [ -f "$ROM" ] || { echo "ROM not found: $ROM" >&2; exit 2; }
  cp "$ROM" "$WORK/rom.gbc"
  ok "real ROM provided"
else
  python3 "$REPO/tests/make_test_rom.py" "$WORK/rom.gbc" >/dev/null
  ok "synthetic ROM used (no third-party material enters the test)"
fi

# ------------------------------------------------- enter the clean distribution
# unshare -Ur: fake root, own mount namespace. Nothing touches the machine.
#
# Binding the machine's whole /tmp would be simpler but would let the clean
# distribution see everything in /tmp. Only $WORK goes in, which is the
# directory this test controls - closer to a real container.
IN_DISTRO() { # IN_DISTRO <command>; prints output and returns the real status
  # The official rootfs has no /work: without creating it, the mount fails
  # silently and the error shows up in there as "no such file or directory",
  # which looks like a bootstrap bug and is not.
  mkdir -p "$ROOTFS/work" "$ROOTFS/proc" "$ROOTFS/dev" "$ROOTFS/sys"
  # -p: without a PID namespace of its own, the kernel refuses to mount /proc
  # inside a user namespace, and apt depends on /proc.
  # /dev and /sys need --rbind: plain --bind fails (EPERM) because they are
  # mounts with locked submounts.
  unshare -Urmpf bash -c '
    set -u
    ROOTFS="$1"; WORK="$2"; shift 2
    mount -t proc proc "$ROOTFS/proc" 2>/dev/null || echo "WARNING: no /proc" >&2
    mount --rbind /dev "$ROOTFS/dev"  2>/dev/null || echo "WARNING: no /dev" >&2
    mount --rbind /sys "$ROOTFS/sys"  2>/dev/null || echo "WARNING: no /sys" >&2
    mount --bind "$WORK" "$ROOTFS/work" || { echo "ERROR: could not mount /work" >&2; exit 99; }
    cp /etc/resolv.conf "$ROOTFS/etc/resolv.conf" 2>/dev/null
    cd "$ROOTFS"
    exec chroot "$ROOTFS" /bin/bash -lc "$*"
  ' _ "$ROOTFS" "$WORK" "$1"
}

printf '  entering %s %s\n' "$DISTRO" "$DISTRO_VER"
IN_DISTRO 'uname -sr; id -u' | head -2 | sed 's/^/       /'
ok "inside the distribution"

# ------------------- 1. what bootstrap says on a distribution with nothing
head_ "2. Bootstrap on a distribution with nothing installed"
OUT="$(IN_DISTRO 'cd /work/repo && ./bootstrap.sh' 2>&1)"
RC=$?

# The real status matters more than the text: a bootstrap that dies with a
# shell error never writes "FAILED", and a broad grep would give a false pass.
if [ "$RC" -ne 0 ]; then
  ok "bootstrap refused to continue without dependencies (exited $RC)"
else
  bad "bootstrap exited 0 on a distribution with nothing installed"
fi

HINT="$(printf '%s' "$OUT" | grep -oE '(sudo )?(apt-get|dnf|pacman|zypper) [^"\\]*' | head -1)"
if [ -n "$HINT" ]; then
  ok "gave the exact install command:"
  printf '         %s\n' "$HINT"
else
  bad "gave no install command (a useless message for a newcomer)"
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
  ok "package manager recognised: $MGR"
else
  bad "could not recognise the package manager from the hint"
fi

# ------------------------------- 2. install what it asked for and carry on
head_ "3. Running the command it gave (already root here, so without 'sudo')"
# Uses the LITERAL command bootstrap printed, minus the "sudo": if the
# instruction is wrong or incomplete, this test finds out - which is how the
# missing `git` showed up, that a hand-written list would have forgotten.
INSTALL_CMD="$(printf '%s' "$HINT" | sed 's/sudo //g')"
if [ -n "$INSTALL_CMD" ]; then
  printf '         %s\n' "$INSTALL_CMD"
  # APT::Sandbox::User=root is not a project requirement: inside a user
  # namespace apt cannot switch to the _apt user (setgroups is EPERM), so it
  # is told to stay root. On a real machine the original command works without
  # this. It goes in a config file so it applies to EVERY apt command in this
  # run - passing -o on the line would only reach the last one in the "&&".
  IN_DISTRO 'mkdir -p /etc/apt/apt.conf.d && printf "APT::Sandbox::User \"root\";\n" > /etc/apt/apt.conf.d/00sandbox'
  IN_DISTRO "export DEBIAN_FRONTEND=noninteractive; $INSTALL_CMD" >"$WORK/install.log" 2>&1
  rc_install=$?
  printf '         (ran the command bootstrap gave, word for word)\n'

  # apt's exit code is NOT the assertion. Inside a user namespace the dpkg
  # postinst always fails on a chown (/var/log/apt/term.log), so apt exits
  # 100 even having installed everything. What proves the install is the tool
  # being there - and bootstrap, which consumes it, decides that in the next
  # step.
  if IN_DISTRO 'command -v cc >/dev/null && command -v git >/dev/null && pkg-config --exists sdl2'; then
    ok "compiler, git and SDL2 present"
    [ "$rc_install" -ne 0 ] && printf '         (apt exited %d: the dpkg postinst does a chown, not allowed here - not a project failure)\n' "$rc_install"
  else
    bad "the install did not complete (cc, git or SDL2 missing):"
    tail -6 "$WORK/install.log" | sed 's/^/         /'
  fi
fi

OUT2="$(IN_DISTRO 'cd /work/repo && ./bootstrap.sh' 2>&1)"
RC2=$?
if [ "$RC2" -ne 0 ]; then
  bad "bootstrap failed even with the dependencies installed (exit $RC2):"
  printf '%s\n' "$OUT2" | tail -10 | sed 's/^/         /'
else
  ok "bootstrap completed (exit 0):"
  printf '%s' "$OUT2" | grep -E 'ok ' | tail -5 | sed 's/^/         /'
fi

IN_DISTRO 'test -x /work/repo/.tools/gbrecomp' \
  && ok "gbrecomp available" || bad "gbrecomp missing"

# ------------------------------------------------------ 3. compile for real
head_ "4. Compiling the ROM"
OUT3="$(IN_DISTRO 'cd /work/repo && ./build.sh /work/rom.gbc' 2>&1)"
RC3=$?
printf '%s' "$OUT3" | grep -E 'ok |FAILED|note' | sed 's/^/         /'

if [ -z "$ROM" ]; then
  # Synthetic ROM: this is the case that must NOT pass. It never reaches a
  # frame, so the smoke test has to fail - and fail fast. If build.sh hangs
  # here, this test times out and reports the bug by itself.
  if [ "$RC3" -eq 0 ]; then
    bad "the synthetic ROM passed the smoke test (it should not: it never produces a frame)"
  else
    ok "the build stopped at the smoke test, as expected for the synthetic ROM"
  fi
  if printf '%s' "$OUT3" | grep -q 'SMOKE_TIMEOUT=1'; then
    ok "offered the way to skip that check"
  else
    bad "failed without explaining how to continue"
    printf '%s\n' "$OUT3" | tail -8 | sed 's/^/         /'
  fi
else
  if [ "$RC3" -eq 0 ]; then
    ok "build completed (validation, generation, compilation and run test)"
  else
    bad "the build failed (exit $RC3):"
    printf '%s\n' "$OUT3" | tail -12 | sed 's/^/         /'
  fi
fi

# ------------------------------------------------------ 4. does it run?
head_ "5. Does the binary run inside the distribution?"
if IN_DISTRO 'test -x /work/repo/build/ghost-babel'; then
  ok "binary produced"
  IN_DISTRO 'cd /work/repo && ./build/ghost-babel --headless --limit 3000000' >/dev/null 2>&1
  check=$?
  [ "$check" -eq 0 ] && ok "the game executes the guest without hanging" \
                     || bad "the game hung or crashed (exit $check)"
else
  bad "no binary was produced"
fi

printf '\n'
if [ "$FAIL" -eq 0 ]; then
  printf '\033[32m%d checks passed on %s %s.\033[0m\n' "$PASS" "$DISTRO" "$DISTRO_VER"
  exit 0
fi
printf '\033[31m%d failed\033[0m (of %d) on %s %s\n' "$FAIL" "$((PASS+FAIL))" "$DISTRO" "$DISTRO_VER"
printf 'To keep the logs: KEEP_WORK=1 ./tests/container.sh\n'
exit 1