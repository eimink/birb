#!/usr/bin/env python3
"""Generate simple test WAV files for birb sample testing."""
import struct
import math
import os

SR = 44100

def write_wav(path, samples):
    samples = [max(-32768, min(32767, int(s))) for s in samples]
    data = struct.pack('<' + 'h' * len(samples), *samples)
    with open(path, 'wb') as f:
        f.write(b'RIFF')
        f.write(struct.pack('<I', 36 + len(data)))
        f.write(b'WAVEfmt ')
        f.write(struct.pack('<I', 16))       # fmt chunk size
        f.write(struct.pack('<H', 1))        # PCM
        f.write(struct.pack('<H', 1))        # mono
        f.write(struct.pack('<I', SR))
        f.write(struct.pack('<I', SR * 2))
        f.write(struct.pack('<H', 2))        # block align
        f.write(struct.pack('<H', 16))       # bits
        f.write(b'data')
        f.write(struct.pack('<I', len(data)))
        f.write(data)
    print(f'Wrote {path} ({len(samples)} samples, {len(samples)/SR:.3f}s)')

def kick():
    """Analog-style kick: sine wave with fast pitch drop and quick decay."""
    n = int(SR * 0.25)
    out = []
    for i in range(n):
        t = i / SR
        # Pitch sweeps from ~150Hz down to ~40Hz
        freq = 150 * math.exp(-t * 25) + 40
        phase = 2 * math.pi * freq * t
        # Amplitude envelope (fast attack, exponential decay)
        env = math.exp(-t * 12)
        out.append(math.sin(phase) * env * 28000)
    return out

def snare():
    """Snare: noise + short tone burst, quick decay."""
    import random
    random.seed(42)
    n = int(SR * 0.15)
    out = []
    for i in range(n):
        t = i / SR
        env = math.exp(-t * 20)
        # Noise component
        noise = (random.random() * 2 - 1) * 18000
        # Tonal component (~180Hz short tone)
        tone = math.sin(2 * math.pi * 180 * t) * 10000 * math.exp(-t * 40)
        out.append((noise + tone) * env)
    return out

def hat():
    """Closed hat: filtered noise, very short."""
    import random
    random.seed(7)
    n = int(SR * 0.05)
    out = []
    prev = 0
    for i in range(n):
        t = i / SR
        env = math.exp(-t * 80)
        # High-passed noise (difference with previous)
        cur = random.random() * 2 - 1
        hp = cur - prev * 0.5
        prev = cur
        out.append(hp * 20000 * env)
    return out

if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    write_wav(os.path.join(here, 'kick.wav'), kick())
    write_wav(os.path.join(here, 'snare.wav'), snare())
    write_wav(os.path.join(here, 'hat.wav'), hat())
