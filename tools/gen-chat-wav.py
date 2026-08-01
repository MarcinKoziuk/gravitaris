"""Synthesises data/sounds/chat-1.wav -- the blip a received chat line plays.
Placeholder art, checked in as a generator so it can be re-tuned;
deterministic.
"""
import math
import struct
import wave

RATE = 44100
SECONDS = 0.16


def main():
    count = int(RATE * SECONDS)
    samples = []
    for i in range(count):
        t = i / RATE
        # One short square-ish blip an octave below the research chime, so a
        # message reads as a terminal beep rather than as anything the sim did.
        env = min(1.0, t / 0.004) * math.exp(-t / 0.045)
        v = math.sin(2.0 * math.pi * 520.0 * t) + 0.35 * math.sin(2.0 * math.pi * 1040.0 * t)
        samples.append(env * v)

    peak = max(abs(s) for s in samples) or 1.0
    with wave.open("data/sounds/chat-1.wav", "w") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(b"".join(
            struct.pack("<h", int(s / peak * 0.8 * 32767)) for s in samples))


if __name__ == "__main__":
    main()
