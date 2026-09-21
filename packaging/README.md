# Packaging Shinto

## Files

| File | Role |
|------|------|
| `shinto.desktop` | XDG application entry (`Exec=shinto %u`) |
| `shinto.service.in` | systemd user unit template (filled by CMake) |
| `PKGBUILD` | AUR `shinto-git` build recipe |
| `shinto-git.install` | Post-install hint to enable the daemon |
| `shinto/PKGBUILD` | Versioned `shinto` recipe for GitHub Releases |

`cmake --install` (with `-DCMAKE_INSTALL_PREFIX=/usr`) installs the binary, desktop file, icon, and generated unit. Each `PKGBUILD`'s own `package()` step separately stages `hypr.lua`, `theme-set-hook.sh`, and the `downloads-panel/` Quickshell plugin under `/usr/share/shinto/`.

## GitHub Release package

Tagged releases attach a `shinto-<ver>-<rel>-x86_64.pkg.tar.zst` built from `shinto/PKGBUILD`. Install with:

```bash
sudo pacman -U shinto-*-x86_64.pkg.tar.zst
systemctl --user enable --now shinto.service
```

To rebuild one locally from a tag:

```bash
git archive --format=tar.gz --prefix=shinto-0.0.1/ -o packaging/shinto/shinto-0.0.1.tar.gz v0.0.1
cd packaging/shinto
makepkg -f
```

## Local `shinto-git` smoke test

```bash
cd packaging
makepkg -si
systemctl --user enable --now shinto.service
xdg-settings get default-web-browser   # set to shinto.desktop when ready
```

## Publishing to the AUR

1. Create an [AUR account](https://aur.archlinux.org) and add your SSH key.
2. `git clone ssh://aur@aur.archlinux.org/shinto-git.git`
3. Copy `PKGBUILD` and `shinto-git.install` into that repo.
4. `makepkg --printsrcinfo > .SRCINFO`
5. Commit and `git push`.

Omarchy's optional-browser installers call `omarchy-pkg-aur-add <pkg>`; once `shinto-git` is on the AUR, an Omarchy PR can add `omarchy install browser shinto`.
