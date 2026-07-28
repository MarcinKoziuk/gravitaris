"""Synthesises data/sounds/gatling-1.wav -- one round of the heavy gun the
GunPower upgrade fits (the A-10 "brrt", heard one shot at a time; the sim's
fire rate does the rest).

Placeholder art, checked in as a generator rather than as a lone binary so the
sound can be re-tuned. Deterministic: same seed, same file.

    python tools/gen-gatling-wav.py
"""

import math
import random
import struct
import wave

RATE = 44100
SECONDS = 0.12
SEED = 20260727


def main():
    rng = random.Random(SEED)
    count = int(RATE * SECONDS)

    # Two one-pole lowpasses in series give the noise a duller, chunkier body
    # than a single pole -- closer to a cannon than to a hiss.
    lp1 = lp2 = 0.0
    lp_coeff = 0.35

    samples = []
    for i in range(count):
        t = i / RATE

        # Fast attack so the transient is the loudest part, then a decay long
        # enough to leave a short tail under the next round.
        attack = min(1.0, t / 0.0012)
        env = attack * math.exp(-t / 0.028)

        noise = rng.uniform(-1.0, 1.0)
        lp1 += lp_coeff * (noise - lp1)
        lp2 += lp_coeff * (lp1 - lp2)

        # Downward sweep under the noise: the body of the report.
        sweep_hz = 190.0 * math.exp(-t / 0.05) + 55.0
        body = math.sin(2.0 * math.pi * sweep_hz * t)

        value = env * (0.62 * lp2 * 3.0 + 0.55 * body * math.exp(-t / 0.02))
        samples.append(max(-1.0, min(1.0, value)))

    peak = max(abs(s) for s in samples) or 1.0
    with wave.open("data/sounds/gatling-1.wav", "w") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(b"".join(
            struct.pack("<h", int(s / peak * 0.92 * 32767)) for s in samples))


if __name__ == "__main__":
    main()
