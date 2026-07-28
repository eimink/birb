#!/usr/bin/env python3
"""
midi2birb.py — Convert MIDI files to birb song format (.bsb)

Usage: python3 midi2birb.py input.mid [-o output.bsb] [--channels 1,2,10,3] [--rows 32]

Maps up to 4 MIDI channels to birb channels.
Quantizes note events to tracker rows.
Maps velocity to volume column.
"""
import struct
import sys
import os
import argparse

# ---- MIDI parser (no external deps) ----

def read_varlen(data, pos):
    val = 0
    while True:
        b = data[pos]; pos += 1
        val = (val << 7) | (b & 0x7F)
        if not (b & 0x80): break
    return val, pos

def parse_midi(data):
    """Parse a Standard MIDI File, return (header, tracks)"""
    if data[:4] != b'MThd':
        raise ValueError('Not a MIDI file')
    hdr_len = struct.unpack('>I', data[4:8])[0]
    fmt, ntrk, division = struct.unpack('>HHH', data[8:14])
    pos = 8 + hdr_len

    tracks = []
    for _ in range(ntrk):
        if data[pos:pos+4] != b'MTrk':
            raise ValueError('Expected MTrk')
        trk_len = struct.unpack('>I', data[pos+4:pos+8])[0]
        trk_data = data[pos+8:pos+8+trk_len]
        tracks.append(parse_track(trk_data))
        pos += 8 + trk_len

    return {'format': fmt, 'division': division}, tracks

def parse_track(data):
    """Parse a single MIDI track, return list of (delta_ticks, event)"""
    events = []
    pos = 0
    running_status = 0
    while pos < len(data):
        delta, pos = read_varlen(data, pos)
        b = data[pos]
        if b == 0xFF:  # meta event
            pos += 1
            meta_type = data[pos]; pos += 1
            length, pos = read_varlen(data, pos)
            meta_data = data[pos:pos+length]; pos += length
            events.append((delta, {'type': 'meta', 'meta_type': meta_type, 'data': meta_data}))
        elif b == 0xF0 or b == 0xF7:  # sysex
            pos += 1
            length, pos = read_varlen(data, pos)
            pos += length
        else:
            if b & 0x80:
                status = b; pos += 1
                running_status = status
            else:
                status = running_status
            cmd = status & 0xF0
            ch = status & 0x0F
            if cmd in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                d1 = data[pos]; pos += 1
                d2 = data[pos]; pos += 1
                if cmd == 0x90 and d2 == 0:
                    cmd = 0x80  # note on with vel 0 = note off
                events.append((delta, {'type': 'midi', 'cmd': cmd, 'ch': ch, 'data1': d1, 'data2': d2}))
            elif cmd in (0xC0, 0xD0):
                d1 = data[pos]; pos += 1
                events.append((delta, {'type': 'midi', 'cmd': cmd, 'ch': ch, 'data1': d1, 'data2': 0}))
    return events

# ---- Quantize MIDI to tracker rows ----

def midi_to_rows(header, tracks, channel_map, rows_per_pattern, tpr):
    """Convert MIDI events to birb pattern data.
    channel_map: list of MIDI channel numbers (0-15) to map to birb ch 0-3.
    Returns: (bpm, patterns, order)
    """
    division = header['division']
    bpm = 120  # default

    # Merge all tracks into a single event list with absolute tick times
    all_events = []
    for track in tracks:
        abs_tick = 0
        for delta, event in track:
            abs_tick += delta
            all_events.append((abs_tick, event))
            # Extract tempo
            if event['type'] == 'meta' and event['meta_type'] == 0x51:
                us_per_beat = struct.unpack('>I', b'\x00' + event['data'][:3])[0]
                bpm = round(60_000_000 / us_per_beat)
    all_events.sort(key=lambda x: x[0])

    # Calculate ticks per row
    # In MIDI: division = ticks per quarter note
    # In tracker: tpr ticks per row, rows map to some musical division
    # Assume 4 rows per beat (16th notes at standard speed)
    rows_per_beat = 4
    midi_ticks_per_row = division / rows_per_beat

    # Build channel map lookup
    ch_lookup = {}
    for birb_ch, midi_ch in enumerate(channel_map[:4]):
        ch_lookup[midi_ch] = birb_ch

    # Collect note events per row per birb channel
    # Each entry: {note, velocity, is_off}
    row_events = {}  # (row_idx, birb_ch) -> (note, velocity)
    note_offs = {}   # (row_idx, birb_ch) -> True

    max_row = 0
    for abs_tick, event in all_events:
        if event['type'] != 'midi': continue
        if event['ch'] not in ch_lookup: continue
        birb_ch = ch_lookup[event['ch']]
        row = int(abs_tick / midi_ticks_per_row + 0.5)
        if row > max_row: max_row = row

        if event['cmd'] == 0x90:  # note on
            midi_note = event['data1']
            velocity = event['data2']
            # MIDI note 0 = C-1, birb note 2 = C0
            # MIDI 24 = C1, MIDI 36 = C2, MIDI 60 = C4
            birb_note = midi_note - 12  # shift: MIDI 12=C0 -> birb semitone 0
            if 0 <= birb_note < 96:
                row_events[(row, birb_ch)] = (birb_note + 2, velocity)
        elif event['cmd'] == 0x80:  # note off
            if (row, birb_ch) not in row_events:
                note_offs[(row, birb_ch)] = True

    # Split into patterns
    total_rows = max_row + 1
    num_patterns = (total_rows + rows_per_pattern - 1) // rows_per_pattern

    patterns = []
    for pat_idx in range(num_patterns):
        pat = []
        for r in range(rows_per_pattern):
            row = []
            abs_row = pat_idx * rows_per_pattern + r
            for c in range(4):
                note = 0
                inst = 255
                vol = 0
                fx = 0
                prm = 0
                if (abs_row, c) in row_events:
                    n, vel = row_events[(abs_row, c)]
                    note = n
                    inst = min(c, 15)  # default: instrument = channel
                    vol = max(1, vel * 2)  # scale MIDI velocity (0-127) to birb vol (1-255)
                    if vol > 255: vol = 255
                elif (abs_row, c) in note_offs:
                    note = 1  # note off
                row.append({'note': note, 'inst': inst, 'vol': vol, 'fx': fx, 'prm': prm})
            pat.append(row)
        patterns.append(pat)

    order = [[i, i, i, i] for i in range(num_patterns)]

    return bpm, patterns, order

# ---- Write .bsb ----

def write_bsb(filename, bpm, tpr, instruments, patterns, order):
    buf = bytearray()
    # header
    buf += b'BRB1'
    buf.append(min(bpm, 255))
    buf.append(tpr)
    buf.append(len(instruments))
    buf.append(len(patterns))
    # order
    buf.append(len(order))
    for o in order:
        for c in range(4):
            buf.append(o[c] if c < len(o) else 0)
    # instruments (12 bytes each)
    for inst in instruments:
        buf.append(inst.get('wave', 0))
        buf.append(inst.get('duty', 2))
        buf.append(inst.get('a', 0))
        buf.append(inst.get('d', 8))
        buf.append(inst.get('s', 180))
        buf.append(inst.get('r', 8))
        pe = inst.get('pe', 0)
        buf.append(pe & 0xFF)
        buf.append(inst.get('pel', 0))
        buf.append(inst.get('arp1', 0))
        buf.append(inst.get('arp2', 0))
        buf.append(inst.get('vol', 255))
        buf.append(0)  # reserved
    # patterns (planar)
    for pat in patterns:
        nrows = len(pat)
        buf.append(nrows)
        # notes
        for c in range(4):
            for r in range(nrows):
                buf.append(pat[r][c]['note'])
        # instruments
        for c in range(4):
            for r in range(nrows):
                buf.append(pat[r][c]['inst'])
        # volume
        for c in range(4):
            for r in range(nrows):
                buf.append(pat[r][c]['vol'])
        # effects
        for c in range(4):
            for r in range(nrows):
                buf.append(pat[r][c]['fx'])
        # params
        for c in range(4):
            for r in range(nrows):
                buf.append(pat[r][c]['prm'])

    with open(filename, 'wb') as f:
        f.write(buf)
    print(f'Wrote {filename} ({len(buf)} bytes, {len(patterns)} patterns, {len(order)} order positions)')

# ---- Main ----

BIRB_VERSION = '3.0.0'

def main():
    parser = argparse.ArgumentParser(description='Convert MIDI to birb .bsb')
    parser.add_argument('--version', action='version', version=f'midi2birb (birb) {BIRB_VERSION}')
    parser.add_argument('input', help='Input MIDI file')
    parser.add_argument('-o', '--output', help='Output .bsb file')
    parser.add_argument('--channels', default='0,1,2,9',
                        help='MIDI channels to map to birb ch 1-4 (comma-separated, 0-indexed, default: 0,1,2,9)')
    parser.add_argument('--rows', type=int, default=32,
                        help='Rows per pattern (default: 32)')
    parser.add_argument('--tpr', type=int, default=6,
                        help='Ticks per row (default: 6)')
    args = parser.parse_args()

    output = args.output or os.path.splitext(args.input)[0] + '.bsb'
    channel_map = [int(x.strip()) for x in args.channels.split(',')]

    with open(args.input, 'rb') as f:
        data = f.read()

    header, tracks = parse_midi(data)
    print(f'MIDI: format {header["format"]}, {len(tracks)} tracks, division {header["division"]}')

    bpm, patterns, order = midi_to_rows(header, tracks, channel_map, args.rows, args.tpr)
    print(f'Converted: {bpm} BPM, {len(patterns)} patterns of {args.rows} rows')

    # Default instruments — one per birb channel
    instruments = [
        {'wave': 0, 'duty': 2, 'a': 1, 'd': 6, 's': 160, 'r': 8, 'vol': 160},   # ch1: lead
        {'wave': 1, 'duty': 0, 'a': 0, 'd': 4, 's': 200, 'r': 4, 'vol': 200},    # ch2: bass
        {'wave': 2, 'duty': 0, 'a': 8, 'd': 4, 's': 140, 'r': 12, 'vol': 120},   # ch3: pad
        {'wave': 3, 'duty': 0, 'a': 0, 'd': 5, 's': 0, 'r': 0, 'vol': 180},      # ch4: drums
    ]

    write_bsb(output, bpm, args.tpr, instruments, patterns, order)
    print(f'Load into birb tracker or convert with: ./birbc {output} --js')

if __name__ == '__main__':
    main()
