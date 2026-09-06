> ### About this fork
>
> This is a fork of **[NaGaa95/gdash_nx](https://github.com/NaGaa95/gdash_nx)**.
> **All credit for the port itself goes to NaGaa95** — they did the actual work
> of getting Geometry Dash running on the Switch. The README below is theirs,
> unchanged.
>
> I am not a developer and know very little programming. Every code change in
> this fork was written by [Claude Code](https://claude.com/claude-code); my
> part was describing the problems I was hitting, testing the builds on my
> Switch, and reporting back what happened. The changes were arrived at by
> measuring on real hardware rather than guessing, but they have had far less
> review than upstream code, so treat them accordingly.
>
> **What this fork changes:** the periodic stutter (four separate causes, idle
> stall time down from ~11.6% to ~1%), the ~20-second autosave freeze
> ([#8](https://github.com/NaGaa95/gdash_nx/issues/8)), the stray corner taps in
> menus ([#7](https://github.com/NaGaa95/gdash_nx/issues/7)), saves that persist
> when you leave a level, and configurable controls. See
> **[CHANGELOG.md](CHANGELOG.md)** for the detail.
>
> **No game files are included here**, exactly as upstream — you supply your own
> legally-owned copy. Only the wrapper source is in this repository.

---

<div align=center>

<img src="extras/banner.png" alt="Banner" width="60%">

</div>
<h1 align=center>Geometry Dash · Switch Port</h1>

A wrapper/port of the Android releases of **Geometry Dash**, **Meltdown**,
**SubZero**, and **World**. It loads the original arm64 game binaries —
`libcocos2dcpp.so` (RobTop's cocos2d-x 2.2 fork + the game) and the stock
`libfmod.so` — patches them and runs them inside a minimal Android-like
environment natively on the Switch.

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

`config.txt` is created on first run:

* `screen_width` / `screen_height` — render resolution; `-1` auto-picks
  1280x720 handheld / 1920x1080 docked.
* `cursor_speed` — pointer speed of the stick-driven cursor, px/s at 720p.

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

If you enjoy my work and want to support me :

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/D1D1P2MOG)

### Legal

This project has no affiliation with RobTop Games. "Geometry Dash" and
related marks belong to their respective owners. No assets or program code
from the original game or its Android release are included here. We do not
condone piracy; users must own a legal copy of the game.

Unless noted otherwise, the source in this repository is under the MIT
License (see LICENSE).
