#!/usr/bin/env python3
"""
Strip the reverb send bus out of the hand-golfed 4K JS players.

The checked-in web/birb_tiny.js and web/birb4k.js are the *with-reverb*
source of truth. Every reverb-only line is fenced in line-level markers:

    //<REV>
    ...reverb-only code...
    //</REV>

For a handful of lines that must differ inline (the master output), the block
carries a lean alternative behind //<REV-else>:

    //<REV>
    out[i]=Math.tanh(v)        <- kept in the with-reverb source, dropped when stripping
    //<REV-else>
    //out[i]=v>1?1:v<-1?-1:v    <- commented in source, uncommented in the lean build
    //</REV>

This script removes the //<REV>..//</REV> blocks (dropping the "then" body and
uncommenting any //<REV-else> body) to emit lean no-reverb variants:
web/birb_tiny.norev.js and web/birb4k.norev.js. It prints the byte size of
each with-reverb source and its lean output.

Marker lines must be *exactly* //<REV>, //<REV-else>, //</REV> after trimming.
"""
import os
import re

OPEN, ELSE, CLOSE = '//<REV>', '//<REV-else>', '//</REV>'


def strip_reverb(src):
    lines = src.split('\n')
    out = []
    i, n = 0, len(lines)
    while i < n:
        if lines[i].strip() == OPEN:
            i += 1
            in_else = False
            else_body = []
            while i < n and lines[i].strip() != CLOSE:
                cur = lines[i].strip()
                if cur == ELSE:
                    in_else = True
                elif in_else:
                    else_body.append(lines[i])
                # non-else lines are the reverb "then" body -> dropped
                i += 1
            i += 1  # skip the //</REV> line
            # keep the lean alternative, uncommented (preserve indentation)
            for el in else_body:
                m = re.match(r'^(\s*)//(.*)$', el)
                out.append(m.group(1) + m.group(2) if m else el)
        else:
            out.append(lines[i])
            i += 1
    return '\n'.join(out)


def main():
    base = os.path.dirname(os.path.abspath(__file__))
    targets = [
        ('web/birb_tiny.js', 'web/birb_tiny.norev.js'),
        ('web/birb4k.js', 'web/birb4k.norev.js'),
    ]
    for src_rel, out_rel in targets:
        src_path = os.path.join(base, src_rel)
        out_path = os.path.join(base, out_rel)
        with open(src_path) as f:
            src = f.read()
        lean = strip_reverb(src)
        with open(out_path, 'w') as f:
            f.write(lean)
        print(f'{src_rel}: {len(src.encode())} bytes (with reverb)')
        print(f'{out_rel}: {len(lean.encode())} bytes (lean)')


if __name__ == '__main__':
    main()
