<div align=center>

<img src="extras/banner.png" alt="Banner" width="60%">

</div>
<h1 align=center>Geometry Dash · Switch Port</h1>

A wrapper/port of the Android releases of **Geometry Dash**, **Meltdown**,
**SubZero**, and **World**. It loads the original arm64 game binaries —
`libcocos2dcpp.so` (RobTop's cocos2d-x 2.2 fork + the game) and the stock
`libfmod.so` — patches them and runs them inside a minimal Android-like
environment natively on the Switch.

### About this fork

This is an unofficial fork of
**[NaGaa95/gdash_nx](https://github.com/NaGaa95/gdash_nx)** with a set of
stutter, save and control fixes. **All credit for the port itself goes to
NaGaa95**, and to Andy Nguyen and fgsfds, whose loader it is built on.
Everything below this section is NaGaa95's README.

I am not a developer and know very little programming. The changes here were made
entirely with [Claude Code](https://claude.com/claude-code). My part was describing
the problems I ran into while playing and testing the results on hardware. The
reasoning behind each change is in [CHANGELOG.md](CHANGELOG.md) so anyone can check
it, and the code comments explain what was measured and why.

**What this fork changes:**

* **The periodic stutter is gone.** Four separate causes, found by instrumenting
  the frame loop. Idle time lost to stalls dropped from ~11.6% to ~1%, and
  asset opens on the render thread reach zero once warmed.
* **The ~20 second freeze is gone** ([#8](https://github.com/NaGaa95/gdash_nx/issues/8)).
  The autosave was blocking the render thread for ~253 ms mid-level.
* **Menu buttons no longer press random things** ([#7](https://github.com/NaGaa95/gdash_nx/issues/7)).
* **Saves persist** when you leave a level or close the game.
* **Controls are configurable**, including which buttons do what.

Full detail in **[CHANGELOG.md](CHANGELOG.md)**.

No game files are included here, exactly as upstream — you supply your own
legally-owned copy.

### How to install

You need a **2.2.14x arm64** Android release; extract the files from your own
legally-owned copy of the game.

Create a separate folder for each game you install and fill it:

```
/switch/<anything>/
  gdash_nx.nro
  libcocos2dcpp.so        # from the APK's lib/arm64-v8a/
  libfmod.so              # from the APK's lib/arm64-v8a/
  assets/                 # the entire contents of the APK's assets/ folder
  config.txt              # created on first run
  prefs.txt               # created on first run; includes anonymous network ID
  cacert.pem               # generated from the Switch firmware trust store
  save/                    # created on first run; game saves
```

For example, `/switch/gdash_full`, `/switch/gdash_meltdown`,
`/switch/gdash_subzero`, and `/switch/gdash_world` each keep an independent
save. Split APK/XAPK releases usually store the assets in the base APK and the
native libraries in `config.arm64_v8a.apk`. An `armeabi-v7a`-only release is
32-bit and cannot supply libraries for this arm64 port.

### Notes

This will not run in applet/album mode — it needs the full memory of a game
override. Launch it by holding **R** while opening an installed title, or use
a forwarder.

`config.txt` is created on first run, and rewritten each launch so options
added by a newer build show up instead of silently using defaults:

* `screen_width` / `screen_height` — render resolution; `-1` auto-picks
  1280x720 handheld / 1920x1080 docked.
* `cursor_speed` — pointer speed of the stick-driven cursor, px/s at 720p.
* `click_buttons`, `left_buttons`, `right_buttons`, `back_buttons` — button
  mapping; see [Controls](#controls).
* `click_zone_x/y`, `left_zone_x/y`, `right_zone_x/y` — where each synthesised
  tap lands, in percent of the screen.
* `show_zones` — draw a marker at each tap position so they can be aimed.

### Controls

The Switch has no touchscreen input in docked mode and the game is touch-only,
so button presses are turned into synthesised taps. Everything here is
configurable in `config.txt`.

| Where | Action | Default | Config key |
|---|---|---|---|
| Anywhere | Touch the screen | Touchscreen | — |
| Anywhere | Tap — jump, confirm, press a button | **A · ZR · R · ZL · L** | `click_buttons` |
| Menus | Aim the cursor | **Left / right stick** | `cursor_speed` |
| Level select | Previous level | **D-Pad Left** | `left_buttons` |
| Level select | Next level | **D-Pad Right** | `right_buttons` |
| Anywhere | Back — pause, or leave a menu | **B · Plus** | `back_buttons` |

**How a tap picks its position.** In a level any tap jumps, so where it lands
does not matter. In a menu it very much does. So a tap goes to the cursor while
the cursor is on screen, and to `click_zone` otherwise — aimed by default at the
level box on the level-select screen. Move a stick and the cursor takes over.

Button lists are comma-separated, and any list can be set to `none` to disable
that control. Valid names:

```
A  B  X  Y  L  R  ZL  ZR  Plus  Minus  Up  Down  Left  Right  LStick  RStick
```

**Aiming the taps.** `click_zone`, `left_zone` and `right_zone` are screen
positions in percent, so they hold in handheld and docked alike. Set
`show_zones 1` to draw a marker at each one — green for the click, orange for
left, blue for right — line them up, then set it back to `0`.

The left/right defaults aim at the **level-select arrows**. Platformer levels
draw their movement arrows elsewhere on screen, so re-aim them (or set them to
`none`) if you play those.

### Online features

Public leaderboards and level search/download use RobTop's native Boomlings HTTPS
client embedded in `libcocos2dcpp.so`; they do not require Google Play Games.
The port supplies the missing Switch socket ABI conversions, reports the real
console connection state, and keeps a random anonymous network ID in
`prefs.txt`. It does not read a Nintendo account or console identifier.

TLS peer and hostname verification are enabled at load time. `cacert.pem` is
regenerated from the Switch firmware's enabled public root certificates, so no
certificate file needs to be copied beside the NRO. Google Play sign-in stays
disabled. The public global leaderboard is fetched without the stock client's
anonymous score-upload bootstrap. Level-specific leaderboards require a logged-in
Geometry Dash account because Boomlings requires account authorization for that
endpoint. Score submission, comments, uploads, messaging, and other social
endpoints are outside the supported/tested scope of this port.

Networking is always enabled. Release builds do not write network diagnostic
logs or record request/response payloads, the anonymous ID, or credentials.

### How to build

Install the devkitPro Switch toolchain and portlibs:

```sh
pacman -S devkitA64 switch-tools libnx switch-sdl2 switch-mesa switch-libdrm_nouveau
```

Then `make`.

### Credits

* TheOfficialFloW for the original Android so-loader.
* hatoving for the Geometry Dash PS Vita port this one is based on.
* fgsfds for max_nx / the Switch so-loader groundwork reused here.

### Support

The Ko-fi below belongs to **NaGaa95**, who made this port — it is not mine.
I only fixed the stutter, the saves and the controls, so if you would like to
support the work, support him.

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no affiliation with RobTop Games. "Geometry Dash" and
related marks belong to their respective owners. No assets or program code
from the original game or its Android release are included here. We do not
condone piracy; users must own a legal copy of the game.

Unless noted otherwise, the source in this repository is under the MIT
License (see LICENSE).
