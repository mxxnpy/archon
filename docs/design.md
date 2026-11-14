# Archon — visual language

The approved direction is **Halo v2 — "o volume é um anel"**: a calm black app
where every volume is a thin pastel-yellow arc (one large arc for the master,
one small ring around each app icon), cards for apps, simple short motion.

Source of truth (tokens, components, motion, contrast numbers):

* [`.creative/direction.md`](../.creative/direction.md) — tokens and components
* [`.creative/storyboards/motion.md`](../.creative/storyboards/motion.md) — motion
* [`.creative/brief.md`](../.creative/brief.md) — the owner's requirements and feedback history

Implementations: `apps/android/.../ui/Theme.kt`, `apps/ios/Archon/UI/Theme.swift`,
`agent-windows/src/ui.cpp` (tray icon and pairing popup). Do not add colours
outside the token table.
