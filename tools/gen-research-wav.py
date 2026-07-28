"""Synthesises data/sounds/research-1.wav -- the chime a Lab plays when its
faction's research bar fills. Placeholder art, checked in as a generator so
it can be re-tuned; deterministic.
"""
import math
import struct
import wave

RATE = 44100
SECONDS = 0.9


def main():
    count = int(RATE * SECONDS)
    samples = []
    for i in range(count):
        t = i / RATE
        # Two rising notes, the second entering partway: reads as "done" rather
        # than as an alarm.
        v = 0.0
        for start, hz in ((0.0, 660.0), (0.13, 990.0)):
            if t < start:
                continue
            u = t - start
            env = min(1.0, u / 0.006) * math.exp(-u / 0.28)
            v += env * (math.sin(2.0 * math.pi * hz * u)
                        + 0.28 * math.sin(4.0 * math.pi * hz * u))
        samples.append(v)

    peak = max(abs(s) for s in samples) or 1.0
    with wave.open("data/sounds/research-1.wav", "w") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(b"".join(
            struct.pack("<h", int(s / peak * 0.85 * 32767)) for s in samples))


if __name__ == "__main__":
    main()
