"""Generate retro vector-arcade SFX as mono 16-bit 44.1kHz WAVs.

Mono is required: OpenAL only pans/attenuates mono sources. Seeded RNG so
regeneration is reproducible.

Usage: python tools/gen-sfx.py data/sounds
"""
import math
import random
import struct
import sys
import wave

RATE = 44100
random.seed(1337)  # reproducible assets


def fade_tail(samples, ms=5.0):
    """Ramp the last `ms` to exact zero so one-shot playback never cuts off
    mid-amplitude -- an exponential envelope alone can still leave an audible
    click at the buffer boundary (e.g. a square wave's abrupt edge)."""
    n = len(samples)
    fade = min(n, int(RATE * ms / 1000.0))
    out = list(samples)
    for i in range(fade):
        out[n - fade + i] *= 1.0 - (i + 1) / fade
    return out


def write_wav(path, samples):
    peak = max(1e-9, max(abs(s) for s in samples))
    scale = 0.8 / peak  # normalize to -2 dBFS-ish
    with wave.open(path, "wb") as w:
        w.setnchannels(1)  # mono: required for OpenAL positional audio
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = b"".join(
            struct.pack("<h", int(max(-1.0, min(1.0, s * scale)) * 32767))
            for s in samples
        )
        w.writeframes(frames)
    print(f"wrote {path} ({len(samples)} samples)")


def laser():
    """Quick descending square sweep -- classic pew."""
    dur = 0.14
    n = int(RATE * dur)
    out = []
    phase = 0.0
    for i in range(n):
        t = i / n
        freq = 1400.0 * math.exp(math.log(220.0 / 1400.0) * t)
        phase += freq / RATE
        duty = 0.5 + 0.15 * math.sin(2 * math.pi * 3 * t)  # slight timbre wobble
        sq = 1.0 if (phase % 1.0) < duty else -1.0
        env = math.exp(-5.0 * t)
        out.append(sq * env)
    return out


def thrust():
    """Loopable low rumble: lowpassed brown noise, crossfaded seamless."""
    dur = 1.2
    n = int(RATE * dur)
    raw = []
    brown = 0.0
    lp = 0.0
    for _ in range(n):
        brown += random.uniform(-1.0, 1.0) * 0.02
        brown *= 0.997  # leak so it doesn't wander off
        lp += 0.08 * (brown - lp)  # ~560 Hz one-pole lowpass
        raw.append(lp)
    # Crossfade the tail into the HEAD and drop it, so the loop's last sample
    # (raw[m-1]) wraps to its first (~raw[m], the fully-blended tail start) --
    # sample-continuous at the loop point. Blending the tail in place instead
    # left last-sample ~raw[fade-1] jumping to first-sample raw[0]: an audible
    # tick every loop iteration.
    fade = int(RATE * 0.1)
    m = n - fade
    out = list(raw[:m])
    for i in range(fade):
        t = (i + 1) / fade
        out[i] = raw[i] * t + raw[m + i] * (1.0 - t)
    return out


def hit():
    """Impact: white-noise crack + low sine thud."""
    dur = 0.18
    n = int(RATE * dur)
    out = []
    lp = 0.0
    for i in range(n):
        t = i / n
        noise = random.uniform(-1.0, 1.0)
        lp += 0.35 * (noise - lp)
        crack = lp * math.exp(-18.0 * t)
        thud = 0.9 * math.sin(2 * math.pi * 90.0 * (i / RATE)) * math.exp(-9.0 * t)
        out.append(crack * 0.9 + thud)
    return out


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    write_wav(f"{outdir}/laser-1.wav", fade_tail(laser()))
    # thrust loops seamlessly (crossfaded head/tail) -- no fade-out wanted.
    write_wav(f"{outdir}/thrust-1.wav", thrust())
    write_wav(f"{outdir}/hit-1.wav", fade_tail(hit()))


if __name__ == "__main__":
    main()
