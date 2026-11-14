# Archon

Control every audio source on your computer from your phone: per-app volume and
mute, the master volume of each output device, live level indicators — in real
time, on your local network (cable or Wi-Fi), with no cloud and no account.

*Controle todo o som do PC pelo celular: volume e mudo de cada app, volume geral
de cada saída e indicador de nível ao vivo, em tempo real, na rede local.*

| Part | Status | Tech |
| --- | --- | --- |
| Windows agent (tray) | ✅ | C++17, Win32, WASAPI, DNS-SD — 0.5 MB, ~3 MB RAM |
| Android app | ✅ | Kotlin + Jetpack Compose over the C++ core (JNI) |
| iOS app | ✅ (built in CI) | SwiftUI over the C++ core (C API) |
| Linux / macOS agents | planned | same core; PipeWire / CoreAudio backends |
| ESP32 controller | planned | same core (`ControllerSession` is sans-IO) |

## How it works

```
 phone app ──(mDNS: _archon._tcp)──▶ finds the PC
     │
     └── TCP 47321 ── pairing: 6-digit code shown on the PC (CPace PAKE)
                       then XChaCha20-Poly1305 encrypted channel
                       ◀── live state: devices, apps, volumes, levels
                       ──▶ set volume / mute
```

* **Pairing like Iriun:** the PC shows a 6-digit code, you type it on the phone.
  The code never travels on the network (CPace, X25519 + Elligator 2), a wrong
  guess costs one attempt, and 5 wrong codes lock pairing for 5 minutes. Paired
  keys are stored with DPAPI (Windows), Android Keystore and the iOS Keychain.
* **Real time both ways:** changes made on the PC (or by another phone) appear
  instantly; dragging a volume never fights the echo from the PC.
* **Light:** nothing polls; the agent sleeps until Windows reports a change and
  only samples levels while a phone has the mixer open.

Full wire contract: [`docs/protocol.md`](docs/protocol.md). Visual language:
[`.creative/direction.md`](.creative/direction.md).

## Repository layout

| Path | What |
| --- | --- |
| `core/` | portable C++17 core: protocol, crypto, controller session, agent server, C API (`archon.h`), tests |
| `agent-windows/` | Windows tray agent (WASAPI backend, pairing popup, paired devices) |
| `apps/android/` | Android app (Compose) |
| `apps/ios/` | iOS app (SwiftUI, XcodeGen project) |
| `tools/archon_cli.cpp` | command-line controller — handy for testing and as an ESP32 reference |

## Build

**Windows agent + core tests** (Visual Studio 2022 Build Tools, CMake, Ninja), from
a *x64 Native Tools* prompt:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
```
```bash
cmake --build build
```
```bash
ctest --test-dir build --output-on-failure
```

Run `build/agent-windows/Archon.exe`; it lives in the tray. Allow it on *private*
networks when Windows Firewall asks.

**Android** (JDK 17, Android SDK + NDK 27):

```bash
cd apps/android && ./gradlew assembleDebug
```

On Windows with a very deep checkout path, set `ARCHON_ANDROID_OUT` to a short
folder (e.g. `C:\aout`) to stay under the 260-character path limit.

**iOS** (macOS with Xcode 16, `brew install xcodegen`):

```bash
cd apps/ios && xcodegen generate && open Archon.xcodeproj
```

Pick your team under *Signing & Capabilities* to install on an iPhone.

**CLI controller:**

```bash
archon-cli pair 192.168.0.10 pc.key
```
```bash
archon-cli ls 192.168.0.10 pc.key
```

## Troubleshooting

* **Phone doesn't see the PC:** both must be on the same network/subnet; some
  routers isolate Wi-Fi clients ("AP isolation"). Use *Add by address* with the
  IP shown in the tray menu → *Paired devices*.
* **iPhone:** allow *Local Network* for Archon (Settings › Privacy › Local Network).
* **Logs:** tray menu → *Open log* (`%APPDATA%\Archon\agent.log`).

## License

MIT — see [LICENSE](LICENSE). Uses [Monocypher](https://monocypher.org) (BSD-2 / CC0).
