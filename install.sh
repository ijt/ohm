#!/usr/bin/env bash
# One-line installer for Shinto on an Omarchy box:
#
#   curl -fsSL https://raw.githubusercontent.com/ijt/shinto/main/install.sh | bash
#
# Installs the newest release (the highest v* tag), not whatever is on
# main. Set SHINTO_REF to a tag, branch or commit to install that instead,
# e.g. SHINTO_REF=main for the latest unreleased code.
#
# Idempotent -- re-running this (e.g. to pick up an update) fetches into
# the existing clone, checks out the newest release again, and
# rebuilds/reinstalls over it. Everything it does past the clone/checkout is exactly
# what a manual "From source" install does (see README.md); this script
# is only the dependency-install + clone/build/install glue around that.

set -euo pipefail

if [[ $EUID -eq 0 ]]; then
  echo "install.sh: don't run this as root -- it installs into \$HOME and" >&2
  echo "  manages a --user systemd service; re-run as your normal user" >&2
  echo "  (it'll ask for sudo itself, just for the pacman step)." >&2
  exit 1
fi

SHINTO_SRC="${SHINTO_SRC:-$HOME/.local/share/shinto/src}"
REPO_URL="${SHINTO_REPO_URL:-https://github.com/ijt/shinto.git}"

# Arch/Omarchy package names for everything CMakeLists.txt needs at build
# time -- see packaging/PKGBUILD's own depends/makedepends for the subset of
# this that a packaged install still needs at runtime. base-devel brings the
# C++ compiler (Qt itself doesn't); no ninja -- this uses the same plain
# default-generator `cmake -S/-B` + `cmake --build` the README's own "From
# source" steps do.
PACMAN_PKGS=(git cmake base-devel qt6-base qt6-webengine lua54)

if command -v pacman >/dev/null 2>&1; then
  missing=()
  for pkg in "${PACMAN_PKGS[@]}"; do
    pacman -Qi "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
  done
  if [[ ${#missing[@]} -gt 0 ]]; then
    echo "install.sh: missing packages: ${missing[*]}"
    # A pipe (curl | bash) leaves no usable stdin for pacman's own y/N
    # prompts, so confirm once here (reading straight from the terminal,
    # not the exhausted stdin the script itself came in on) and then run
    # pacman non-interactively. `-r /dev/tty` isn't a reliable predictor
    # here -- the device node is normally readable regardless of whether
    # this process actually has a controlling terminal attached, so it's
    # the read itself (not a pre-check) that has to be allowed to fail.
    #
    # Prompt on stdout, not `read -p`: that writes the prompt to stderr,
    # and we redirect stderr on the read so a missing /dev/tty doesn't
    # print a bash error. Combined, that used to swallow the prompt and
    # look like a hang.
    printf 'Install these via sudo pacman? [Y/n] '
    if read -r reply </dev/tty 2>/dev/null; then
      :
    else
      echo # no tty: finish the prompt line we just printed
      reply="y" # no controlling terminal to ask on
    fi
    case "$reply" in
      [nN]*)
        echo "install.sh: skipping -- install them yourself, then re-run this script." >&2
        exit 1
        ;;
    esac
    # --verbose dumps Root/DB/cache paths up front. It does not add
    # per-byte download logs -- that's --debug, which is much noisier.
    echo "install.sh: running sudo pacman -S --needed --noconfirm --verbose ${missing[*]}"
    sudo pacman -S --needed --noconfirm --verbose "${missing[@]}"
  fi
else
  echo "install.sh: no pacman -- skipping automatic dependency install." >&2
  echo "  Make sure these are on PATH/installed before continuing: git, cmake," >&2
  echo "  ninja (or make), a C++ compiler, Qt6 (base + webengine + sql), lua5.4, go." >&2
fi

# A re-exec from the first pass below has already checked out the ref.
if [[ -z ${SHINTO_INSTALL_REEXEC:-} ]]; then
  if [[ -d "$SHINTO_SRC/.git" ]]; then
    echo "install.sh: updating existing clone at $SHINTO_SRC"
    # --force: a moved tag should win over the clone's stale copy of it.
    git -C "$SHINTO_SRC" fetch --tags --force --prune origin
  else
    echo "install.sh: cloning to $SHINTO_SRC"
    mkdir -p "$(dirname "$SHINTO_SRC")"
    git clone "$REPO_URL" "$SHINTO_SRC"
  fi

  ref=${SHINTO_REF:-}
  if [[ -z $ref ]]; then
    ref=$(git -C "$SHINTO_SRC" tag -l 'v*' --sort=-v:refname | head -n 1)
    if [[ -z $ref ]]; then
      echo "install.sh: no v* release tags in $REPO_URL; set SHINTO_REF=main to install the latest code" >&2
      exit 1
    fi
  fi

  # A branch is followed (reset to the fetched origin copy, so a re-run
  # upgrades it); a tag or commit is checked out detached.
  if git -C "$SHINTO_SRC" rev-parse --verify --quiet "refs/remotes/origin/$ref" >/dev/null; then
    echo "install.sh: checking out branch $ref"
    git -C "$SHINTO_SRC" checkout --quiet -B "$ref" "origin/$ref"
  else
    echo "install.sh: checking out $ref"
    git -C "$SHINTO_SRC" checkout --quiet --detach "$ref"
  fi

  # This script came from main (or wherever it was run from); the code is
  # now $ref. Hand off to $ref's own install.sh so its steps (package list,
  # build flags) match the code it builds. Only an install.sh that knows
  # this handoff can take it: older releases' copies would try to
  # `git pull` the detached checkout, so those continue with this script.
  if grep -q SHINTO_INSTALL_REEXEC "$SHINTO_SRC/install.sh" &&
    ! cmp -s "${BASH_SOURCE[0]:-}" "$SHINTO_SRC/install.sh"; then
    echo "install.sh: continuing with $ref's install.sh"
    SHINTO_INSTALL_REEXEC=1 SHINTO_SRC="$SHINTO_SRC" exec bash "$SHINTO_SRC/install.sh"
  fi
fi

echo "install.sh: building the C++ daemon"
cmake -S "$SHINTO_SRC/app" -B "$SHINTO_SRC/app/build" -DCMAKE_BUILD_TYPE=Release
# A first, fully-clean build of QtWebEngine-linked C++ is the slow part of
# this whole script -- plain `cmake --build` doesn't parallelize on the
# default Makefiles generator without being told to.
cmake --build "$SHINTO_SRC/app/build" --parallel "$(nproc)"

echo "install.sh: running ./shinto install"
"$SHINTO_SRC/shinto" install

# ./shinto install only enables+starts the service if it wasn't already
# running -- an update re-run needs an explicit restart to actually pick
# up the rebuilt binary, so just always do it.
systemctl --user restart shinto.service

echo
echo "install.sh: done. Source lives at $SHINTO_SRC (re-run this script to update)."
