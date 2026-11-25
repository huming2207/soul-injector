#!/usr/bin/env python3
import os
import struct
import subprocess
import sys
import tempfile

tool, src_dir, out = sys.argv[1:4]
blob = b''
with tempfile.TemporaryDirectory() as tmp:
    for file in sorted(os.listdir(src_dir)):
        name, ext = os.path.splitext(file)
        qbc = os.path.join(tmp, name + '.qbc')
        cmd = [sys.executable, tool, 'compile', os.path.join(src_dir, file), '-o', qbc, '--name', name]
        if ext == '.js':
            cmd.append('--script')
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        data = open(qbc, 'rb').read()
        blob += name.encode() + b'\0' + struct.pack('<I', len(data)) + data
with open(out, 'wb') as f:
    f.write(blob)
