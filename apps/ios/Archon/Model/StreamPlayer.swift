import AVFoundation

/// Plays the PC's audio on the phone ("Ouvir no celular"): Opus packets (48 kHz,
/// stereo, 20 ms) from the core are decoded with the system's Opus decoder and
/// scheduled on an AVAudioPlayerNode. Packets arrive on the core's network
/// thread; everything audio-related runs on one serial queue.
final class StreamPlayer {
    /// Called on the main thread when playback must stop (headphones unplugged).
    var onStopRequest: (() -> Void)?

    private let queue = DispatchQueue(label: "dev.archon.stream", qos: .userInteractive)
    private let engine = AVAudioEngine()
    private let node = AVAudioPlayerNode()
    private let pcm = AVAudioFormat(standardFormatWithSampleRate: 48000, channels: 2)!
    private let opus: AVAudioFormat
    private var converter: AVAudioConverter?
    private var queued = 0      // buffers scheduled and not played yet
    private var playing = false
    private var running = false
    private var observers: [NSObjectProtocol] = []

    static let startFrames = 3  // 60 ms buffered before playback starts
    static let maxFrames = 10   // > 200 ms behind: drop packets to catch up

    init() {
        var asbd = AudioStreamBasicDescription(mSampleRate: 48000, mFormatID: kAudioFormatOpus, mFormatFlags: 0,
                                               mBytesPerPacket: 0, mFramesPerPacket: 960, mBytesPerFrame: 0,
                                               mChannelsPerFrame: 2, mBitsPerChannel: 0, mReserved: 0)
        opus = AVAudioFormat(streamDescription: &asbd)!
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: pcm)
    }

    /// Main thread. Activates the audio session (plays with the screen locked).
    func start() throws {
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.playback, mode: .default, options: [])
        try? session.setPreferredIOBufferDuration(0.01)
        try session.setActive(true)
        try engine.start()
        queue.sync {
            converter = AVAudioConverter(from: opus, to: pcm)
            queued = 0
            playing = false
            running = true
        }
        let nc = NotificationCenter.default
        observers.append(nc.addObserver(forName: AVAudioSession.interruptionNotification, object: session, queue: .main) {
            [weak self] note in self?.interrupted(note)
        })
        observers.append(nc.addObserver(forName: AVAudioSession.routeChangeNotification, object: session, queue: .main) {
            [weak self] note in self?.routeChanged(note)
        })
    }

    /// Main thread.
    func stop() {
        observers.forEach(NotificationCenter.default.removeObserver)
        observers.removeAll()
        queue.sync {
            running = false
            converter = nil
        }
        node.stop()
        engine.stop()
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    /// Core network thread: one packet in order, or a lost frame.
    func feed(packet: Data?, lost: Bool) {
        queue.async { [self] in
            guard running else { return }
            if queued > Self.maxFrames { return }  // too far behind: catch up
            guard let out = AVAudioPCMBuffer(pcmFormat: pcm, frameCapacity: 960) else { return }
            if lost || !(packet.map { decode($0, into: out) } ?? false) {
                silence(out)  // a gap keeps its 20 ms so timing does not drift
            }
            queued += 1
            node.scheduleBuffer(out) { [weak self] in
                guard let self else { return }
                self.queue.async { self.queued = max(0, self.queued - 1) }
            }
            if !playing && queued >= Self.startFrames {
                node.play()
                playing = true
            }
        }
    }

    private func silence(_ b: AVAudioPCMBuffer) {
        b.frameLength = 960
        if let ch = b.floatChannelData {
            for c in 0..<Int(b.format.channelCount) { ch[c].update(repeating: 0, count: 960) }
        }
    }

    private func decode(_ p: Data, into out: AVAudioPCMBuffer) -> Bool {
        guard let conv = converter, !p.isEmpty, p.count <= 1275 else { return false }
        let cb = AVAudioCompressedBuffer(format: opus, packetCapacity: 1, maximumPacketSize: 1275)
        p.withUnsafeBytes { raw in
            if let base = raw.baseAddress { cb.data.copyMemory(from: base, byteCount: p.count) }
        }
        cb.byteLength = UInt32(p.count)
        cb.packetCount = 1
        cb.packetDescriptions?.pointee = AudioStreamPacketDescription(mStartOffset: 0, mVariableFramesInPacket: 0,
                                                                      mDataByteSize: UInt32(p.count))
        var given = false
        var error: NSError?
        let status = conv.convert(to: out, error: &error) { _, inStatus in
            if given {
                inStatus.pointee = .noDataNow
                return nil
            }
            given = true
            inStatus.pointee = .haveData
            return cb
        }
        return status != .error && out.frameLength > 0
    }

    // A call or alarm took the audio: resume when it ends.
    private func interrupted(_ note: Notification) {
        guard let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
              AVAudioSession.InterruptionType(rawValue: raw) == .ended else { return }
        try? AVAudioSession.sharedInstance().setActive(true)
        try? engine.start()
        queue.async { [self] in
            if playing { node.play() }
        }
    }

    // Headphones unplugged: stop instead of suddenly playing on the speaker.
    private func routeChanged(_ note: Notification) {
        guard let raw = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt,
              AVAudioSession.RouteChangeReason(rawValue: raw) == .oldDeviceUnavailable else { return }
        onStopRequest?()
    }
}
