#!/usr/bin/env bash
# Runs as root inside an Arch container (see .github/workflows/release.yml).
# TAG is v0.0.2; writes ohm-*-*.pkg.tar.* into packaging/ohm/ on the
# mounted repo. makepkg refuses root, so the build runs as an unprivileged
# user after deps are installed.
set -euo pipefail

if [[ -z ${TAG:-} ]]; then
  echo "build-release-pkg.sh: TAG is required (e.g. v0.0.2)" >&2
  exit 1
fi
ver=${TAG#v}

# pacman 7 sandboxes downloads with Landlock and a dedicated user. The ARM
# release container cannot apply that ruleset, so `pacman -S` aborts with
# "Landlock ruleset could not be applied" and the GitHub Release is never
# created (this is why v0.0.3 has a tag and no release). Older pacman
# rejects the key, so only add it when this binary accepts it. Re-check
# after -Syu, which may have just upgraded pacman.
disable_pacman_sandbox() {
  if grep -q '^DisableSandbox$' /etc/pacman.conf; then
    return
  fi
  local probe
  probe=$(mktemp)
  printf '[options]\nDisableSandbox\n' >"$probe"
  if pacman-conf --config "$probe" >/dev/null 2>&1; then
    sed -i '/^\[options\]/a DisableSandbox' /etc/pacman.conf
  fi
  rm -f "$probe"
}

pacman-key --init
pacman-key --populate archlinux
disable_pacman_sandbox
pacman -Sy --noconfirm --needed archlinux-keyring
disable_pacman_sandbox
pacman -Syu --noconfirm
disable_pacman_sandbox
pacman -S --needed --noconfirm base-devel cmake qt6-base qt6-webengine lua54 git zstd \
  rust publicsuffix-list

id builder >/dev/null 2>&1 || useradd -m builder
# The GHA checkout is root-owned on the mount; copy so the builder can
# git-archive and makepkg without fighting permissions.
rm -rf /home/builder/src
cp -a /src /home/builder/src
chown -R builder:builder /home/builder/src

su - builder -c "set -euo pipefail
  git config --global --add safe.directory /home/builder/src
  cd /home/builder/src
  cmake_ver=\$(sed -n 's/^project(ohm VERSION \\([^ ]*\\).*/\\1/p' app/CMakeLists.txt)
  if [[ \$cmake_ver != '$ver' ]]; then
    echo \"CMakeLists version \$cmake_ver does not match tag $ver\" >&2
    exit 1
  fi
  sed -i 's/^pkgver=.*/pkgver=$ver/' packaging/ohm/PKGBUILD
  git archive --format=tar.gz --prefix=ohm-${ver}/ -o packaging/ohm/ohm-${ver}.tar.gz HEAD
  cd packaging/ohm
  PKGEXT='.pkg.tar.zst' makepkg -f --noconfirm
"

install -d /src/packaging/ohm
cp -a /home/builder/src/packaging/ohm/ohm-"$ver"-*.pkg.tar.* /src/packaging/ohm/
