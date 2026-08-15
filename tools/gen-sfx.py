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


def match_rms(samples, target):
    """Scale to a given RMS rather than to a peak.

    For anything being compared by ear this is the honest normalisation: a tone
    and a hiss at the same PEAK differ by several dB of loudness (their crest
    factors are nothing alike -- measured 2.1 against 5.0 across the beam
    voices), and in an A/B the louder one simply wins. Peak is still guarded,
    since a clipped comparison is no comparison either.
    """
    rms = max(1e-9, math.sqrt(sum(s * s for s in samples) / len(samples)))
    scaled = [s * (target / rms) for s in samples]
    peak = max(abs(s) for s in scaled)
    if peak > 0.95:
        scaled = [s * (0.95 / peak) for s in scaled]
    return scaled


def write_wav(path, samples, normalize=True):
    peak = max(1e-9, max(abs(s) for s in samples))
    # Already levelled by match_rms, and re-peaking would undo exactly that.
    scale = 1.0 if not normalize else 0.8 / peak  # normalize to -2 dBFS-ish
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


def thrust_boost():
    """The overburn's loop: thrust(), but with the injectors open.

    Same brown-noise bed so the two read as one engine rather than two, then
    the parts that say "past the redline" -- a brighter lowpass (more of the
    hiss survives), a hard-clipped body, and a pair of detuned sawtooth
    harmonics beating slowly against each other for the whine. Crossfaded
    seamless exactly as thrust() is; see its comment for why the tail is
    blended into the head rather than in place.

    Draws from its own generator rather than the module-level one: sharing it
    would shift the stream for every clip written after this one, silently
    re-rolling assets this function has nothing to do with.
    """
    rng = random.Random(20260802)
    dur = 1.2
    n = int(RATE * dur)
    raw = []
    brown = 0.0
    lp = 0.0
    phase_a = 0.0
    phase_b = 0.0
    # Both whine partials complete a whole number of cycles over the loop, so
    # the crossfade has nothing to smooth over at the seam.
    freq_a = round(146.0 * dur) / dur
    freq_b = round(219.0 * dur) / dur
    for _ in range(n):
        brown += rng.uniform(-1.0, 1.0) * 0.02
        brown *= 0.997
        lp += 0.22 * (brown - lp)  # brighter than thrust()'s 0.08: more edge

        phase_a = (phase_a + freq_a / RATE) % 1.0
        phase_b = (phase_b + freq_b / RATE) % 1.0
        whine = (phase_a - 0.5) + (phase_b - 0.5)  # two saws, slowly beating

        body = lp * 3.0
        body = max(-0.6, min(0.6, body))  # clipped: the engine is straining
        raw.append(body + whine * 0.18)

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


# The beam's voice, in the two numbers that turn out to describe it: the pair of
# resonances the hiss is built around. Both clips share them so the charge opens
# straight out into the held note instead of handing over to another sound.
#
# These are not a guess. A ship-phaser reference was measured (spectral centroid,
# per-band energy, tonality, envelope) and it is nothing like a tone: energy runs
# from about 600 Hz to 6 kHz with essentially nothing outside that, the strongest
# resonances sit near 1.4 kHz and 2.7 kHz, and the forty strongest bins hold 3%
# of the total -- noise, not partials. Its charge starts as a narrow whistle
# around 3.2 kHz that swells and spreads out into that band, which is exactly
# what beam_windup() does. Everything below is synthesised to those figures; see
# the docstrings for what each part is answering for.
# A voice is: a low band (the body -- the dull part), an upper band (the edge),
# an optional `air` band far above both (the sharp part), a shared lowpass over
# the sum, and an optional slow tremolo. `r` is each band's tightness and `mix`
# its amplitude; `charge` (the windup's narrow whistle) is derived from `upper`
# unless a profile names its own.
#
# `measured` is the fitted one and what data/upgrades.toml names. Its weights
# were SOLVED, not chosen: the bands are independent noise sources, so their band
# powers add, and one least-squares fit against the reference's thirds
# (600-1500, 1500-3000, 3000-6000 Hz, with a hundredth outside 600 Hz-6 kHz)
# lands it. Its shared lowpass is steep on purpose -- a two-pole resonator sheds
# only 6 dB an octave, so a bank of them leaks a tenth of its energy up where the
# reference has almost none, and one filter over the sum fixes every skirt.
#
# The rest are alternatives to pick from in the Audio debug tab, and they move in
# one direction: sharper and higher than `measured`, most of them keeping a
# quieter low band underneath for the dullness. `sharpness` in the report main()
# prints is the share of energy above 3 kHz -- the number that tracks what the
# ear calls sharp, so a pick can be described rather than only heard.
BEAM_PROFILES = {
    "measured": dict(low=(1400.0, 0.94, 1.00), upper=(4300.0, 0.88, 0.74),
                     post=(0.575, 3)),

    # --- kept as anchors -----------------------------------------------------
    # Both survived auditions: `octave-up` won its round, `octave-higher-sharp`
    # the next. Everything after them is a different mechanism rather than
    # another set of numbers, so these two are what the new ones are judged
    # against.
    "octave-up": dict(low=(2400.0, 0.94, 0.80), upper=(7000.0, 0.90, 1.00),
                      post=(0.85, 2)),
    "octave-higher-sharp": dict(low=(3400.0, 0.94, 0.70), upper=(10500.0, 0.96, 1.15),
                                post=(0.92, 1)),

    # --- other ways to build a beam ------------------------------------------
    # Filtered noise is one idea, and the six above were all that idea. These
    # are six others, each a synthesis technique the film-and-television ray gun
    # actually leans on -- and none of them a buzz: no sawtooths, no arcing, no
    # amplitude pulsing anywhere (`tremolo` is gone from this file for that
    # reason). What they have in common is only the brightness the auditions
    # settled on.
    #
    # `charge` is explicit on each: the windup's whistle cannot be derived from
    # an `upper` band that these do not necessarily have.

    # A comb filter is a delay fed back into itself: it reinforces one pitch and
    # its harmonics out of plain noise, which is the metallic ring of a beam
    # heard down a pipe. The oldest trick in this list and still the most
    # "laser" -- the Death Star's superlaser is a comb on a rumble.
    "comb-metal": dict(kind="comb", freq=2900.0, feedback=0.82,
                       air=(8200.0, 0.90, 0.55), post=(0.90, 1), charge=3400.0),

    # Ring modulation: multiply the noise by a sine instead of filtering it, and
    # every frequency present splits into a pair either side of the carrier. The
    # result has no fundamental at all, which is why it reads as machinery that
    # was never alive -- the Dalek/ray-gun sound, used here on a hiss rather
    # than on a voice.
    "ring-mod": dict(kind="ring", carrier=4200.0, band=(3000.0, 0.86),
                     post=(0.92, 1), charge=3800.0),

    # Frequency modulation at a deliberately non-integer ratio: the sidebands
    # land between the harmonics, so it is bright and pitched but inharmonic --
    # a tone that cannot be sung. Half the ray guns of the 1980s are this. A
    # little noise underneath keeps it from sounding like a synthesiser patch.
    "fm-ray": dict(kind="fm", carrier=3800.0, ratio=1.55, index=3.2, noise=0.35,
                   post=(0.92, 1), charge=4600.0),

    # Three tight resonances at inharmonic spacings -- formants, the way a vowel
    # is three peaks rather than a pitch. Gives the beam a fixed "vowel" it
    # holds: closer to a phaser singing than to gas escaping, without a tremolo
    # doing the work.
    "formant-beam": dict(kind="formant", formants=((3100.0, 0.97, 1.00),
                                                   (4700.0, 0.96, 0.85),
                                                   (7300.0, 0.94, 0.70)),
                         post=(0.92, 1), charge=4200.0),

    # A plucked string, plucked continuously: noise into a delay line whose
    # feedback path is lowpassed (Karplus-Strong). Each pass loses its top, so
    # what comes out is a tuned metallic sizzle that keeps re-exciting itself --
    # a beam under tension rather than one being sprayed.
    "string-sizzle": dict(kind="string", freq=1850.0, feedback=0.96, damping=0.45,
                          post=(0.92, 1), charge=3600.0),

    # Six detuned bands stacked high with no single one dominant: no pitch to
    # latch onto, just a dense wall well above where the ear looks for one. The
    # "phaser array" reading -- many emitters, one sound.
    "array-stack": dict(kind="stack", bands=((6800.0, 0.93, 1.00), (7300.0, 0.94, 0.90),
                                             (8000.0, 0.93, 0.85), (8800.0, 0.94, 0.80),
                                             (9700.0, 0.93, 0.75), (10600.0, 0.94, 0.70)),
                        post=(0.94, 1), charge=5200.0),
}

# The windup's whistle sits here relative to the profile's upper band: measured
# at 3.2 kHz against a 4.3 kHz upper on the reference, and derived rather than
# authored so a brighter voice charges brighter too.
BEAM_CHARGE_RATIO = 0.74

# Every held note is levelled to this RMS instead of to a common peak, so the
# voices can be compared by character rather than by loudness (see match_rms).
BEAM_TARGET_RMS = 0.20

# The beam's voice, chosen by audition out of the profiles above: `comb-metal`
# beat eight others across three rounds. This is the one written as
# laser-loop-1.wav / laser-windup-1.wav -- the files data/upgrades.toml names --
# so changing this line changes the weapon's sound. The losers are kept in the
# table because they cost nothing there and document what was tried; pass
# `--variants` to write them out again for another comparison.
BEAM_VOICE = "comb-metal"


def resonator(freq, r):
    """A two-pole band, as a step function over one noise sample.

    `r` is how tight it is: near 1 rings at `freq` like a whistle, lower is a
    wide band. Bandwidth is roughly (1 - r) * RATE / pi, which is what makes
    the measured band shares reachable by choosing numbers rather than by ear.

    Zeros at DC and Nyquist (the `x - x[n-2]` term) are not decoration. Without
    them the two poles alone pass DC at a gain comparable to the resonance
    itself, and a stack of these tuned across the spectrum piles up a bass
    rumble that no amount of highpassing the bed will remove -- measured as a
    sixth of the total energy below 200 Hz on a band centred at 1.4 kHz.
    """
    coeff = 2.0 * r * math.cos(2.0 * math.pi * freq / RATE)
    decay = r * r
    gain = (1.0 - decay) / 2.0
    past_in = [0.0, 0.0]
    past_out = [0.0, 0.0]

    def step(x):
        y = gain * (x - past_in[1]) + coeff * past_out[0] - decay * past_out[1]
        past_in[1] = past_in[0]
        past_in[0] = x
        past_out[1] = past_out[0]
        past_out[0] = y
        return y

    return step


def lowpass_chain(coeff, poles):
    """`poles` one-pole lowpasses in series, as a step function."""
    state = [0.0] * poles

    def step(x):
        for i in range(poles):
            state[i] += coeff * (x - state[i])
            x = state[i]
        return x

    return step


def delay_line(freq, length=8192):
    """A fractionally-interpolated delay of one period of `freq`, as read/write
    closures. The interpolation matters: at these pitches a period is only a
    dozen-odd samples, so rounding the delay to a whole one detunes the comb
    audibly (and differently per profile, which would make two voices differ for
    a reason nobody chose)."""
    period = RATE / freq
    buf = [0.0] * length
    cursor = [0]

    def read():
        at = cursor[0] - period
        low = math.floor(at)
        frac = at - low
        return buf[int(low) % length] * (1.0 - frac) + buf[(int(low) + 1) % length] * frac

    def write(value):
        buf[cursor[0] % length] = value
        cursor[0] += 1

    return read, write


def voice_comb(profile):
    """Noise through a feedback comb: one pitch and its harmonics pulled out of
    a hiss, which is a beam ringing down a tube."""
    read, write = delay_line(profile["freq"])
    feedback = profile["feedback"]
    air = resonator(*profile["air"][:2]) if "air" in profile else None
    air_mix = profile["air"][2] if "air" in profile else 0.0
    post = lowpass_chain(*profile["post"])

    def step(white):
        value = white + read() * feedback
        write(value)
        out = value * (1.0 - feedback)
        if air:
            out += air(white) * air_mix
        return post(out)

    return step


def voice_ring(profile):
    """Banded noise times a sine: every frequency splits into a pair either side
    of the carrier, leaving no fundamental at all."""
    band = resonator(*profile["band"])
    carrier = profile["carrier"]
    post = lowpass_chain(*profile["post"])
    clock = [0]

    def step(white):
        t = clock[0] / RATE
        clock[0] += 1
        return post(band(white) * math.sin(2 * math.pi * carrier * t) * 4.0)

    return step


def voice_fm(profile):
    """Frequency modulation at a non-integer ratio: bright, pitched, and
    inharmonic -- a tone that cannot be sung."""
    carrier = profile["carrier"]
    modulator = carrier * profile["ratio"]
    index = profile["index"]
    noise_mix = profile.get("noise", 0.0)
    grit = resonator(carrier * 1.7, 0.90)
    post = lowpass_chain(*profile["post"])
    phase = [0.0, 0.0]

    def step(white):
        phase[1] += modulator / RATE
        phase[0] += carrier / RATE
        tone = math.sin(2 * math.pi * (phase[0] + index * math.sin(2 * math.pi * phase[1])))
        return post(tone * 0.5 + grit(white) * noise_mix)

    return step


def voice_formant(profile):
    """Three tight resonances at inharmonic spacings: a vowel rather than a
    pitch, held."""
    bands = [(resonator(freq, r), mix) for freq, r, mix in profile["formants"]]
    post = lowpass_chain(*profile["post"])
    return lambda white: post(sum(band(white) * mix for band, mix in bands))


def voice_string(profile):
    """Karplus-Strong, excited continuously instead of once: a delay whose
    feedback path loses its top on every pass, so the noise settles into a tuned
    metallic sizzle that keeps re-exciting itself."""
    read, write = delay_line(profile["freq"])
    feedback = profile["feedback"]
    damping = profile["damping"]
    post = lowpass_chain(*profile["post"])
    state = [0.0]

    def step(white):
        delayed = read()
        state[0] += damping * (delayed - state[0])  # the string losing its edge
        value = white * 0.5 + state[0] * feedback
        write(value)
        return post(value * (1.0 - feedback) * 4.0)

    return step


def voice_stack(profile):
    """Many detuned bands high up, none dominant: dense, with no pitch to latch
    onto."""
    bands = [(resonator(freq, r), mix) for freq, r, mix in profile["bands"]]
    post = lowpass_chain(*profile["post"])
    return lambda white: post(sum(band(white) * mix for band, mix in bands))


def voice_bands(profile):
    """The original idea: a low band for the body, an upper for the edge, and an
    optional one above both."""
    bands = [(resonator(freq, r), mix)
             for freq, r, mix in (profile["low"], profile["upper"])
             + ((profile["air"],) if "air" in profile else ())]
    post = lowpass_chain(*profile["post"])
    return lambda white: post(sum(band(white) * mix for band, mix in bands))


VOICE_KINDS = {
    "bands": voice_bands,
    "comb": voice_comb,
    "ring": voice_ring,
    "fm": voice_fm,
    "formant": voice_formant,
    "string": voice_string,
    "stack": voice_stack,
}


def beam_bands(profile):
    """One step function over one noise sample, whichever mechanism the profile
    names. Shared by the held note and the charge so a profile cannot drift
    between the two halves of its own sound."""
    return VOICE_KINDS[profile.get("kind", "bands")](profile)


def beam(profile):
    """The laser's held note: pshhhhh. Escaping pressure, not a tone.

    Bands of noise and a steep lowpass over the sum, and that is the whole
    sound -- no tonal core at all. One was tried and it is what made an earlier
    attempt read as a hum; the reference measures as noise (the forty strongest
    bins hold 3% of its energy), so anything that sings is wrong.

    Crossfaded head-to-tail as thrust() is: with no partials to keep in phase,
    the seam only needs the amplitudes to meet, which the crossfade does.

    Two seconds rather than one. Nothing in the sound repeats, but the CLIP does,
    and a held beam plays it over and over -- with noise this dense the ear
    starts hearing the same texture come round again as a rhythm, which is a
    repetition nobody authored. Doubling the period roughly halves how often
    that lands, at a few hundred KB per voice.

    No tremolo, deliberately, and no knob for one: anything that pulses reads as
    exactly the repetition above.
    """
    dur = 2.0
    n = int(RATE * dur)

    voice = beam_bands(profile)

    raw = []
    for _ in range(n):
        raw.append(voice(random.uniform(-1.0, 1.0)))

    fade = int(RATE * 0.08)
    m = n - fade
    out = list(raw[:m])
    for i in range(fade):
        t = (i + 1) / fade
        out[i] = raw[i] * t + raw[m + i] * (1.0 - t)
    return out


def beam_windup(profile):
    """The emitter charging: the *p* of pshhhh, a fifth of a second of it.

    Authored to the windup in data/upgrades.toml (0.2 s): the audio system holds
    this as a voice for exactly that long, so a clip longer than the charge gets
    cut off mid-sweep and a shorter one repeats.

    Ends on the band beam() holds, so the charge does not so much finish as open
    out into the held hiss.

    Its shape is the reference's own onset, measured: nearly all of that energy
    starts inside one narrow band around 3.2 kHz -- tight enough to measure as
    half tonal, a whistle rather than a hiss -- and spreads out into the wide
    band as it comes up in level. So this is one high-Q resonance loosening
    while beam()'s pair rises underneath it.

    Draws from its own generator rather than the module-level one: sharing it
    would shift the stream for every clip written after this one.
    """
    dur = 0.2
    n = int(RATE * dur)
    rng = random.Random(20260813)

    charge = None
    # Not dict.get with a default: the fallback dereferences `upper`, which the
    # mechanisms that are not band stacks do not have, and a default argument is
    # evaluated whether it is needed or not.
    charge_freq = (profile["charge"] if "charge" in profile
                   else profile["upper"][0] * BEAM_CHARGE_RATIO)
    voice = beam_bands(profile)
    post = lowpass_chain(*profile["post"])

    out = []
    for i in range(n):
        t = i / n

        # Re-struck rather than smoothly swept: a two-pole resonator's tightness
        # lives in its coefficients, and stepping them a few times over a fifth
        # of a second is inaudible under noise this dense.
        if i % 512 == 0:
            charge = resonator(charge_freq, 0.988 - 0.048 * t)

        white = rng.uniform(-1.0, 1.0)
        # The held band comes up under the whistle over the second half.
        opening = max(0.0, t * 2.0 - 1.0)

        env = t ** 1.4  # in from nothing, hardest at the end
        out.append((post(charge(white) * 2.6) + voice(white) * opening) * env)
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    outdir = args[0] if args else "."
    write_wav(f"{outdir}/laser-1.wav", fade_tail(laser()))
    # thrust loops seamlessly (crossfaded head/tail) -- no fade-out wanted.
    write_wav(f"{outdir}/thrust-1.wav", thrust())
    write_wav(f"{outdir}/thrust-boost-1.wav", thrust_boost())
    write_wav(f"{outdir}/hit-1.wav", fade_tail(hit()))
    write_wav(f"{outdir}/shield-bubble-1.wav", fade_tail(bubble_shield()))
    write_wav(f"{outdir}/shield-plating-1.wav", fade_tail(plating_hit()))
    # Written last on purpose: the RNG is seeded once for the whole run, so
    # appending here leaves every file above byte-identical to its last build.
    write_wav(f"{outdir}/laser-loop-1.wav",
              match_rms(beam(BEAM_PROFILES[BEAM_VOICE]), BEAM_TARGET_RMS), normalize=False)
    # The windup draws from its own generator (see beam_windup), so this one is
    # last only for tidiness -- but keep it here anyway: anything inserted
    # ABOVE re-rolls every clip that shares the module-level stream.
    write_wav(f"{outdir}/laser-windup-1.wav",
              fade_tail(beam_windup(BEAM_PROFILES[BEAM_VOICE])))

    # The voices that lost, on demand only: `--variants` writes one pair per
    # profile so another audition can be run without editing anything. They are
    # not game assets -- nothing in data/ names them -- so a normal run leaves
    # them out rather than shipping eight unused clips.
    if "--variants" in sys.argv:
        for name, profile in BEAM_PROFILES.items():
            if name == BEAM_VOICE:
                continue
            write_wav(f"{outdir}/laser-loop-{name}.wav",
                      match_rms(beam(profile), BEAM_TARGET_RMS), normalize=False)
            write_wav(f"{outdir}/laser-windup-{name}.wav", fade_tail(beam_windup(profile)))


if __name__ == "__main__":
    main()
