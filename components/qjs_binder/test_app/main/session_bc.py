#!/usr/bin/env python3
"""Split session.js at '//--- name [module]' lines, compile each piece, bundle them: name NUL, u32 length, container."""
import os
import struct
import subprocess
import sys
import tempfile

tool, src, out = sys.argv[1:4]
pieces, cur = [], None
for line in open(src, encoding='utf-8'):
    if line.startswith('//--- '):
        words = line[6:].split()
        cur = [words[0], 'module' in words[1:], []]
        pieces.append(cur)
    elif cur:
        cur[2].append(line)

blob = b''
with tempfile.TemporaryDirectory() as tmp:
    for name, module, lines in pieces:
        js, qbc = os.path.join(tmp, name + '.js'), os.path.join(tmp, name + '.qbc')
        with open(js, 'w', encoding='utf-8') as f:
            f.write(''.join(lines))
        cmd = [sys.executable, tool, 'compile', js, '-o', qbc, '--name', name]
        if not module:
            cmd.append('--script')
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        data = open(qbc, 'rb').read()
        blob += name.encode() + b'\0' + struct.pack('<I', len(data)) + data
with open(out, 'wb') as f:
    f.write(blob)
