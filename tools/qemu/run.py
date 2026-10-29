"""Drive the sipwire firmware in QEMU over its serial console.

usage: python -I run.py <build_dir> <script-file> [log-file]
script lines:  do <text>                  type a console line, wait for the next `sipwire> ` prompt
               send <text>                type a console line, do not wait for the reply
               prompt [timeout_s]         wait until the console is idle (a prompt is out)
               expect <regex> [timeout_s]
               absent <regex> [seconds]   fails if a matching line shows up in the window
               poll <timeout_s> <regex> :: <text>
                                          repeat `do <text>` until a line of its reply matches
               sleep <s>                  only where time itself is the thing tested
               shell <cmd>
               include <file>             steps of another file (relative to this one)
               # comment
`${NAME}` in do/send/expect/absent/poll lines is replaced by the environment variable NAME
(when set). A line view of the raw serial stream feeds expect/absent/poll; the prompt has no
trailing newline, so it is detected on the raw bytes. `do` and `send` first wait for the
console to be idle (the previous command's prompt), so a line is never typed into a running
command.
Every ### line carries the host wall time since start and the duration of the step.
Exits non-zero on the first failed step. The same QEMU process (and so the same flash image,
OTA slots included) lives for the whole script: a firmware reboot (esp_restart) does not
restart QEMU. Scratch files (flash copy, efuse) are named after the script, so several
instances can run side by side.
"""
import binascii
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

PROMPT = b'sipwire> '
CMD_TMO = 15.0      # idle / prompt wait before typing, and for the prompt after typing
CHAR_DELAY = float(os.environ.get('RUN_CHAR_DELAY', '0.005'))

build, script = sys.argv[1], sys.argv[2]
logpath = sys.argv[3] if len(sys.argv) > 3 else None
T0 = time.monotonic()

name = re.sub(r'\W+', '_', os.path.splitext(os.path.basename(script))[0])
efuse = os.path.join(build, f'qemu_efuse-{name}.bin')
src = open(os.environ['IDF_PATH'] + '/tools/idf_py_actions/qemu_ext.py').read()
blk = src[src.index("'esp32s3': QemuTarget("):]
hexs = ''.join(re.findall(r"'([0-9a-f]{20,})'", blk[:blk.index('-global driver=esp32s3.gpio')]))
open(efuse, 'wb').write(binascii.unhexlify(hexs))

# Boot a scratch copy: the firmware writes NVS, and every run must start clean.
flash = os.path.join(build, f'qemu_flash_run-{name}.bin')
shutil.copyfile(os.path.join(build, 'qemu_flash.bin'), flash)

qemu = os.environ.get('QEMU', 'qemu-system-xtensa')
args = [qemu, '-M', 'esp32s3', '-m', '8M',             # PSRAM size, as on the N16R8
        '-drive', f'file={flash},if=mtd,format=raw',
        '-drive', f'file={efuse},if=none,format=raw,id=efuse',
        '-global', 'driver=nvram.esp32s3.efuse,property=drive,value=efuse',
        '-global', 'driver=timer.esp32s3.timg,property=wdt_disable,value=true',
        '-nic', 'user,model=open_eth',
        '-display', 'none', '-monitor', 'none', '-serial', 'stdio']
p = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

buf = []            # completed lines (ANSI colours stripped)
lock = threading.Lock()
prompts = 0         # prompts seen on the raw stream
# Console lines typed, plus one per ROM boot banner after the first. The console is idle when
# prompts > sent: every line and every reboot has had its prompt, so a reboot mid-script keeps
# the bookkeeping straight (the very first prompt is the boot prompt).
sent = 0
first_boot = True
ROM = b'ESP-ROM:'
eof = threading.Event()     # QEMU's output ended (it exited or crashed)


class QemuDied(Exception):
    pass


def check_alive():
    """A QEMU that exited can never answer: fail now instead of at the step's timeout."""
    if eof.is_set():
        raise QemuDied(f'QEMU exited (rc={p.wait()})')
log = open(logpath, 'w') if logpath else None


def out(text):
    """stdout and log, one line."""
    print(text, flush=True)
    if log:
        log.write(text + '\n')
        log.flush()


def reader():
    global prompts, sent, first_boot
    fd = p.stdout.fileno()
    part = b''              # incomplete line
    tail = b''              # last bytes of the stream, so a prompt split across reads still counts
    while True:
        data = os.read(fd, 65536)
        if not data:
            break
        scan = tail + data
        n = scan.count(PROMPT) - tail.count(PROMPT)     # only matches that end in the new data
        boots = scan.count(ROM) - tail.count(ROM)
        if boots and first_boot:
            boots -= 1
            first_boot = False
        tail = scan[-(max(len(PROMPT), len(ROM)) - 1):]
        lines = []
        part += data
        while b'\n' in part:
            raw, part = part.split(b'\n', 1)
            line = raw.decode('utf-8', 'replace').rstrip('\r')
            lines.append(re.sub(r'\x1b\[[0-9;]*m', '', line))
        with lock:
            prompts += n
            sent += boots
            buf.extend(lines)
        for line in lines:
            out(line)
    if part:
        out(part.decode('utf-8', 'replace'))
    eof.set()


def expand(text):
    return re.sub(r'\$\{(\w+)\}', lambda m: os.environ.get(m.group(1), m.group(0)), text)


def steps(path, seen=()):
    """Yield (location, line) of the script, includes expanded."""
    if path in seen:
        raise SystemExit(f'include loop at {path}')
    for no, ln in enumerate(open(path), 1):
        ln = ln.strip()
        if not ln or ln.startswith('#'):
            continue
        cmd, _, arg = ln.partition(' ')
        if cmd == 'include':
            yield from steps(os.path.join(os.path.dirname(path), arg.strip()), seen + (path,))
        else:
            yield f'{os.path.basename(path)}:{no}', ln


def stamp(t_step):
    now = time.monotonic()
    return f'[{now - T0:6.1f}s +{now - t_step:.1f}s]'


def wait_prompt(n_needed, tmo):
    """Wait until at least n_needed prompts have been seen."""
    end = time.monotonic() + tmo
    while time.monotonic() < end:
        with lock:
            if prompts >= n_needed:
                return True
        check_alive()
        time.sleep(0.02)
    return False


def type_line(text):
    global sent
    with lock:
        sent += 1
    out(f'>>> {text}')
    data = (text + '\r').encode()
    if CHAR_DELAY <= 0:
        p.stdin.write(data)
        p.stdin.flush()
        return
    for ch in data:
        p.stdin.write(bytes([ch]))
        p.stdin.flush()
        time.sleep(CHAR_DELAY)


def scan_lines(pat, start, tmo):
    """Search buf[start:] for pat until tmo; returns (matching line or None, new position)."""
    end = time.monotonic() + tmo
    pos = start
    while True:
        with lock:
            while pos < len(buf):
                line = buf[pos]
                pos += 1
                if pat.search(line):
                    return line, pos
        if time.monotonic() >= end:
            return None, pos
        check_alive()
        time.sleep(0.03)


threading.Thread(target=reader, daemon=True).start()
signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))   # the finally below stops QEMU
pos = 0
rc = 0
timing = []
try:
    for loc, ln in steps(script):
        cmd, _, arg = ln.partition(' ')
        if cmd in ('do', 'send', 'expect', 'absent', 'poll'):
            arg = expand(arg)
        t_step = time.monotonic()
        if cmd in ('do', 'send'):
            with lock:
                need = sent + 1     # idle: every typed line has had its prompt (boot prompt = first)
            if not wait_prompt(need, CMD_TMO):
                out(f'### {stamp(t_step)} {cmd} {arg}: console not idle (no prompt) at {loc}')
                rc = 1
                break
            type_line(arg)
            if cmd == 'do':
                with lock:
                    need = sent + 1
                ok = wait_prompt(need, CMD_TMO)
                out(f'### {stamp(t_step)} do {arg}: {"OK" if ok else "NO PROMPT"}')
                if not ok:
                    rc = 1
                    break
        elif cmd == 'prompt':
            tmo = float(arg) if arg else CMD_TMO
            with lock:
                need = sent + 1
            ok = wait_prompt(need, tmo)
            out(f'### {stamp(t_step)} prompt: {"OK" if ok else "TIMEOUT"}')
            if not ok:
                rc = 1
                break
        elif cmd == 'sleep':
            time.sleep(float(arg))
            out(f'### {stamp(t_step)} sleep {arg}')
        elif cmd == 'expect':
            m = re.match(r'(.*?)(?:\s+(\d+))?$', arg)
            pat, tmo = re.compile(m.group(1)), float(m.group(2) or 15)
            line, pos = scan_lines(pat, pos, tmo)
            out(f'### {stamp(t_step)} expect {pat.pattern}: {"OK" if line is not None else "TIMEOUT"}')
            if line is None:
                rc = 1
                break
        elif cmd == 'absent':
            m = re.match(r'(.*?)(?:\s+(\d+))?$', arg)
            pat, secs = re.compile(m.group(1)), float(m.group(2) or 3)
            hit, pos = scan_lines(pat, pos, secs)
            out(f'### {stamp(t_step)} absent {pat.pattern}: {"OK" if hit is None else "FOUND " + hit}')
            if hit is not None:
                rc = 1
                break
        elif cmd == 'poll':
            m = re.match(r'(\d+)\s+(.*?)\s+::\s+(.*)$', arg)
            if not m:
                out(f'### bad poll line at {loc}: {ln}')
                rc = 1
                break
            tmo, pat, text = float(m.group(1)), re.compile(m.group(2)), m.group(3)
            end = time.monotonic() + tmo
            found = None
            start = 0
            tries = 0
            while found is None and time.monotonic() < end:
                tries += 1
                with lock:
                    need = sent + 1
                if not wait_prompt(need, CMD_TMO):
                    break
                with lock:
                    start = len(buf)
                type_line(text)
                with lock:
                    need = sent + 1
                if not wait_prompt(need, CMD_TMO):
                    break
                found, _ = scan_lines(pat, start, 0.3)
                if found is None:
                    time.sleep(0.3)
            pos = start     # the reply of the last try stays visible to the expects that follow
            out(f'### {stamp(t_step)} poll {text} ~ {pat.pattern}: '
                f'{"OK" if found is not None else "TIMEOUT"} ({tries} tries)')
            if found is None:
                rc = 1
                break
        elif cmd == 'shell':
            r = subprocess.run(arg, shell=True)
            out(f'### {stamp(t_step)} shell {arg}: rc={r.returncode}')
            if r.returncode != 0:
                rc = 1
                break
        else:
            out(f'### unknown step at {loc}: {ln}')
            rc = 1
            break
        timing.append((time.monotonic() - t_step, ln))
except QemuDied as e:
    out(f'### [{time.monotonic() - T0:6.1f}s] {e}')
    rc = 1
finally:
    p.kill()
    p.wait()
    for f in (flash, efuse):
        try:
            os.unlink(f)
        except OSError:
            pass
slow = sorted(timing, reverse=True)[:5]
out('### slowest steps: ' + '; '.join(f'{d:.1f}s {l[:50]}' for d, l in slow))
out(f'### total {time.monotonic() - T0:.1f}s')
out('RESULT ' + ('OK' if rc == 0 else 'FAIL'))
sys.exit(rc)
