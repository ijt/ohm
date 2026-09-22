#!/usr/bin/env bash
# Runs as root inside an Arch container (see .github/workflows/release.yml).
# TAG is v0.0.2; writes shinto-*-*.pkg.tar.* into packaging/shinto/ on the
# mounted repo. makepkg refuses root, so the build runs as an unprivileged
# user after deps are installed.
set -euo pipefail

if [[ -z ${TAG:-} ]]; then
  echo "build-release-pkg.sh: TAG is required (e.g. v0.0.2)" >&2
  exit 1
fi
ver=${TAG#v}

pacman-key --init
pacman-key --populate archlinux
pacman -Sy --noconfirm --needed archlinux-keyring
pacman -Syu --noconfirm
pacman -S --needed --noconfirm base-devel cmake qt6-base qt6-webengine lua54 git zstd

id builder >/dev/null 2>&1 || useradd -m builder
# The GHA checkout is root-owned on the mount; copy so the builder can
# git-archive and makepkg without fighting permissions.
rm -rf /home/builder/src
cp -a /src /home/builder/src
chown -R builder:builder /home/builder/src

su - builder -c "set -euo pipefail
  git config --global --add safe.directory /home/builder/src
  cd /home/builder/src
  cmake_ver=\$(sed -n 's/^project(shinto-app VERSION \\([^ ]*\\).*/\\1/p' app/CMakeLists.txt)
  if [[ \$cmake_ver != '$ver' ]]; then
    echo \"CMakeLists version \$cmake_ver does not match tag $ver\" >&2
    exit 1
  fi
  sed -i 's/^pkgver=.*/pkgver=$ver/' packaging/shinto/PKGBUILD
  git archive --format=tar.gz --prefix=shinto-${ver}/ -o packaging/shinto/shinto-${ver}.tar.gz HEAD
  cd packaging/shinto
  PKGEXT='.pkg.tar.zst' makepkg -f --noconfirm
"

install -d /src/packaging/shinto
cp -a /home/builder/src/packaging/shinto/shinto-"$ver"-*.pkg.tar.* /src/packaging/shinto/
