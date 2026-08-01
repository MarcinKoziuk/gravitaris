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


def bubble_shield():
    """Bubble field absorbing a round, in the Star Trek deflector idiom: a
    downward-swept resonant WHOOMP with a bright edge on the attack, smeared by
    a short feedback delay so it rings in a space rather than in the open.

    The recognisable parts are the falling pitch (the field flexing and
    recovering, not a struck object), the heavy low fundamental, and the tail
    that blooms slightly after the transient instead of decaying straight from
    it. Long by this file's standards -- the impression is of energy dispersing.
    """
    dur = 0.62
    n = int(RATE * dur)
    dry = []
    lp = 0.0
    phase = 0.0
    for i in range(n):
        t = i / n
        s = i / RATE

        # Falling sweep, fast at first then settling: 300 Hz down to ~95.
        freq = 95.0 + 205.0 * math.exp(-7.0 * t)
        # Integrate the sweep rather than using freq*s directly, or the phase
        # (and so the audible pitch) is wrong the moment freq stops being
        # constant.
        phase += freq / RATE

        fundamental = math.sin(2 * math.pi * phase)
        # An octave and a twelfth up, both fading faster than the fundamental:
        # the bright edge of the impact, gone while the body still rings.
        edge = (0.45 * math.sin(4 * math.pi * phase) * math.exp(-9.0 * t)
                + 0.22 * math.sin(6 * math.pi * phase) * math.exp(-14.0 * t))
        # Band-limited hiss riding the attack -- the crackle of the field.
        lp += 0.20 * (random.uniform(-1.0, 1.0) - lp)
        crackle = lp * 0.35 * math.exp(-16.0 * t)

        dry.append((fundamental + edge + crackle) * math.exp(-4.2 * t))

    # Two short feedback taps: a cheap stand-in for the reverberant space these
    # always sit in. Without it the sweep reads as a synth tone rather than as
    # something happening around a hull.
    out = list(dry)
    for delay_ms, gain in ((37.0, 0.42), (73.0, 0.22)):
        d = int(RATE * delay_ms / 1000.0)
        for i in range(d, n):
            out[i] += dry[i - d] * gain
    return out


def field_ring():
    """Bright detuned ring that shimmers and settles: energy holding, with no
    transient of its own. Layered under the plate clank below -- 'FIELD PLATING'
    is a field as much as it is armour, and the pair reads as both at once."""
    dur = 0.26
    n = int(RATE * dur)
    out = []
    lp = 0.0
    for i in range(n):
        t = i / n
        s = i / RATE
        # Slight upward bend on the attack -- the field taking the load.
        bend = 1.0 + 0.18 * math.exp(-22.0 * t)
        a = math.sin(2 * math.pi * 520.0 * bend * s)
        b = math.sin(2 * math.pi * 784.0 * bend * s)  # a fifth up, detuned by the bend
        shimmer = 0.75 + 0.25 * math.sin(2 * math.pi * 38.0 * s)
        # Airy wash under the attack, not a crack: heavily lowpassed and gone fast.
        lp += 0.06 * (random.uniform(-1.0, 1.0) - lp)
        out.append(((a * 0.6 + b * 0.4) * shimmer + lp * 0.5 * math.exp(-30.0 * t))
                   * math.exp(-7.0 * t))
    return out


def plate_clank():
    """A struck metal plate. Inharmonic partials (the mode ratios of a bar, not
    a string) over a noise transient, with the upper modes dying first -- that
    ordering is what makes it read as metal."""
    dur = 0.22
    n = int(RATE * dur)
    modes = [(1.00, 1.00, 11.0), (2.76, 0.55, 16.0), (5.40, 0.30, 24.0), (8.93, 0.16, 34.0)]
    f0 = 380.0
    out = []
    lp = 0.0
    for i in range(n):
        t = i / n
        s = i / RATE
        body = sum(amp * math.sin(2 * math.pi * f0 * ratio * s) * math.exp(-decay * t)
                   for ratio, amp, decay in modes)
        lp += 0.45 * (random.uniform(-1.0, 1.0) - lp)
        clank = lp * math.exp(-40.0 * t)
        out.append(body * 0.5 + clank * 0.7)
    return out


def plating_hit():
    """Ablative plate taking a round: the metal clank with the field ring under
    it. Mixed into one clip rather than played as two voices, so a plate hit
    costs the one-shot pool exactly what every other event does."""
    clank = plate_clank()
    ring = field_ring()
    n = max(len(clank), len(ring))
    out = [0.0] * n
    for i, v in enumerate(clank):
        out[i] += v
    # Under, not over: the clank is what says which shield stopped the round.
    for i, v in enumerate(ring):
        out[i] += v * 0.55
    return out


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    write_wav(f"{outdir}/laser-1.wav", fade_tail(laser()))
    # thrust loops seamlessly (crossfaded head/tail) -- no fade-out wanted.
    write_wav(f"{outdir}/thrust-1.wav", thrust())
    write_wav(f"{outdir}/hit-1.wav", fade_tail(hit()))
    write_wav(f"{outdir}/shield-bubble-1.wav", fade_tail(bubble_shield()))
    write_wav(f"{outdir}/shield-plating-1.wav", fade_tail(plating_hit()))


if __name__ == "__main__":
    main()
