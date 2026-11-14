# Archon protocol — version 1

This document is the contract between the **agent** (the program running on the
computer whose audio is controlled) and every **controller** (phone app, future
ESP32 device, test tools). Any implementation that follows it interoperates.
The reference implementation lives in `core/` (portable C++17 + Monocypher).

All multi-byte integers are **big-endian**. Strings are UTF-8, prefixed by a
`u16` byte length, and must not exceed the per-field limits below.

## 1. Discovery

The agent advertises a DNS-SD (mDNS/Bonjour) service:

| Field | Value |
| --- | --- |
| Service type | `_archon._tcp` (domain `local.`) |
| Instance name | computer name (e.g. `DESKTOP-01`) |
| Port | TCP port the agent listens on (default **47321**) |
| TXT `v` | protocol version (`1`) |
| TXT `id` | agent id, 32 lowercase hex chars (16 bytes) |
| TXT `os` | `windows`, `linux`, `macos` |

Controllers browse for `_archon._tcp`, show the instance names, and connect to
the resolved address. mDNS works across Ethernet and Wi-Fi on the same subnet
(same router). Networks with client isolation block it; controllers must
therefore also accept a manually typed `host[:port]`.

## 2. Transport and framing

TCP, one connection per controller. Each frame:

```
u32 length   (1 .. 65536)
u8[length]   payload
```

A frame longer than 65536 bytes is a protocol error and closes the connection.

Before the secure channel is established the payload is a plain **message**.
After it, the payload is `mac[16] || ciphertext` (see §4) and the plaintext is a
message.

A message is `u8 type || body`. Unknown types received **inside** the secure
channel must be ignored (forward compatibility); unknown types during the
handshake are a protocol error.

## 3. Handshake

```
controller                                 agent
   HELLO(mode=PAIR)            ───────────▶
                                           shows 6-digit code on the PC screen
                               ◀─────────── HELLO_ACK(sid, Ys)
   user types the code
   PAIR_RESPONSE(Yc, tag_c)    ───────────▶  verify tag_c
                               ◀─────────── PAIR_CONFIRM(tag_s)
   ═══════════════ encrypted channel ══════════════

   HELLO(mode=RESUME, nonce_c) ───────────▶  look up controller id
                               ◀─────────── HELLO_ACK(nonce_s, tag_s)
   verify tag_s
   RESUME_CONFIRM(tag_c)       ───────────▶  verify tag_c
   ═══════════════ encrypted channel ══════════════
```

### Messages

| Type | Name | Direction | Body |
| --- | --- | --- | --- |
| `0x01` | HELLO | C→A | `magic "ARCN"` (bytes `41 52 43 4E`), `u16 version=1`, `u8 mode` (1 = pair, 2 = resume), `id[16]` controller id, `str name` (≤ 64 B, see *Device names*), resume only: `nonce_c[32]` |
| `0x02` | HELLO_ACK | A→C | `u16 version`, `id[16]` agent id, `str name` (≤ 64 B), `u8 mode`, pair: `sid[16] Ys[32]`; resume: `nonce_s[32] tag_s[32]` |
| `0x03` | PAIR_RESPONSE | C→A | `Yc[32] tag_c[32]` |
| `0x04` | PAIR_CONFIRM | A→C | `tag_s[32]` |
| `0x05` | RESUME_CONFIRM | C→A | `tag_c[32]` |
| `0x0F` | ERROR | A→C | `u16 code`, `str message` (≤ 256 B). Sent plain during the handshake, encrypted after it. The agent closes the connection after sending it. |

### Error codes

| Code | Meaning | Controller should |
| --- | --- | --- |
| 1 | protocol error | report a bug |
| 2 | unsupported version | ask the user to update |
| 3 | unknown controller (not paired, or pairing deleted on the PC) | see *Unauthenticated errors* — never delete the pairing automatically |
| 4 | wrong pairing code | let the user retry (a new code is shown) |
| 5 | pairing temporarily locked (too many wrong codes) | wait and retry later |
| 6 | pairing rejected by the user on the PC | stop |
| 7 | agent busy (another pairing in progress or too many controllers) | retry later |
| 8 | authentication failed | see *Unauthenticated errors* |
| 9 | timeout | retry |
| 10 | controller was revoked on the PC | forget the pairing **only if received inside the secure channel** |

### Unauthenticated errors (normative)

During the handshake ERROR frames are plaintext, so anyone on the LAN can forge
them (the agent id is public in the mDNS TXT record). Controllers therefore:

* treat plaintext 3, 8 and 10 as **not recognized**, and treat **any** other
  refusal received before the channel is up (busy, version, malformed reply…)
  as unverified too: move on to the next known address, and only after *every*
  known address refused, stop. If any address answered "not recognized", report
  that (and offer *Pair again*) rather than whichever refusal came last — a
  spoofer chooses its refusal. Never delete the stored pairing on these;
* once the channel is up, treat a framing error (zero or oversized length) or a
  frame that fails decryption as a **lost connection** and reconnect: those bytes
  are not authenticated, so they are not a verdict from the PC;
* delete a pairing automatically only on ERROR 10 received **inside** the
  encrypted channel (an authenticated revocation);
* treat a HELLO_ACK with an unexpected agent id or a `tag_s` that does not
  verify as **wrong agent**: that address is not the paired PC (stale DHCP
  lease, another PC, or a spoofer) — try the next known address and keep retrying;
* save a newly discovered address only after a resume succeeded through it.

### Device names

Names are normalized before use: invalid UTF-8 replaced with U+FFFD, NUL, C0/C1
control characters and bidi controls (U+061C, U+200E/F, U+202A–202E,
U+2066–2069) removed, then cut to at most 64 bytes on a code-point boundary.
Both sides bind the normalized names into the handshake transcripts below, so a
man-in-the-middle cannot relabel either device.

### Cryptography

Primitives (all available in Monocypher 4, libsodium-compatible output for
BLAKE2b/X25519):

* `H(k, m)` = BLAKE2b with 32-byte output, keyed with `k` (unkeyed if `k` empty).
* `X(s, P)` = X25519 scalar multiplication of point `P` by secret `s`.
* `MAP(r)` = Elligator 2 map of a 32-byte representative onto Curve25519.
* `lp(x)` = `u16 len(x) || x`.

**Pairing — CPace-style PAKE.** The 6-digit code is never sent. A wrong guess
costs the attacker one online attempt, and an eavesdropper learns nothing that
allows an offline search.

```
code      = 6 ASCII digits, uniformly random, generated by the agent
sid       = 16 random bytes chosen by the agent
G         = MAP( H("", "ARC1-CPace-generator" || lp(code) || sid || agent_id || controller_id) )
ys, yc    = 32 random bytes each (secret)
Ys = X(ys, G)            Yc = X(yc, G)
K  = X(ys, Yc) = X(yc, Ys)          — reject if K is all zeros
T  = sid || agent_id || controller_id || Ys || Yc || lp(controller_name) || lp(agent_name)
ISK      = H(K,   "ARC1-ISK" || T)
tag_c    = H(ISK, "ARC1-confirm-controller" || T)
tag_s    = H(ISK, "ARC1-confirm-agent" || T)
pair_key = H(ISK, "ARC1-pair-key" || T)          — stored by both sides
k_c2a    = H(ISK, "ARC1-key-c2a" || T)
k_a2c    = H(ISK, "ARC1-key-a2c" || T)
```

The agent verifies `tag_c` in constant time before sending `tag_s`. On failure
it sends ERROR 4 and invalidates the code. The controller verifies `tag_s`;
on failure it discards everything.

**Resume — mutual authentication with the stored `pair_key`.**

```
T     = controller_id || agent_id || nonce_c || nonce_s || lp(controller_name) || lp(agent_name)
SK    = H(pair_key, "ARC1-resume" || T)
tag_s = H(SK, "ARC1-confirm-agent")
tag_c = H(SK, "ARC1-confirm-controller")
k_c2a = H(SK, "ARC1-key-c2a")
k_a2c = H(SK, "ARC1-key-a2c")
```

Both nonces are fresh random values, so a recorded session cannot be replayed.

### Agent policy (normative)

* Only one pairing at a time; a second one gets ERROR 7.
* The code is shown on the agent's screen only after a HELLO(mode=PAIR) arrives,
  with the controller's name and a **Reject** button.
* One attempt per code. After 5 wrong codes within 10 minutes, pairing is locked
  (ERROR 5) for 5 minutes, doubling on every consecutive lockout up to 24 h;
  a successful pairing resets the escalation. The escalation state is persisted,
  so restarting the agent does not reset it. The user can lift a lock from the
  PC (tray: *Desbloquear pareamento*); that keeps the escalation count, so a
  guesser still on the network meets a longer lockout next time.
* Pairing requests are flood-limited: at most one per source address every
  20 s (so one host can't exhaust the global budget) and 8 per minute overall
  (ERROR 7 beyond that).
* A new connection must send HELLO within 5 s; the rest of the handshake has
  10 s, except the wait for PAIR_RESPONSE, which is 120 s.
* LAN only: connections from addresses outside loopback, RFC 1918, link-local,
  100.64/10 and IPv6 ULA/link-local are refused at accept time.
* Unauthenticated sockets are capped (16 overall, 4 per address). To make room
  the agent drops, in this order: sockets that already failed their handshake,
  then silent ones (no HELLO), taking them from the address holding the most
  sockets, oldest first — so a flood can't evict a phone's single in-progress
  handshake. Authenticated links and the pairing in progress are never evicted.
  A failed handshake lingers at most 1 s after its ERROR.
* After sending a final ERROR the agent half-closes (FIN) and keeps draining
  input until the peer closes or 5 s pass, so the ERROR is not lost to a reset.
* Revocation takes effect immediately for the running agent even if the store
  cannot be written.
* `pair_key` is stored encrypted at rest with an OS facility (DPAPI on Windows),
  written durably (flush + atomic rename).
* At most 8 simultaneous controllers; a controller that reconnects replaces its
  previous link and is never refused for capacity.

## 4. Secure channel

XChaCha20-Poly1305 (`crypto_aead_lock` in Monocypher), one key per direction
(`k_c2a`, `k_a2c`). The 24-byte nonce is implicit: a per-direction message
counter starting at 0, written as `u64` **little-endian** in the first 8 bytes,
the remaining 16 bytes zero. The counter never repeats; TCP preserves order, so
the receiver always knows the expected counter. Any authentication failure
closes the connection. No associated data.

Frame payload: `mac[16] || ciphertext`.

## 5. Application messages

Volumes are `u16` in **0..10000** (0.00 % – 100.00 %). Ids are opaque strings
(≤ 512 B) chosen by the agent; controllers must not parse them.

### Controller → agent

| Type | Name | Body |
| --- | --- | --- |
| `0x20` | GET_STATE | — (agent replies with a full state dump) |
| `0x21` | SET_ENDPOINT_VOLUME | `str endpoint_id`, `u16 volume` |
| `0x22` | SET_ENDPOINT_MUTE | `str endpoint_id`, `u8 muted` |
| `0x23` | SET_SOURCE_VOLUME | `str source_id`, `u16 volume` |
| `0x24` | SET_SOURCE_MUTE | `str source_id`, `u8 muted` |
| `0x25` | GET_ICON | `str icon_key` |
| `0x26` | PING | `u32 seq` |
| `0x27` | SET_METERS | `u8 on` — subscribe (1) or unsubscribe (0) to live level meters. Per connection; send it again after every reconnect |
| `0x28` | SET_SOURCE_ROUTE | `str source_id`, `str endpoint_id` — send this app's audio to an output device; empty `endpoint_id` = follow the system default again (1.2) |
| `0x29` | PANIC_MUTE | `u8 on` — 1 mutes every output and input device that is not muted yet and remembers which; 0 unmutes exactly those (1.2) |

### Agent → controller

| Type | Name | Body |
| --- | --- | --- |
| `0x40` | STATE_RESET | — controller clears its model |
| `0x41` | ENDPOINT | `str id`, `str name` (≤ 256 B), `u8 flags` (bit0 default output, bit1 muted, bit2 input device, bit3 default input), `u16 volume`, `u16 meter_slot` — insert or replace |
| `0x42` | ENDPOINT_REMOVED | `str id` |
| `0x43` | SOURCE | `str id`, `str endpoint_id`, `str name` (≤ 256 B), `str icon_key` (≤ 128 B, empty = none), `u32 pid`, `u8 flags` (bit0 muted, bit1 playing now, bit2 system sounds), `u16 volume`, `u16 meter_slot`, `str route` (≤ 512 B; the output this app is pinned to, empty = follows the default; 1.2) — insert or replace |
| `0x44` | SOURCE_REMOVED | `str id` |
| `0x45` | STATE_SYNCED | — end of a full dump |
| `0x46` | ICON | `str icon_key`, `u32 length`, PNG bytes (≤ 48 KiB) |
| `0x47` | PONG | `u32 seq` |
| `0x48` | METERS | `u8 count`, then `count` × (`u16 meter_slot`, `u8 peak`) — peak 0..255 on a perceptual (square-root) scale |
| `0x49` | AGENT_INFO | `u32 features`, `u8 panic` (1 while a panic mute is active), `str os` (≤ 32 B: `windows`, `macos`) — sent right after STATE_RESET in every dump and again whenever it changes (1.2) |

### Inputs, routing and panic mute (added in 1.2, backward compatible)

* **Microphones** are endpoints with flags bit2 set. They take the same
  SET_ENDPOINT_VOLUME / SET_ENDPOINT_MUTE commands and have meters like outputs.
  bit0 stays "default output" only; the default microphone has bit3. Agents of
  1.1 send no inputs; controllers of 1.1 ignore the unknown bits.
* **Per-app output.** SOURCE carries `route`: the output device the app is
  pinned to (or empty). SET_SOURCE_ROUTE pins or unpins it; the agent answers
  with the updated SOURCE (and, as the app's audio moves, a SOURCE whose
  `endpoint_id` is the new device). A route to an unknown or input endpoint is
  ignored. The pin is per application and survives app restarts where the OS
  allows it (Windows keeps it per executable).
* **Panic mute** is a toggle the controller can send without a mixer on screen
  (shortcut, widget, tile). The agent tracks which devices *it* muted, so
  PANIC_MUTE 0 never unmutes a device the user had muted before; a device the
  user unmutes by hand in between is simply forgotten. The state lives in the
  agent's memory (a restart ends the panic).
* **AGENT_INFO.features** tells the controller what to offer:

  | Bit | Feature |
  | --- | --- |
  | 0 | input devices (microphones) |
  | 1 | per-app output routing |
  | 2 | panic mute |

  Controllers hide what the agent does not support. A 1.1 agent never sends
  AGENT_INFO: treat that as features = 0.

### Level meters (added in 1.1, backward compatible)

`meter_slot` is a small number the agent assigns to each endpoint and source
(stable while it exists, never 0; 0 means "no meter"). Agents of 1.0 do not
send the field; receivers treat a missing field as 0. While at least one
controller is subscribed (SET_METERS 1), the agent samples peaks about 20 times
per second and sends METERS frames only to subscribed controllers. When
everything is silent it sends one all-zero frame and then stays quiet. METERS
frames are disposable: an agent may skip them for a controller that is slow to
read. Controllers should subscribe only while a mixer is on screen, and draw
their own decay (the agent sends raw peaks).

A **source** is one application on one output device (all audio sessions of a
process are grouped, like the Windows volume mixer). Sources that are open but
silent are still listed with bit1 cleared.

Right after the secure channel is established the agent sends a full dump:
`STATE_RESET`, every `ENDPOINT`, every `SOURCE`, `STATE_SYNCED`. Afterwards it
sends incremental `ENDPOINT`/`SOURCE`/`*_REMOVED` messages whenever anything
changes on the computer — including changes made by another controller or by
the user on the PC.

### Liveness

The controller sends PING every 5 s. Either side closes the connection after
15 s without receiving any frame. Controllers reconnect with exponential backoff
(0.5 s → 10 s) using RESUME.

## 6. Versioning

`version` changes only on incompatible changes. New message types and new flag
bits may be added within version 1; receivers ignore what they do not know.
