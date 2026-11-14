# Archon — visual language

**Halo v2 — "o volume é um anel".** A calm, modern black app where every volume
is a thin pastel-yellow arc: one large arc for the master, one small arc around
each app icon. Everything else steps back.

Implementations: `apps/ios/Archon/UI/Theme.swift`, `apps/android/.../ui/Theme.kt`,
`agent-windows/src/ui.cpp` (tray icon and pairing popup). Do not add colours
outside the token table.

## Invariants

- Name **Archon**. Palette: black, white, pastel yellow; green / red / yellow only as small status indicators. No other hues.
- Journey: computers on the network → pairing code → mixer; Windows tray popup shows the code.
- Mixer structure: output device, master volume + mute, one card per app (icon, name, playing state, volume, mute).
- WCAG 2.2 AA in the only theme (dark): text ≥ 4.5:1, UI boundaries and state graphics ≥ 3:1. State never by colour alone. Targets ≥ 44 pt / 48 dp. Reduce Motion honoured.
- Native only: SwiftUI and Jetpack Compose with platform SDK; no third-party UI libraries.

## Concept

> **Halo v2 — o volume é um anel.** A calm, modern black app where every volume is a thin pastel-yellow arc: one large arc for the master, one small arc around each app icon. Everything else steps back.

Axes: plain ●●●●○ (flat, no bloom) · airy ●●●○○ · instrumental motion (short, functional).

## Principles (verifiable)

1. **One ring = one volume.** The only yellow on the mixer is the volume arcs and the primary action. No yellow decoration.
2. **One status per item.** A card or row shows at most one status mark (a dot or the red mute button), never a dot + chip + label.
3. **Numbers only when they matter.** The master shows its %; app cards show % only while being dragged (it replaces the icon inside the ring).
4. **No captions inside screens.** Explanatory text exists only in empty / error states.
5. **Flat and crisp.** No glow, blur bloom, gradients or texture. Depth = surface step (#000 → #111 → #1C1C1C) only.
6. **Motion confirms, never decorates.** ≤ 240 ms UI transitions; the only loop is the radar pulse while no PC is connected.

## Typography

- iOS: **SF Pro** (system). Android: **Roboto** (system). Windows agent: **Segoe UI Variable**. No bundled font. (Inter, OFL 1.1, is used in the HTML preview only as a stand-in for SF Pro.)
- Tabular figures for all numbers (`.monospacedDigit()` / `fontFeatureSettings = "tnum"`).
- Scale (pt/sp): caption 13/400 · body 15–16/500 · title 26/600 · large title 34/600 · card % 26/600 · code orb 24/600 · ring counter 44/600 · master value 64/600 (tracking −3 %). Dynamic Type / font scale supported; the master value caps at 1.3× and the card % falls back to the icon slot.

## Colour tokens (single dark theme)

| Token | Hex | Role | Contrast (measured, WCAG 2.x) |
| --- | --- | --- | --- |
| `bg` | `#000000` | screen background | — |
| `surface` | `#111111` | cards, list rows, popup | — |
| `raised` | `#1C1C1C` | buttons, empty orbs, keys, icon tile | — |
| `track` | `#262626` | unlit part of rings, radar orbits | 1.25:1 vs surface — **decorative only**; the value is read from the lit arc + thumb |
| `edge` | `#6B6B6B` | muted-app arc, control outline | 3.54:1 vs surface · 3.94:1 vs bg |
| `paper` | `#FFFFFF` | primary text, master value, ring thumb | 21.00:1 vs bg · 18.88:1 vs surface · 17.04:1 vs raised |
| `muted` | `#9A9A9A` | secondary text, inactive names, idle icons | 7.46:1 vs bg · 6.71:1 vs surface · 6.06:1 vs raised |
| `accent` | `#F6E7A1` | volume arc, filled code orb, progress arc, primary button | 16.86:1 vs bg · 15.16:1 vs surface · 12.15:1 vs track |
| `ink` | `#000000` | text/icon on accent and on error | 16.86:1 on accent · 7.77:1 on error |
| `ok` | `#6FD39B` | connected / available / playing dot | 11.47:1 vs bg · 10.31:1 vs surface |
| `warn` | `#F2CF5B` | reconnecting / waiting for code | 13.85:1 vs bg · 12.46:1 vs surface |
| `error` | `#F07A7A` | offline, failed, muted button fill | 7.77:1 vs bg · 6.99:1 vs surface |

Ratios computed with the WCAG relative-luminance formula (Node script, sRGB hex); all text pairs ≥ 4.5:1, all state graphics ≥ 3:1.

## Shape, depth, icons

- Radii: cards 24 · list rows 20 · keys 8 · popup 8 (Windows 11) · buttons and orbs fully round.
- Depth: surface steps only; no shadows on phone (the Windows popup keeps the OS shadow).
- Strokes: master arc 8 pt · app arc 4 pt (6 pt while dragging) · code ring 3 pt · radar orbits 1 pt. Arcs span 270°, start at 135°, round caps; the master and the dragged card show a white thumb (r 9 / 7).
- Icons: SF Symbols / Material Symbols, outline, 2 pt: `speaker.wave.1`, `speaker.slash`, `chevron.*`, `iphone`. App icons are the real PNGs sent by the agent, shown in a 48 pt `raised` rounded tile; fallback = monogram.

## Components

### Radar (Computers)
- Two concentric 1 pt `track` orbits (r 70 / 130) around a thin phone glyph. Paired PCs sit on the inner orbit, new and offline PCs on the outer one. Each point = 8 pt status dot + name (13/500); offline name in `muted`.
- Pulse: a single 1 pt `accent` ring expands from the centre and fades (2.4 s loop) — the screen's only loop. It runs the whole time the app is open with no PC connected, and stops while a PC is connected or with Reduce Motion.
- Below: one `surface` row per PC (dot + name; `›` or a yellow **Parear** button). "Adicionar por endereço" as a plain accent text button. IPs and "last seen" go to the row's detail, not the row.
- Empty state: radar alone + "Procurando computadores na rede…" + same-network hint (the only caption allowed here).

### Code ring (Pairing)
- Title "Código do PC" + PC name. A 310 pt ring (3 pt `track`) with six 52 pt orbs at 60° steps from the top.
- Typed digit: orb fills `accent` with `ink` digit (scale 0.85 → 1, 180 ms); progress arc advances 1/6 (240 ms ease-out). Current orb: 1.5 pt accent outline + caret. Centre: "4/6".
- System numeric keyboard (`.numberPad` / `KeyboardType.NumberPad`), one-time-code paste accepted.
- States: verifying (progress arc rotates slowly), wrong code (orbs turn `error` for 600 ms, empty, text "Código errado — veja o novo código no PC"), temporarily blocked (centre shows countdown), refused on PC, timed out. Each state has text; colour is never alone.

### Master arc (Mixer)
- Top bar: back · device name + `▾` (opens device sheet; default device first) · connection dot (`ok`/`warn`/`error`, with VoiceOver label).
- 220 pt arc, value "64" + small `%`. Drag along the arc or vertically on it. One round mute button below; muted = `error` fill + slashed speaker, arc turns `edge`.

### App card (Mixer)
- 2-column grid, card 172 pt, `surface`, radius 24. Ring 96 pt around the app icon; name below (ellipsis); mute button top-right (36 pt visual, 44 pt hit).
- Playing: one 8 pt `ok` dot before the name. Its opacity follows the live per-source peak level (protocol LEVELS, ~20 Hz, 0..255) smoothed to 55–100 %; nothing else uses level data. Silent: no dot. Muted: arc `edge`, icon at 45 %, name `muted`, red mute button — and no playing dot.
- Gesture: drag along the ring or horizontally on the card; vertical drag scrolls the grid. While dragging, the stroke grows 4 → 6 pt, the thumb appears and the % replaces the icon; on release it returns after 600 ms. Remote changes animate the arc 240 ms; no echo jump while the finger is down.
- Haptics: light tick every 10 %, stronger at 0 and 100 %.

### Windows tray popup (C++ agent)
- 340 px `surface` card, radius 8, Segoe UI Variable: "Archon" + 18 px countdown ring (accent arc); one line "Digite este código no celular para conectar."; code `482 913` at 36 px/600 in `paper`; "Recusar" button. Tray icon: thin accent ring (turns `ok` when a phone is connected). Plain Direct2D/GDI drawing; no effects.

### Desktop window (Windows, macOS, Linux agents)

The same mixer on the PC, native per OS (Win32 + Direct2D, SwiftUI, GTK 4); the
same tokens, rings and cards as the phone, laid out for a mouse and a keyboard.
It sees exactly what a paired phone sees (through the agent's local view).

- **Window**: 960 × 680 by default, at least 400 × 560, resizable; `bg`; the
  system title bar in dark mode. Opened by clicking the tray icon (Windows), the
  menu bar item (macOS) or the app launcher (Linux); closing hides it, the agent
  keeps running.
- **Top bar** (56 px): output pill (device name ▾, opens the output list) ·
  phones line (`ok` dot + "2 celulares conectados", or `muted` "Nenhum celular
  conectado") · right: **Perfis**, **Silenciar tudo** (panic, `error` fill when
  on), **Configurações** — 40 px round `raised` buttons, 44 px hit target.
- **Body**, width ≥ 760: a 320 px left column with the master arc (220 px), its
  mute button, the "Ouvindo no celular" line while a phone listens and the
  profile in force; the right side scrolls the card grid (mic + apps, then the
  "Abas do navegador" header and the tab cards). Below 760: one column, master
  arc 180 px on top.
- **Cards** as on the phone (172 px wide, gap 12, columns fill the width).
  Right-click an app card for its output menu.
- **Mouse**: drag along a ring or horizontally on a card; the wheel over a ring
  or card changes ±5 % per notch (±1 % with Shift). The hover state brightens a
  card from `surface` to `raised`; nothing else moves.
- **Keyboard**: Tab / Shift+Tab move between controls in reading order; on a
  volume control ←/→ or ↑/↓ change ±5 % (±1 % with Shift), Home/End go to
  0/100 %, Space or M toggles mute, Shift+F10 or the menu key opens a card's
  output menu; Esc closes a panel. Focus = 2 px `accent` outline following the
  control's shape, 3 px outside it.
- **Panels** (Perfis, Configurações) slide in from the right as a 380 px
  `surface` sheet over the grid (full width below 760), 220 ms; Esc or the ×
  closes. Perfis: the same list and editor as the phone. Configurações:
  celulares pareados (remover), iniciar com o Windows, extensão do navegador
  (installed / not detected + how to install), atalho do "Silenciar tudo",
  rede do PC, versão.
- **Accessibility**: every control is exposed to the screen reader (UI
  Automation / NSAccessibility / ATK) with name, role, value (volume %) and
  state (muted, selected); reduce motion follows the system setting.

## Motion

| Element | Motion | Duration / easing | Reduce Motion |
| --- | --- | --- | --- |
| Screen push | system navigation transition | platform default | platform default |
| List rows / cards appear | fade + 8 pt rise, 40 ms stagger, once | 220 ms `cubic-bezier(0.2,0,0,1)` | fade only |
| Code orb fill | scale 0.85 → 1 | 180 ms same curve | instant |
| Progress / volume arc (remote change) | stroke end animates | 240 ms same curve | instant |
| Arc while dragging | follows finger, no easing | — | — |
| Radar pulse (app open, no PC connected) | 1 ring expands + fades | 2.4 s loop | off; static ring |
| Playing dot | opacity follows level | 50 ms smoothing | static 100 % |
| Pairing success | ring closes, cross-fade to mixer | 300 ms | cross-fade |

Never animated: text content, layout reflow, colour of text.


## Native buildability and performance

- SwiftUI: `Circle().trim(from:to:)` + `.stroke(style: StrokeStyle(lineCap: .round))`, rotated 135°; Compose: `Canvas { drawArc(...) }`. Orbs and rows are plain shapes. No blur, no shadows, no gradients, no shader.
- Level data: subscribe only while the mixer is visible and the app is foreground; drop to 0 Hz in background / Reduce Motion / Low Power Mode. Only the dot's opacity redraws (one small layer per playing card) — negligible CPU/GPU.
- The radar pulse is one layer animation and stops once a PC is connected.
