"""Runs bare-metal PowerPC programs on qemu-system-ppc64 (the empty `none`
machine, a 970FX, 3 GiB of RAM) and dumps what they leave: the oracle of the
Xbox 360 lifter tests, a 64-bit CPU running in 32-bit mode as a Xenon runs a
title (the programs clear MSR[SF] in oracle/crt0.S).

  qemu_run.py --qemu qemu-system-ppc64 --vectors vectors.elf --program prog.elf --out file

The program ends with `sc` (oracle/vectors.S writes the done area, oracle.h);
then [data, data + length) of the done area is written to --out. Any other
interrupt, or no end within --timeout seconds, is a failure. Standard
library only."""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

DONE = 0x00E00000
MAGIC = 0xD0E0D0E0


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        self.s.connect(path)
        self.f = self.s.makefile('rw')
        self.f.readline()
        self.cmd('qmp_capabilities')

    def cmd(self, name, **args):
        msg = {'execute': name}
        if args:
            msg['arguments'] = args
        self.f.write(json.dumps(msg) + '\n')
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError('QEMU closed the QMP connection')
            r = json.loads(line)
            if 'error' in r:
                raise RuntimeError(r['error'])
            if 'return' in r:
                return r['return']

    def hmp(self, line):
        return self.cmd('human-monitor-command', **{'command-line': line})


def read_done(q, path):
    q.cmd('pmemsave', val=DONE, size=24, filename=path)
    with open(path, 'rb') as f:
        return struct.unpack('>6I', f.read(24))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--qemu', default='qemu-system-ppc64')
    ap.add_argument('--vectors', required=True)
    ap.add_argument('--program', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--timeout', type=float, default=120)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='qrun', dir='/tmp') as tmp:
        sock = os.path.join(tmp, 'q')
        p = subprocess.Popen([a.qemu, '-M', 'none', '-cpu', '970fx', '-m', '3G', '-nographic',
                              '-monitor', 'none', '-serial', 'none', '-S',
                              '-device', 'loader,file=' + a.vectors,
                              '-device', 'loader,file=' + a.program + ',cpu-num=0',
                              '-qmp', 'unix:' + sock + ',server=on,wait=off'],
                             stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            for _ in range(200):
                if os.path.exists(sock) or p.poll() is not None:
                    break
                time.sleep(0.02)
            if p.poll() is not None:
                sys.exit('qemu_run: QEMU failed: ' + p.stderr.read().decode())
            q = Qmp(sock)
            q.cmd('cont')
            end = time.time() + a.timeout
            scratch = os.path.join(tmp, 'done')
            while True:
                magic, status, code, length, data, srr0 = read_done(q, scratch)
                if magic == MAGIC:
                    break
                if time.time() > end:
                    q.cmd('stop')
                    sys.exit('qemu_run: no end within %gs; %s' % (a.timeout, q.hmp('info registers')[:600]))
                time.sleep(0.02)
            if status:
                sys.exit('qemu_run: interrupt %#x at %#x\n%s' % (status, srr0, q.hmp('info registers')[:1500]))
            q.cmd('pmemsave', val=data, size=length, filename=os.path.abspath(a.out))
            q.cmd('quit')
        finally:
            if p.poll() is None:
                p.kill()
            p.wait()
    print('qemu_run: %s ended with code %d, %d bytes of output' % (os.path.basename(a.program), code, length))


if __name__ == '__main__':
    main()
