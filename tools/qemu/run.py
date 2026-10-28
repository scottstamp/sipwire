"""Drive the sipwire firmware in QEMU over its serial console.

usage: python -I run.py <build_dir> <script-file> [log-file]
script lines:  send <text> | expect <regex> [timeout_s] | absent <regex> [seconds] | sleep <s>
               | shell <cmd> | # comment
`${NAME}` in send/expect/absent lines is replaced by the environment variable NAME (when set).
`absent` fails if a matching line shows up within the window (default 3 s).
Exits non-zero on the first failed expect / absent / shell step.
The same QEMU process (and so the same flash image, OTA slots included) lives for the whole
script: a firmware reboot (esp_restart) does not restart QEMU.
"""
import binascii
import os
import re
import shutil
import subprocess
import sys
import threading
import time

build, script = sys.argv[1], sys.argv[2]
logpath = sys.argv[3] if len(sys.argv) > 3 else None

efuse = os.path.join(build, 'qemu_efuse.bin')
src = open(os.environ['IDF_PATH'] + '/tools/idf_py_actions/qemu_ext.py').read()
blk = src[src.index("'esp32s3': QemuTarget("):]
hexs = ''.join(re.findall(r"'([0-9a-f]{20,})'", blk[:blk.index('-global driver=esp32s3.gpio')]))
open(efuse, 'wb').write(binascii.unhexlify(hexs))

# Boot a scratch copy: the firmware writes NVS, and every run must start clean.
flash = os.path.join(build, 'qemu_flash_run.bin')
shutil.copyfile(os.path.join(build, 'qemu_flash.bin'), flash)

qemu = os.environ.get('QEMU', 'qemu-system-xtensa')
args = [qemu, '-M', 'esp32s3', '-m', '32M',
        '-drive', f'file={flash},if=mtd,format=raw',
        '-drive', f'file={efuse},if=none,format=raw,id=efuse',
        '-global', 'driver=nvram.esp32s3.efuse,property=drive,value=efuse',
        '-global', 'driver=timer.esp32s3.timg,property=wdt_disable,value=true',
        '-nic', 'user,model=open_eth',
        '-display', 'none', '-monitor', 'none', '-serial', 'stdio']
p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

buf = []
lock = threading.Lock()
log = open(logpath, 'w') if logpath else None


def reader():
    for raw in iter(p.stdout.readline, b''):
        line = raw.decode('utf-8', 'replace').rstrip('\r\n')
        line = re.sub(r'\x1b\[[0-9;]*m', '', line)
        with lock:
            buf.append(line)
        print(line, flush=True)
        if log:
            log.write(line + '\n')
            log.flush()


def expand(text):
    return re.sub(r'\$\{(\w+)\}', lambda m: os.environ.get(m.group(1), m.group(0)), text)


threading.Thread(target=reader, daemon=True).start()
pos = 0
rc = 0
try:
    for ln in open(script):
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        cmd, _, arg = ln.partition(' ')
        if cmd in ('send', 'expect', 'absent'):
            arg = expand(arg)
        if cmd == 'send':
            print(f'>>> {arg}', flush=True)
            for ch in (arg + '\r').encode():
                p.stdin.write(bytes([ch]))
                p.stdin.flush()
                time.sleep(0.01)
        elif cmd == 'sleep':
            time.sleep(float(arg))
        elif cmd == 'expect':
            m = re.match(r'(.*?)(?:\s+(\d+))?$', arg)
            pat, tmo = re.compile(m.group(1)), float(m.group(2) or 15)
            end = time.time() + tmo
            found = False
            while time.time() < end and not found:
                with lock:
                    while pos < len(buf):
                        if pat.search(buf[pos]):
                            found = True
                        pos += 1
                        if found:
                            break
                if not found:
                    time.sleep(0.05)
            print(f'### expect {pat.pattern}: {"OK" if found else "TIMEOUT"}', flush=True)
            if not found:
                rc = 1
                break
        elif cmd == 'absent':
            m = re.match(r'(.*?)(?:\s+(\d+))?$', arg)
            pat, secs = re.compile(m.group(1)), float(m.group(2) or 3)
            end = time.time() + secs
            hit = None
            while time.time() < end and hit is None:
                with lock:
                    while pos < len(buf) and hit is None:
                        if pat.search(buf[pos]):
                            hit = buf[pos]
                        pos += 1
                time.sleep(0.05)
            print(f'### absent {pat.pattern}: {"OK" if hit is None else "FOUND " + hit}', flush=True)
            if hit is not None:
                rc = 1
                break
        elif cmd == 'shell':
            r = subprocess.run(arg, shell=True)
            print(f'### shell {arg}: rc={r.returncode}', flush=True)
            if r.returncode != 0:
                rc = 1
                break
finally:
    p.kill()
    p.wait()
print('RESULT', 'OK' if rc == 0 else 'FAIL', flush=True)
sys.exit(rc)
