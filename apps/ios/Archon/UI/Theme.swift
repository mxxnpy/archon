import SwiftUI

/// Halo v2 tokens — .creative/direction.md is the source of truth.
enum Archon {
    static let bg = Color(hex: 0x000000)
    static let surface = Color(hex: 0x111111)
    static let raised = Color(hex: 0x1C1C1C)
    static let track = Color(hex: 0x262626)
    static let edge = Color(hex: 0x6B6B6B)
    static let paper = Color(hex: 0xFFFFFF)
    static let muted = Color(hex: 0x9A9A9A)
    static let accent = Color(hex: 0xF6E7A1)
    static let ink = Color(hex: 0x000000)
    static let ok = Color(hex: 0x6FD39B)
    static let warn = Color(hex: 0xF2CF5B)
    static let error = Color(hex: 0xF07A7A)
}

/// Motion tokens (.creative/storyboards/motion.md).
enum Motion {
    static let ui = Animation.timingCurve(0.2, 0, 0, 1, duration: 0.22)
    static let arc = Animation.timingCurve(0.2, 0, 0, 1, duration: 0.24)
    static let pop = Animation.timingCurve(0.2, 0, 0, 1, duration: 0.18)
    static let stagger = 0.035
    static let staggerCap = 0.24
    static let echoHold = 0.4
}

extension Font {
    // Text styles scale with Dynamic Type; ring numerals use @ScaledMetric capped at 1.3×.
    static let archonCaption = Font.footnote
    static let archonBody = Font.body.weight(.medium)
    static let archonTitle = Font.title.weight(.semibold)
    static let archonLargeTitle = Font.largeTitle.weight(.semibold)
    static let archonCardPct = Font.system(size: 26, weight: .semibold).monospacedDigit()
    static let archonOrb = Font.system(size: 24, weight: .semibold).monospacedDigit()
    static let archonCounter = Font.system(size: 44, weight: .semibold).monospacedDigit()
    static let archonMaster = Font.system(size: 64, weight: .semibold).monospacedDigit()
}

extension Color {
    init(hex: UInt32) {
        self.init(.sRGB, red: Double((hex >> 16) & 0xFF) / 255, green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255, opacity: 1)
    }
}

/// 270° ring starting at 135°: track + lit arc + optional thumb.
struct VolumeArc: View {
    var fraction: Double
    var lineWidth: CGFloat
    var color: Color = Archon.accent
    var thumbRadius: CGFloat = 0

    var body: some View {
        GeometryReader { g in
            let inset = max(lineWidth / 2, thumbRadius)
            let d = min(g.size.width, g.size.height) - inset * 2
            let f = min(max(fraction, 0), 1)
            ZStack {
                Circle().trim(from: 0, to: 0.75)
                    .stroke(Archon.track, style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
                Circle().trim(from: 0, to: 0.75 * f)
                    .stroke(color, style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
                    .opacity(f > 0.002 ? 1 : 0)
                if thumbRadius > 0 {
                    // The whole ring is rotated 135°, so the thumb angle is relative.
                    let a = 270 * f * .pi / 180
                    Circle().fill(Archon.paper).frame(width: thumbRadius * 2, height: thumbRadius * 2)
                        .offset(x: d / 2 * cos(a), y: d / 2 * sin(a))
                }
            }
            .rotationEffect(.degrees(135), anchor: .center)
            .frame(width: d, height: d)
            .position(x: g.size.width / 2, y: g.size.height / 2)
        }
    }
}

struct Dot: View {
    var color: Color
    var opacity: Double = 1
    var body: some View { Circle().fill(color.opacity(opacity)).frame(width: 8, height: 8) }
}

struct StatusLine: View {
    var color: Color
    var text: LocalizedStringKey
    var body: some View {
        HStack(spacing: 8) {
            Dot(color: color)
            Text(text).font(.archonCaption).foregroundStyle(Archon.muted)
        }
    }
}

/// Round mute button: 36 pt visual, 44 pt hit area.
struct MuteButton: View {
    var label: String
    var muted: Bool
    var visual: CGFloat = 36
    var toggle: () -> Void
    var body: some View {
        Button(action: toggle) {
            ZStack {
                Circle().fill(muted ? Archon.error : Archon.raised).frame(width: visual, height: visual)
                Image(systemName: muted ? "speaker.slash" : "speaker.wave.1")
                    .font(.system(size: visual * 0.4, weight: .medium))
                    .foregroundStyle(muted ? Archon.ink : Archon.paper)
            }
            .frame(width: 44, height: 44)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(muted ? Text("Desmutar \(label)") : Text("Mutar \(label)"))
    }
}

/// Appear once: fade + 8 pt rise, staggered.
struct Appear: ViewModifier {
    let index: Int
    @Environment(\.accessibilityReduceMotion) private var reduce
    @State private var shown = false
    func body(content: Content) -> some View {
        content
            .opacity(shown ? 1 : 0)
            .offset(y: shown || reduce ? 0 : 8)
            .onAppear {
                withAnimation(reduce ? nil : Motion.ui.delay(min(Double(index) * Motion.stagger, Motion.staggerCap))) {
                    shown = true
                }
            }
    }
}

extension View {
    func appear(_ index: Int) -> some View { modifier(Appear(index: index)) }
}
