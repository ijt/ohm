#!/usr/bin/env bash
# One-line installer for Ohm on an Omarchy box:
#
#   curl -fsSL https://raw.githubusercontent.com/ijt/ohm-browser/main/install.sh | bash
#
# Installs the newest release (the highest v* tag), not whatever is on
# main. Set OHM_REF to a tag, branch or commit to install that instead,
# e.g. OHM_REF=main for the latest unreleased code.
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

OHM_SRC="${OHM_SRC:-$HOME/.local/share/ohm-browser/src}"
REPO_URL="${OHM_REPO_URL:-https://github.com/ijt/ohm-browser.git}"

# Arch/Omarchy package names for everything CMakeLists.txt needs at build
# time -- see packaging/PKGBUILD's own depends/makedepends for the subset of
# this that a packaged install still needs at runtime. base-devel brings the
# C++ compiler (Qt itself doesn't); no ninja -- this uses the same plain
# default-generator `cmake -S/-B` + `cmake --build` the README's own "From
# source" steps do.
PACMAN_PKGS=(git cmake base-devel qt6-base qt6-webengine lua54)
# Phone passkeys: ohm-browser-passkey is Rust, and checks sites against the
# public suffix list. A rustup toolchain already on PATH is fine, and
# pacman's rust would conflict with the rustup package, so only ask for
# rust when there's no cargo at all.
PACMAN_PKGS+=(publicsuffix-list)
command -v cargo >/dev/null 2>&1 || PACMAN_PKGS+=(rust)

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
if [[ -z ${OHM_INSTALL_REEXEC:-} ]]; then
  if [[ -d "$OHM_SRC/.git" ]]; then
    echo "install.sh: updating existing clone at $OHM_SRC"
    # --force: a moved tag should win over the clone's stale copy of it.
    git -C "$OHM_SRC" fetch --tags --force --prune origin
  else
    echo "install.sh: cloning to $OHM_SRC"
    mkdir -p "$(dirname "$OHM_SRC")"
    git clone "$REPO_URL" "$OHM_SRC"
  fi

  ref=${OHM_REF:-}
  if [[ -z $ref ]]; then
    ref=$(git -C "$OHM_SRC" tag -l 'v*' --sort=-v:refname | head -n 1)
    if [[ -z $ref ]]; then
      echo "install.sh: no v* release tags in $REPO_URL; set OHM_REF=main to install the latest code" >&2
      exit 1
    fi
  fi

  # A branch is followed (reset to the fetched origin copy, so a re-run
  # upgrades it); a tag or commit is checked out detached.
  if git -C "$OHM_SRC" rev-parse --verify --quiet "refs/remotes/origin/$ref" >/dev/null; then
    echo "install.sh: checking out branch $ref"
    git -C "$OHM_SRC" checkout --quiet -B "$ref" "origin/$ref"
  else
    echo "install.sh: checking out $ref"
    git -C "$OHM_SRC" checkout --quiet --detach "$ref"
  fi

  # This script came from main (or wherever it was run from); the code is
  # now $ref. Hand off to $ref's own install.sh so its steps (package list,
  # build flags) match the code it builds. Only an install.sh that knows
  # this handoff can take it: older releases' copies would try to
  # `git pull` the detached checkout, so those continue with this script.
  if grep -q OHM_INSTALL_REEXEC "$OHM_SRC/install.sh" &&
    ! cmp -s "${BASH_SOURCE[0]:-}" "$OHM_SRC/install.sh"; then
    echo "install.sh: continuing with $ref's install.sh"
    OHM_INSTALL_REEXEC=1 OHM_SRC="$OHM_SRC" exec bash "$OHM_SRC/install.sh"
  fi
fi

echo "install.sh: building the C++ daemon"
cmake -S "$OHM_SRC/app" -B "$OHM_SRC/app/build" -DCMAKE_BUILD_TYPE=Release
# A first, fully-clean build of QtWebEngine-linked C++ is the slow part of
# this whole script -- plain `cmake --build` doesn't parallelize on the
# default Makefiles generator without being told to.
cmake --build "$OHM_SRC/app/build" --parallel "$(nproc)"

echo "install.sh: running ./ohm install"
"$OHM_SRC/ohm" install

# ./ohm install only enables+starts the service if it wasn't already
# running -- an update re-run needs an explicit restart to actually pick
# up the rebuilt binary, so just always do it.
systemctl --user restart ohm-browser.service

echo
echo "install.sh: done. Source lives at $OHM_SRC (re-run this script to update)."
