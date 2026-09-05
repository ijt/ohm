#!/usr/bin/env bash
# One-line installer for Shinto on an Omarchy box:
#
#   curl -fsSL https://raw.githubusercontent.com/ijt/shinto/main/install.sh | bash
#
# Idempotent -- re-running this (e.g. to pick up an update) just pulls the
# latest commit into the existing clone and rebuilds/reinstalls over it.
# Everything it does past the initial `git clone`/`git pull` is exactly
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

# Arch/Omarchy package names for everything CMakeLists.txt and
# downloads-tui/go.mod need at build time -- see packaging/PKGBUILD's own
# depends/makedepends for the subset of this that a packaged install
# still needs at runtime. base-devel brings the C++ compiler (Qt itself
# doesn't); no ninja -- this uses the same plain default-generator `cmake
# -S/-B` + `cmake --build` the README's own "From source" steps do.
PACMAN_PKGS=(git cmake base-devel qt6-base qt6-webengine lua54 go)

if command -v pacman >/dev/null 2>&1; then
  missing=()
  for pkg in "${PACMAN_PKGS[@]}"; do
    pacman -Qi "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
  done
  if [[ ${#missing[@]} -gt 0 ]]; then
    echo "install.sh: installing missing packages: ${missing[*]}"
    # A pipe (curl | bash) leaves no usable stdin for pacman's own y/N
    # prompts, so confirm once here (reading straight from the terminal,
    # not the exhausted stdin the script itself came in on) and then run
    # pacman non-interactively. `-r /dev/tty` isn't a reliable predictor
    # here -- the device node is normally readable regardless of whether
    # this process actually has a controlling terminal attached, so it's
    # the read itself (not a pre-check) that has to be allowed to fail.
    if read -r -p "Install these via sudo pacman? [Y/n] " reply 2>/dev/null </dev/tty; then
      :
    else
      reply="y" # no controlling terminal to ask on
    fi
    case "$reply" in
      [nN]*)
        echo "install.sh: skipping -- install them yourself, then re-run this script." >&2
        exit 1
        ;;
    esac
    sudo pacman -S --needed --noconfirm "${missing[@]}"
  fi
else
  echo "install.sh: no pacman -- skipping automatic dependency install." >&2
  echo "  Make sure these are on PATH/installed before continuing: git, cmake," >&2
  echo "  ninja (or make), a C++ compiler, Qt6 (base + webengine + sql), lua5.4, go." >&2
fi

if [[ -d "$SHINTO_SRC/.git" ]]; then
  echo "install.sh: updating existing clone at $SHINTO_SRC"
  git -C "$SHINTO_SRC" pull --ff-only
else
  echo "install.sh: cloning to $SHINTO_SRC"
  mkdir -p "$(dirname "$SHINTO_SRC")"
  git clone "$REPO_URL" "$SHINTO_SRC"
fi

echo "install.sh: building the C++ daemon"
cmake -S "$SHINTO_SRC/app" -B "$SHINTO_SRC/app/build" -DCMAKE_BUILD_TYPE=Release
# A first, fully-clean build of QtWebEngine-linked C++ is the slow part of
# this whole script -- plain `cmake --build` doesn't parallelize on the
# default Makefiles generator without being told to.
cmake --build "$SHINTO_SRC/app/build" --parallel "$(nproc)"

echo "install.sh: building the downloads view (shinto-downloads)"
(cd "$SHINTO_SRC/downloads-tui" && go build -o shinto-downloads .)

echo "install.sh: running ./shinto install"
"$SHINTO_SRC/shinto" install

# ./shinto install only enables+starts the service if it wasn't already
# running -- an update re-run needs an explicit restart to actually pick
# up the rebuilt binary, so just always do it.
systemctl --user restart shinto.service

echo
echo "install.sh: done. Source lives at $SHINTO_SRC (re-run this script to update)."
