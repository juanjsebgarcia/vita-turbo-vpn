# vita-turbo-vpn app

The LiveArea app (title ID `VPNT00001`) for vita-turbo-vpn, a VPN client
for the PS Vita. It draws the five main screens (home, profiles,
migrate, leak check, not connected) plus Details,
About, QR import and the resume-lockdown state, against a back-end
interface. The profile list and QR import are real; the
tunnel side is either a scripted mock (screenshots, development) or the
kernel module's control API (`backend_vt.c`, every release).

The display name is "Vita Turbo VPN" (interim name), set in `CMakeLists.txt`
(`VITA_APP_NAME`) and `src/strings_en.def` (`S_APP_DISPLAY_NAME`, not
translated). No VPN provider is named anywhere and there are no provider
presets: the exit-IP check is the generic one, or a profile's own
https:// URL.

## Layout

| Path | What it is |
|---|---|
| `src/app.c`, `app.h`, `app_internal.h` | Navigation, actions, input. Platform-independent |
| `src/screens.c` | Drawing for each screen |
| `src/ui.c`, `ui.h` | Palette, text styles, header, footer hints, tiles, rich text, the pipe diagram, touch targets |
| `src/draw.c`, `draw.h` | Drawing API: rectangles, rounded rectangles, dashed outlines, circles, icons, text |
| `src/draw_backend.h` | The two primitives each platform provides |
| `src/draw_vita.c` | Console back end (vita2d, one texture atlas) |
| `src/draw_host.c`, `draw_host.h` | Software back end: memory framebuffer, PNG output |
| `src/strings_en.def`, `strings.c`, `strings.h` | All UI text, by id; English |
| `src/lang/<code>.def` | The other 19 system languages ([TRANSLATIONS.md](TRANSLATIONS.md)) |
| `tools/check_strings.py` | Checks every language against English (ids, placeholders, UTF-8) |
| `tools/fetch_fallback_fonts.sh` | Fetches the host's fallback fonts (`make fallback-fonts`) |
| `src/backend.h` | What the screens need from the system |
| `src/backend_mock.c` | Scripted scenarios; real files from a folder on the computer on request |
| `src/backend_vt.c` | The real back end: real profile list and QR import, the tunnel through the kernel module's syscalls |
| `src/profiles.c`, `profiles.h` | The profile scan and the file side of QR import, shared by both back ends |
| `src/qr.c`, `qr.h` | QR decoding with quirc's relaxed grouping build |
| `src/camera_vita.c`, `camera_host.c`, `camera.h` | The rear camera (console); a placeholder (host) |
| `src/main_vita.c` | Console main loop |
| `src/main_host.c` | Screenshot and LiveArea image generator |
| `src/stb_impl.c` | stb_truetype and stb_image_write implementations |
| `tools/make_fixture.py` | A made-up profiles tree for `make shots-files` (random keys) |
| `sce_sys/` | LiveArea: icon, background, startup image, template |
| `third_party/stb/` | stb_truetype, stb_image_write, stb_image (public domain or MIT, `LICENSE`) |
| `third_party/miniz/` | miniz 3.0.2 (MIT, `LICENSE`, `VENDORED.txt`): reading `.zip` bundles |

Also built in from the rest of the repository: the profile parser
([../conf/](../conf/README.md)) and quirc ([../third_party/quirc/](../third_party/quirc/),
ISC) with the relaxed finder grouping of [../qr/quirc_relaxed.h](../qr/quirc_relaxed.h), generated
at build time from the vendored `identify.c` as the QR test app does. The
VPK carries the licences of quirc, miniz and stb in `app0:licenses/`.

Fonts are in [../third_party/fonts/](../third_party/fonts/LICENSES.md).

## Drawing

Screens call `draw.h` only. Everything is built from two platform
primitives: fill a rectangle, and blend a coverage mask in one colour.
Shared code rasterises the masks (rounded corners, rings, icons from small
vector paths, glyphs with stb_truetype at four subpixel positions) and
caches them; a rounded rectangle of any size is four cached corners plus
plain rectangles. The console copies each mask once into a 1024x1024 atlas
and draws it tinted with vita2d; the host blends it into a framebuffer. The
same code lays out both, so a screenshot is what the console shows, up to
the GPU's blending.

Text metrics follow the browser's, so the layout can be checked against a
browser rendering: the line box is the rounded ascent plus the rounded descent, and
letter spacing is added after every character.

## Screenshots: `make shots`

On macOS or Linux, with a C99 compiler:

```bash
cd app
make shots      # build-host/shots/*.png
```

`make shots` renders, with the mock back end at a fixed time:

- `main`, `profiles`, `migrate`, `leak`, `disconnected`: the five main
  screens;
- `home-<scenario>` for every mock scenario;
- the other screens and states: `profiles-many`, `profiles-scrolled`,
  `migrate-reconnect`, `migrate-held`, `leak-ok`, `leak-not-connected`,
  `details`, `details-blocked`, `details-profile`, `details-invalid`,
  `about`, `dialog-clash`, `dialog-key-notice`, `toast`;
- Lockdown: `home-lockdown-disconnected` (blocked),
  `home-connected-lockdown` (the chip), `home-lockdown-waiting` (turned on
  with no profile loaded: not blocking yet), `details-lockdown`,
  `details-lockdown-off`, `dialog-lockdown`, `dialog-disconnect-lockdown`,
  `toast-pause-lockdown`, `help-lockdown`;
- QR import: `profiles-empty`, `scan` (the camera is a placeholder on the
  host), `scan-invalid`, `scan-no-profiles`, `profiles-saved` and
  `profiles-already` (a code imported twice, from Profiles through Start).

```bash
make shots-files   # build-host/shots-files/: the Profiles screens from real files
make shots BRAND=B # build-host/shots-brand-B/: a brand-colour option
```

`make shots-files` writes a made-up profiles tree (`tools/make_fixture.py`:
loose files, a folder, a zip, a name on ux0 and uma0, an invalid file, one
with ignored lines, one that clashes with the mock's Wi-Fi) and renders the
Profiles screens from it through the same scan and parser as the console,
then imports a profile into it.


After each screenshot the tool lists text that runs off the screen, out of
the box it starts in (tiles, chips, cards, the tube) or into other text,
and characters no font could draw; it exits non-zero if it listed any.

## Languages

The app follows the system language: the 20 languages of
`SCE_SYSTEM_PARAM_LANG`, English (US) as the fallback for anything a table
leaves out. English is `src/strings_en.def`; each other language is
`src/lang/<code>.def` with the same `X(id, "text")` lines (`ja`, `fr`, `es`,
`de`, `it`, `nl`, `pt`, `ru`, `ko`, `zht`, `zhs`, `fi`, `sv`, `da`, `no`,
`pl`, `ptbr`, `engb`, `tr`). Wording follows the Vita's own Settings names
and Wi-Fi Turbo's translations; review notes per language are in
[TRANSLATIONS.md](TRANSLATIONS.md).

```bash
make check-strings              # ids, {0}/{1} placeholders, UTF-8, per language
make fallback-fonts             # once: Noto fonts for the host (about 45 MB, not shipped)
make shots SHOTS_LANG=ja        # build-host/shots-ja/
make shots-all                  # every language, problems listed per language
```

Fonts: the three bundled faces cover every Latin-script language, Turkish
and Polish included; Martian Mono also has Cyrillic. Anything a face lacks
(Cyrillic in Chakra Petch and Barlow, kana, Han, Hangul) is drawn
character by character from a fallback:

- on the console, the system fonts through ScePvf (the library behind
  vita2d's pvf text): the Latin set, which has Cyrillic, then the Japanese,
  Korean and Chinese sets in the order the system language prefers, so Han
  characters take their regional forms. Glyphs are rasterised once into
  the same coverage masks as everything else (`src/draw_vita.c`);
- on the host, Noto Sans and Noto Sans CJK (SIL OFL 1.1, see
  [../third_party/fonts/LICENSES.md](../third_party/fonts/LICENSES.md)),
  fetched by `make fallback-fonts` into `build-host/fonts/fallback/` and
  checked against pinned SHA-256 sums. They stand in for the system fonts in
  screenshots only, so line widths in CJK can differ slightly on the console.

Capitals for labels cover Latin Extended-A and Cyrillic, with the Turkish
dotted capital I in Turkish. Paragraphs in Chinese and Japanese break
between any two characters except before closing or after opening
punctuation. Long status words shrink, down to two thirds, to fit beside
the timer.



`make livearea` redraws `sce_sys/icon0.png` and the LiveArea images (8-bit
palette PNGs, which LiveArea requires) from the same drawing code.

## Console build

```bash
cd app
./build.sh                       # build/vpnturbo.vpk, mock back end
./build.sh -DVPNTURBO_BACKEND=vt     # the real back end (the kernel module's API)
./build.sh release               # build-release/vpnturbo.vpk: real back end only
./build.sh -DVPNTURBO_BRAND=B        # brand-colour option (see below)
```


A release cannot contain the mock (it ships with an empty profile list): `-DVPNTURBO_RELEASE=ON` with `VPNTURBO_BACKEND=mock` stops at configure time,
and `backend_mock.c` refuses to compile with `VPNTURBO_RELEASE` defined.

`build.sh` runs CMake in the pinned VitaSDK Docker image, as
the other builds do, mounting the repository root so the fonts are reachable,
and gives the output back to the host user. The VPK carries the fonts and
their licences in `app0:fonts/`. Compiled with `-Wall -Wextra`, no
warnings.

## Controls

Each screen's footer shows its face buttons, and the hint
is the button's only meaning on that screen. Every hint is also a 64 px
touch target.

| Screen | Cross | Triangle | Square | Circle | Other |
|---|---|---|---|---|---|
| Home, protected | Disconnect | Migrate | Profiles | Details | |
| Home, not protected / held | Retry now | Kill switch on/off (not under Lockdown) | Profiles | Disconnect | |
| Home, blocked after wake | Retry now | Pause 5 min (dimmed under Lockdown: says why) | Profiles | Disconnect | |
| Home, blocked by Lockdown | Connect (last used) | | Profiles | Details | Start: Help |
| Home, connecting | Cancel | | Profiles | Details | |
| Home, paused | Resume now | | Profiles | Disconnect | |
| Home, not connected | Connect (last used) | | Profiles | Details | |
| Home, not installed | Check again | | Profiles | About | |
| Profiles | Connect (or Migrate, if a tunnel is up); Scan QR when the list is empty | Details | Rescan | Back | Up/Down, Start: Scan QR |
| Scan QR | | | | Back | |
| Migrate | Migrate to the selection | | | Cancel | Up/Down |
| Leak check | Run again | | Network settings | Back | |
| Details | Leak check | Pause 5 min / Resume now (left out under Lockdown; pressing it says why) | Help | Back | L kill switch, R local network, Start Lockdown on/off (on asks first) |
| About | | | | Back | |

Start opens Details from any home state (on the ones where Circle is
Disconnect, it is the only way in besides touching the cause tile). Touch:
tapping a profile or migrate option selects it, tapping it again acts;
tiles on the home screen open the leak check, Details or toggle the kill
switch; the Lockdown row on Details toggles Lockdown. With the mock back
end, Select cycles the scenarios.

**Lockdown** is offered only when the module reports
`VT_FEAT_LOCKDOWN`. Turning it on asks first (what it blocks, and the three
ways back: Details, Settings › Network › VPN Settings, holding L at
boot); turning it off does not. While on: a "lockdown on" chip; disconnected
with the block in effect (`VT_HOLD_LOCKDOWN_MODE` and `VT_RT_TUNNEL`) Home
says Blocked with Connect offered; on but not yet in effect, Home says so
under "Not connected"; the kill switch reads as on; Pause is refused
("Pause is off while Lockdown is on"); Disconnect's dialog says the Vita
stays offline.

## Brand colour

Lime (`#b8f53d`) means protected and orange (`#ff8a3d`) trouble; neither is
used for branding. The brand colour is the release splash's Signal red
(`#dc1c2c`), and how far it reaches is a build-time option
(`BRAND_LEVEL` in `src/ui.h`, `-DVPNTURBO_BRAND` in CMake, `BRAND=` for
`make shots`):

| Option | Red in |
|---|---|
| none | nowhere: the header mark lights lime when protected |
| A (default) | the LiveArea images (bubble icon, startup gate, background) and the header mark |
| B | A, plus the focus highlight (Profiles and Migrate) and the About screen |
| C | B, plus a 2 px red rule under the header |

`make shots BRAND=<x>` renders each option into
`build-host/shots-brand-<x>/`. `make livearea` redraws `sce_sys/` with the
default option.

## Profiles and QR import

The real back end lists, in order, `ux0:`, `uma0:`, `imc0:` and
`ur0:data/vita-turbo-vpn/profiles/` (the folder on ux0 is
created on start): `.conf` files, sub-folders one level deep as groups, and
`.zip` files as groups of the `.conf` files inside (miniz, in memory, up to
8 MB a zip and 16 KB a profile; desktop metadata such as `__MACOSX/` and
dot-files is skipped). The same name on two storages is listed twice,
tagged with the storage. Each file is parsed with the shared parser:
ignored lines are its notes, an invalid file shows the parser's message
(English, naming the line and key; a quoted line that could hold key
material is left out), and the subnet clash is checked against the Wi-Fi
subnet from SceNetCtl. The read buffer and the parse result are wiped after
each file. Nicknames, Recent and `profiles.meta` are not read yet.

Scan QR opens the rear camera (640x480, YUV420 planar, luma to quirc's
relaxed build, CPU at 444 MHz while it runs) with the 85% framing guide.
A decoded code is parsed; if it does not parse, the reason is shown and
nothing is written. Otherwise every listed profile (zips included) is
compared with it line endings aside (`vtc_same_text`): a match says
"Already saved as X"; else it is written to the ux0 folder as
`vtc_qr_file_name`'s name from the endpoint, and the app goes back to the
list with the new profile selected and "Saved as X". The first save also
shows the plain-text key notice. Nothing is logged, and the decoded text,
quirc's copy of the frame and the camera textures are wiped.

## Back-end interface (`src/backend.h`)

| Area | Calls |
|---|---|
| Status | `bk_get_status(BkStatus*)`: state (`BK_IDLE`, `HANDSHAKING`, `UP`, `STALLED`, `ERROR`, `PAUSED`), `locked` and `lock_drops`, handshake age, time up, time in state, retry countdown, attempts, pause remaining, rx/tx bytes and packets, endpoint, MTU, tunnel and Wi-Fi addresses, kill switch, allow local network, Lockdown (offered, setting, blocking, on but not blocking yet), last error with detail |
| Installation | `bk_get_components()`: kernel module and SceShell plugin loaded and API version matches; clock plausible |
| Profiles | `bk_profile_count/get()`: name, nickname, group (sub-folder or zip), storage, path, Recent position, address, AllowedIPs, full or split, endpoint, DNS, MTU, keepalive, check (generic or the profile's own URL), warnings (subnet clash with the prefix, ignored lines, invalid with the parser's message); `bk_rescan()`, `bk_profiles_storages()`, `bk_profiles_folder()`; `bk_profile_set_nickname/rename/delete/set_check()`; `bk_import_text()` (QR import) |
| Actions | `bk_connect(i)`, `bk_disconnect()`, `bk_retry()`, `bk_migrate_kind(i)` (seamless or reconnect), `bk_migrate(i)`, `bk_pause(s)` (`BK_PAUSE_LOCKDOWN` when refused under Lockdown), `bk_resume()` |
| Options | `bk_get_options/bk_set_options(BkOptions*)`: kill switch, allow local network, Lockdown (sent only when it changes) |
| Checks | `bk_check_start()`, `bk_check_get(BkCheck*)`: state, exit IP with its city, country and network when the service gives them, last ISP address, local DNS route check per server with a verdict (ok, leak, split, not managed), remote resolvers seen with their kind |
| Lifetime | `bk_init()`, `bk_tick(ms)` once a frame, `bk_open_network_settings()` |
| Scenarios | `bk_scenario_count/get/set/name()`: the mock's; 0 elsewhere. `bk_mock_profiles_from(dir)`: the mock lists real files |

`BkState`, `BkStatus` and `BkOptions` mirror the module's status and options;
`backend_vt.c` maps the module's own types (`../include/vpnturbo.h`) onto them,
so the screens and the host build do not need that header.

### The real back end (`src/backend_vt.c`)

**Linking.** CMake makes the link stub for the `SceVpnTurbo` syscall
library from the kernel module's own `../kernel/vpnturbo_kernel.yml`
(a stand-in ELF with one empty function per exported name, as the SceShell
plugin's kstub; NIDs come from the names), so the kernel module need not be
built first. The import is weak, so the app starts without the module.

**Installation.**

| Part | How it is detected |
|---|---|
| Kernel module | found by name (`_vshKernelSearchModuleByName("vpnturbo_kernel")`) and then `vtGetStatus().api_version == VT_API_VERSION`. Nothing calls the module before that; a module missing at the first probe is never called in that run |
| SceShell plugin | no call reports it, and nothing was added to the kernel for it. Seen working: the module reports a gateway for the endpoint route, a handshake or received packets, which only the plugin's lent threads produce. Otherwise the taiHEN config read at boot (`ux0:tai/config.txt` if present, else `ur0:`) is read: `vpnturbo.suprx` listed under `*main` or `*ALL` with the file present counts as loaded, a readable config without it as missing, an unreadable one as loaded |
| Clock | the date is 2025 or later |

Without the plugin nothing carries the tunnel's traffic and nothing sets
the tunnel interface's address, so Home shows Not installed with "The quick
menu plugin is not loaded" and connecting is refused with that message.

**Connecting.** A worker thread does it, so a lookup does not hold up the
screen (Home says Connecting meanwhile): a hostname endpoint is looked up
with the system resolver first, before any key is in memory; then the
profile (a file, or an entry in a zip) is read and parsed with
`conf/`, loaded with `vtSetProfile` with the address filled in,
and connected with `vtConnect`. The text, the parse result and the
`VtProfile` are wiped as soon as the module has them. The file's path
(never its contents) goes to `ur0:data/vpnturbo/last_profile` for the
SceShell plugin's preload at boot; a zip entry cannot be preloaded,
so that file is removed instead. A failure before `vtConnect` (lookup,
unreadable or invalid file, refused profile) shows Error with the reason.
The app never sets the interface address: the plugin does, whoever
connected.

**Other actions.** Disconnect (`vtDisconnect`; also drops a connect still
in progress), Retry (a hostname is looked up again and `vtSetEndpoint` given
a new address, else `vtConnect`), Pause 5 min and Resume (`vtPause`,
`vtResume`; `-VT_ERR_LOCKDOWN` becomes `BK_PAUSE_LOCKDOWN`), kill switch,
local network and Lockdown (`vtSetOptions`; `VT_LOCKDOWN_ON/OFF` only when
the setting changes, else `VT_LOCKDOWN_KEEP`, so a change made on the
Settings page is not undone), Migrate (the same private key, compared by a 32-bit hash taken at scan time, and the
same tunnel address use `vtMigrate`; anything else `vtDisconnect`,
`vtSetProfile`, `vtConnect`).

**Status.** `vtGetStatus` four times a second, mapped to `BkStatus`.
`VT_STATE_LOCKED` is shown as Blocked. The module does not report
time up, time in state, attempts or the retry countdown, so they are
counted from the state changes the app sees (an initiation every 5 s);
when the app starts on a running tunnel they count from the start. The
Wi-Fi address and gateway come from the system (`sceNetCtl`) unless the
module reports them. The active profile is the one the app loaded; on a
fresh start it is the path in `last_profile` if the module's profile name
agrees, else the first profile with that name.

**Checks.** On a second worker thread, once each time the tunnel comes up
(and after a migrate), once at start while it is down, and when the user
asks; never on a timer. A result is dropped when the tunnel moves to
another state. Exit IP: one HTTPS GET (`https_get.c`) through
mbedTLS 3.6 from the VitaSDK, verifying the chain against the root bundle
in `../third_party/ca-bundle` (built in by `ca_bundle.c` with `.incbin`)
plus `ux0:data/vita-turbo-vpn/extra-ca.pem` if present, and the
host name (SNI). The request carries only `Host`, a generic `User-Agent`,
`Accept: application/json` and `Connection: close`; no session is kept.
The URL is the built-in service unless the profile names its own. The
answer is JSON (`ip`, and optionally `city`, `country`, `organization`)
or the bare address. A result seen with the tunnel down is stored as the
ISP's address in `ur0:data/vpnturbo/isp_ip` (the address only).
DNS: the servers come from `sceNetCtlInetGetInfo`
(the module does not override DNS yet), each matched against
`vtGetRouteFor` plus the endpoint and Wi-Fi subnet routes the module
keeps direct. The remote resolver check is not done.

Not done yet: the remote resolver check, `profiles.meta`
(nicknames, Recent), opening Network settings.

Profile parsing, the profile scan and QR import are
done (`profiles.c`); endpoint resolution is in `backend_vt.c`;
`profiles.meta` still belongs to `backend_vt.c` or files it calls.

## What is mocked

`backend_mock.c` holds five sample profiles, with neutral names
(Home server, Travel, Home LAN only with a subnet clash, Cloud VPS with an
ignored line, and Backup sharing Travel's key and address so the move is
seamless; Backup names its own check URL)
and these scenarios:

| Scenario | State |
|---|---|
| connected | up on Home server, checks clean |
| connected, Travel | up on Travel |
| connecting | handshaking, no answer yet |
| stalled, kill switch holding | stalled, kill switch on |
| not protected | stalled, kill switch off |
| leak found | up on Cloud VPS, DNS going over Wi-Fi |
| blocked after wake | resume lockdown |
| paused | paused, 4:12 left |
| idle | nothing connected |
| plugin missing | kernel module not loaded |
| shell plugin missing | kernel module loaded, SceShell plugin not loaded |
| many profiles | Recent, a sub-folder, a zip with nicknames, a name on two storages, an invalid profile |
| no profiles | nothing in the folders: the Profiles screen says where to put files |
| lockdown, disconnected | Lockdown on, not connected, the block in effect |
| connected, lockdown | up on Home server with Lockdown on |

The clock moves everything on: handshake age (rekey at 120 s), traffic,
retry and pause countdowns. Connect handshakes for 2.5 s and comes up;
scenarios that are stuck keep retrying. Disconnect, pause, resume, migrate
(seamless keeps the tunnel, reconnect handshakes again), the options and
the checks (1.5 s) all respond. Nothing touches the network or the files,
except with `bk_mock_profiles_from` (`vpnturbo-shots --profiles <dir>`), which
lists and imports into real files on the computer. A QR import in the
scripted mode adds the profile to the list in memory.

Not built yet: `profiles.meta` (nicknames, Recent), the nickname and rename
editors (which need the on-screen keyboard), and the swap of Cross and
Circle on consoles set to Circle-confirm.

## Trademarks

PlayStation and PS Vita are trademarks of Sony Interactive Entertainment.
This project is not affiliated with or endorsed by Sony.
