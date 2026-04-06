#!/usr/bin/env python3
"""
Minify editor.html using terser for JS, simple rules for CSS/HTML.
Produces editor.min.html
"""
import re
import subprocess
import sys
import tempfile
import os

def minify_css(css):
    css = re.sub(r'/\*.*?\*/', '', css, flags=re.DOTALL)
    css = re.sub(r'\s+', ' ', css)
    css = re.sub(r'\s*([{}:;,>+~])\s*', r'\1', css)
    css = re.sub(r';\s*}', '}', css)
    return css.strip()

def minify_js_terser(js):
    with tempfile.NamedTemporaryFile(mode='w', suffix='.js', delete=False) as f:
        f.write(js)
        tmp_in = f.name
    tmp_out = tmp_in + '.min'
    try:
        result = subprocess.run(
            ['terser', tmp_in, '-o', tmp_out,
             '--compress', 'passes=3',
             '--mangle'],
            capture_output=True, text=True
        )
        if result.returncode != 0:
            print(f'terser warning: {result.stderr}', file=sys.stderr)
        if os.path.exists(tmp_out):
            with open(tmp_out) as f:
                return f.read()
        # Fallback: return original
        return js
    finally:
        os.unlink(tmp_in)
        if os.path.exists(tmp_out):
            os.unlink(tmp_out)

def main():
    infile = sys.argv[1] if len(sys.argv) > 1 else 'web/editor.html'
    outfile = sys.argv[2] if len(sys.argv) > 2 else 'web/editor.min.html'

    with open(infile) as f:
        html = f.read()

    # Extract and minify CSS
    css_match = re.search(r'<style>(.*?)</style>', html, re.DOTALL)
    if css_match:
        css_min = minify_css(css_match.group(1))
        html = html[:css_match.start(1)] + css_min + html[css_match.end(1):]

    # Extract and minify JS with terser
    js_match = re.search(r'<script>(.*?)</script>', html, re.DOTALL)
    if js_match:
        js = js_match.group(1)
        js_min = minify_js_terser(js)
        html = html[:js_match.start(1)] + '\n' + js_min + '\n' + html[js_match.end(1):]

    # Minify HTML: remove comments, collapse whitespace between tags
    html = re.sub(r'<!--.*?-->', '', html, flags=re.DOTALL)
    html = re.sub(r'>\s+<', '><', html)

    with open(outfile, 'w') as f:
        f.write(html)

    orig_size = len(open(infile).read())
    new_size = len(html)
    print(f'{infile}: {orig_size} bytes')
    print(f'{outfile}: {new_size} bytes ({100 - new_size * 100 // orig_size}% smaller)')

if __name__ == '__main__':
    main()
