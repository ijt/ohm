# Draft: Omarchy Suggestions discussion

**Title:** Ohm: an Omarchy-native page browser (Install > Browser proposal)

**Category:** Suggestions — https://github.com/basecamp/omarchy/discussions/categories/suggestions

---

## Pitch

Omarchy already treats the compositor as the window manager people wish browsers were. Ohm takes that seriously: **one window is one page**, Hyprland groups are the tab bar, and the omnibox is a gate you walk through and then it disappears.

It is a small Qt6/QtWebEngine app ([ijt/ohm](https://github.com/ijt/ohm)), not another Chromium skin. Warm daemon opens are well under 150ms. It picks up Omarchy theme colors for the overlay.

## Proposal (v1 — optional, not a Chromium replacement)

Add Ohm beside Firefox/Zen under **Install > Browser**, and list it under **Setup > Defaults > Browser** once installed:

- `omarchy install browser ohm` → install AUR `ohm-git`, enable `ohm.service`
- `omarchy default browser ohm` → XDG default for links / Super+Shift+B
- Tag `ohm` in `default/hypr/apps/browser.lua` like other browsers
- Add `ohm*` to the browsers `omarchy-launch-webapp` hands web apps to, so
  web apps follow the default browser the way they do for Brave or Vivaldi:

  ```diff
  -google-chrome* | brave* | microsoft-edge* | opera* | vivaldi* | helium*) ;;
  +google-chrome* | brave* | microsoft-edge* | opera* | vivaldi* | helium* | ohm*) ;;
  ```

  Ohm takes Chromium's `--app=URL`, and a Ohm window is already
  chrome-less, so a web app is just a page. What web apps need works:
  permission prompts (camera, mic, notifications, location; remembered per
  site), desktop notifications through the notification daemon (a click
  focuses the app's window), and screen sharing through
  xdg-desktop-portal-hyprland's picker. `omarchy-launch-or-focus` finds
  Ohm web apps by title, since titles are the page titles.

## Explicit non-goals for v1

- **Do not** remove Chromium from the base system
- No extension store / Chrome Web Store story

Chromium stays in the base system as the escape hatch (extensions, sites
that need a full Chrome).

## Why this belongs in Omarchy

Firefox and Zen are fine browsers that fight the desktop less than Chrome, but they still reinvent tabs and window chrome. Ohm's UI is intentionally empty so Hyprland can do what it already does well. That matches Omarchy's taste more closely than "install yet another full browser."

## Packaging status

- Repo: https://github.com/ijt/ohm
- `cmake --install` layout: binary, `.desktop`, icon, systemd user unit
- AUR recipe in-tree at `packaging/PKGBUILD` (`ohm-git`)

Happy to open a PR against `basecamp/omarchy` wiring the install/default/remove/menu surfaces the same way Zen does, once `ohm-git` is on the AUR (or with whatever package source you prefer).

## Demo

_(attach short GIF: empty gate → navigate → Hyprland group tabs showing page titles → YouTube fullscreen)_
