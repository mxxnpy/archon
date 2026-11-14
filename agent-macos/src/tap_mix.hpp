#pragma once
// Pure DSP used by the per-app volume taps (no Apple headers, so it is unit
// tested on every platform). Runs on the real-time audio thread: no
// allocation, no locks.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace agent::mix {

// One channel inside a (possibly interleaved) float buffer.
struct Chan {
    float* data = nullptr;
    size_t stride = 1;  // in floats: number of interleaved channels
};
struct ConstChan {
    const float* data = nullptr;
    size_t stride = 1;
};

// Protocol level (0..10000) <-> slider scalar (0..1).
inline uint16_t to_level(float scalar) {
    const float v = std::clamp(scalar, 0.0f, 1.0f);
    return uint16_t(std::lround(v * 10000.0f));
}
inline float to_scalar(uint16_t level) { return float(std::min<uint16_t>(level, 10000)) / 10000.0f; }

// Slider position to amplitude. Squared so the slider feels like a volume
// knob (50% is about -12 dB), not a linear fader where most travel is loud.
inline float gain_for(uint16_t level, bool muted) {
    if (muted) return 0.0f;
    const float s = to_scalar(level);
    return s * s;
}

// Peak (0..1) to the 0..255 meter byte; same perceptual curve as Windows.
inline uint8_t meter_byte(float peak) {
    return uint8_t(std::lround(std::sqrt(std::clamp(peak, 0.0f, 1.0f)) * 255.0f));
}

// Copies `in` to `out` applying a gain that ramps linearly from g0 to g1 over
// the block (no zipper noise when the slider moves). Channel mapping: mono
// input feeds the first two outputs; otherwise input k feeds output k; any
// other output channel is silenced. Returns the output peak (post-gain).
inline float render(const ConstChan* in, size_t n_in, const Chan* out, size_t n_out, size_t frames, float g0,
                    float g1) {
    float peak = 0.0f;
    const float step = frames > 1 ? (g1 - g0) / float(frames - 1) : 0.0f;
    for (size_t k = 0; k < n_out; ++k) {
        const ConstChan* src = nullptr;
        if (n_in == 1 && k < 2) src = &in[0];
        else if (k < n_in) src = &in[k];
        float* dst = out[k].data;
        if (!dst) continue;
        if (!src || !src->data) {
            for (size_t f = 0; f < frames; ++f) dst[f * out[k].stride] = 0.0f;
            continue;
        }
        for (size_t f = 0; f < frames; ++f) {
            const float v = src->data[f * src->stride] * (g0 + step * float(f));
            dst[f * out[k].stride] = v;
            peak = std::max(peak, std::fabs(v));
        }
    }
    return std::min(peak, 1.0f);
}

// Peak of the input only (meter-only taps that leave the app's sound alone).
inline float peak_of(const ConstChan* in, size_t n_in, size_t frames, float gain = 1.0f) {
    float peak = 0.0f;
    for (size_t k = 0; k < n_in; ++k) {
        if (!in[k].data) continue;
        for (size_t f = 0; f < frames; ++f) peak = std::max(peak, std::fabs(in[k].data[f * in[k].stride]));
    }
    return std::min(peak * gain, 1.0f);
}

// One-pole smoothing toward the target gain, applied once per block so a
// burst of slider updates never jumps.
inline float approach(float current, float target, float max_step = 0.25f) {
    const float d = target - current;
    if (std::fabs(d) <= max_step) return target;
    return current + (d > 0 ? max_step : -max_step);
}

}  // namespace agent::mix
