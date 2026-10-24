#!/usr/bin/env python3
"""Static recompiler for Game Boy cartridges (SM83 -> C).

    recomp.py game.gb -o generated/ [--roots misses.txt] [--noreturn-rst 28] [--listing]

For 32 KB ROM-only cartridges (mapper 0):
    Writes: game.c, interp.c, game_info.h, [listing.txt]

For MBC1 multi-bank cartridges (mapper 1):
    Writes: game_bank0.c, game_bank1.c, ..., game_bankN.c,
            game_banked.c   (top-level dispatcher),
            interp.c, game_info.h, [listing.txt]

No ROM bytes are copied into the output; the runtime loads the ROM at start-up.
"""
import argparse
import os
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sm83 import decode, ILLEGAL  # noqa: E402

VECTORS = [0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38,   # RST
           0x40, 0x48, 0x50, 0x58, 0x60]                      # interrupts
ENTRY = 0x100


def decode_at(rom, a):
    op = rom[a]
    b1 = rom[a + 1] if a + 1 < len(rom) else 0
    b2 = rom[a + 2] if a + 2 < len(rom) else 0
    return decode(op, b1, b2, pc=a)


def auto_noreturn(rom):
    """RST vectors that behave like `call table; .dw ...`: the handler POPs the
    return address (before pushing anything) and ends in JP HL.  Callers never
    resume after such an RST, the bytes after it are a table."""
    found = set()
    for v in (0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38):
        a, pushed, popped, jphl = v, False, False, False
        for _ in range(14):
            if a >= len(rom):
                break
            ins = decode_at(rom, a)
            if ins.op in (0xC5, 0xD5, 0xE5, 0xF5):
                pushed = True
            if ins.op in (0xC1, 0xD1, 0xE1) and not pushed:
                popped = True
            if ins.kind == 'jphl':
                jphl = True
                break
            if ins.kind in ('ret', 'reti', 'jp', 'call', 'illegal') and ins.cond is None:
                break
            a += ins.length
        if popped and jphl:
            found.add(v)
    return found


# ---------------------------------------------------------------------------
# Bank-aware discovery for MBC1 ROMs
# ---------------------------------------------------------------------------

def discover_bank0(rom, bank0, extra_roots, noreturn):
    """Discover instructions in the fixed bank (0x0000-0x3FFF).
    bank0 is the 16 KB slice at offset 0.
    Calls into 0x4000-0x7FFF are noted but their targets are left for per-bank discovery.
    """
    insns = {}    # addr -> Insn (addr in 0x0000..0x3FFF)
    labels = set()
    roots = set(VECTORS) | {ENTRY} | set(extra_roots)
    work = sorted(roots, reverse=True)
    labels |= roots
    sites = {}
    tables_done = set()

    def decode_b0(a):
        op = bank0[a]
        b1 = bank0[a + 1] if a + 1 < len(bank0) else 0
        b2 = bank0[a + 2] if a + 2 < len(bank0) else 0
        return decode(op, b1, b2, pc=a)

    def walk():
        while work:
            a = work.pop()
            while 0 <= a < 0x4000 and a not in insns:
                ins = decode_b0(a)
                insns[a] = ins
                nxt = a + ins.length
                if ins.kind in ('jp', 'call', 'rst'):
                    t = ins.target
                    if t < 0x4000:
                        labels.add(t)
                        work.append(t)
                    # targets >= 0x4000 are cross-bank calls; handled per-bank
                if ins.kind == 'jp' and ins.cond is None:
                    break
                if ins.kind in ('ret', 'reti', 'jphl', 'illegal') and ins.cond is None:
                    break
                if ins.kind in ('call', 'rst'):
                    if ins.kind == 'rst' and ins.target in noreturn:
                        sites[a] = ins.target
                        break
                    if nxt < 0x4000:
                        labels.add(nxt)
                elif ins.kind == 'jp' and ins.cond is not None and nxt < 0x4000:
                    labels.add(nxt)
                a = nxt

    walk()
    while True:
        new_roots = []
        for s_addr in sorted(sites):
            if s_addr in tables_done:
                continue
            tables_done.add(s_addr)
            pos = s_addr + 1
            targets = []
            lo = 0x4000
            while pos + 1 < 0x4000 and len(targets) < 128 and pos < lo:
                t = bank0[pos] | (bank0[pos + 1] << 8)
                if t < 0x100 or t >= 0x4000 or s_addr <= t < pos + 2:
                    break
                targets.append(t)
                lo = min(lo, t)
                pos += 2
            new_roots += targets
        new_roots = [t for t in new_roots if t not in insns]
        if not new_roots:
            break
        for t in new_roots:
            labels.add(t)
            work.append(t)
        walk()
    return insns, labels


def discover_banked(bank_slice, bank_no, extra_roots, noreturn):
    """Discover instructions in a switchable bank (mapped at 0x4000-0x7FFF).
    bank_slice is the 16 KB slice for this bank.
    All addresses are in the 0x4000-0x7FFF range.
    """
    insns = {}    # addr -> Insn (addr in 0x4000..0x7FFF)
    labels = set()
    # Standard roots for the banked region: nothing by default —
    # we add cross-bank call targets discovered from bank0 + extra_roots
    roots = set()
    for e in extra_roots:
        if 0x4000 <= e < 0x8000:
            roots.add(e)
    work = list(roots)
    labels |= roots
    sites = {}
    tables_done = set()

    def decode_bk(a):
        off = a - 0x4000
        op = bank_slice[off]
        b1 = bank_slice[off + 1] if off + 1 < len(bank_slice) else 0
        b2 = bank_slice[off + 2] if off + 2 < len(bank_slice) else 0
        return decode(op, b1, b2, pc=a)

    def walk():
        while work:
            a = work.pop()
            while 0x4000 <= a < 0x8000 and a not in insns:
                ins = decode_bk(a)
                insns[a] = ins
                nxt = a + ins.length
                if ins.kind in ('jp', 'call', 'rst'):
                    t = ins.target
                    if 0x4000 <= t < 0x8000:
                        labels.add(t)
                        work.append(t)
                if ins.kind == 'jp' and ins.cond is None:
                    break
                if ins.kind in ('ret', 'reti', 'jphl', 'illegal') and ins.cond is None:
                    break
                if ins.kind in ('call', 'rst'):
                    if ins.kind == 'rst' and ins.target in noreturn:
                        sites[a] = ins.target
                        break
                    if 0x4000 <= nxt < 0x8000:
                        labels.add(nxt)
                elif ins.kind == 'jp' and ins.cond is not None and 0x4000 <= nxt < 0x8000:
                    labels.add(nxt)
                a = nxt

    walk()
    while True:
        new_roots = []
        for s_addr in sorted(sites):
            if s_addr in tables_done:
                continue
            tables_done.add(s_addr)
            pos = s_addr + 1
            targets = []
            lo = 0x8000
            while pos + 1 < 0x8000 and len(targets) < 128 and pos < lo:
                # table entries are in the ROM - but RST tables can point anywhere
                off = pos - 0x4000
                t = bank_slice[off] | (bank_slice[off + 1] << 8)
                if t < 0x100 or t >= 0x8000 or s_addr <= t < pos + 2:
                    break
                targets.append(t)
                lo = min(lo, t)
                pos += 2
            new_roots += targets
        new_roots = [t for t in new_roots if t not in insns]
        if not new_roots:
            break
        for t in new_roots:
            labels.add(t)
            work.append(t)
        walk()
    return insns, labels


# ---------------------------------------------------------------------------
# Legacy 32 KB ROM-only discovery (unchanged)
# ---------------------------------------------------------------------------

def discover(rom, extra_roots, noreturn):
    insns = {}
    labels = set()
    roots = set(VECTORS) | {ENTRY} | set(extra_roots)
    work = sorted(roots, reverse=True)
    labels |= roots
    sites = {}
    tables_done = set()

    def walk():
        while work:
            a = work.pop()
            while 0 <= a < len(rom) and a not in insns:
                ins = decode_at(rom, a)
                insns[a] = ins
                nxt = a + ins.length
                if ins.kind in ('jp', 'call', 'rst'):
                    t = ins.target
                    if t < len(rom):
                        labels.add(t)
                        work.append(t)
                if ins.kind == 'jp' and ins.cond is None:
                    break
                if ins.kind in ('ret', 'reti', 'jphl', 'illegal') and ins.cond is None:
                    break
                if ins.kind in ('call', 'rst'):
                    if ins.kind == 'rst' and ins.target in noreturn:
                        sites[a] = ins.target
                        break
                    if nxt < len(rom):
                        labels.add(nxt)
                elif ins.kind == 'jp' and ins.cond is not None and nxt < len(rom):
                    labels.add(nxt)
                a = nxt

    walk()
    while True:
        new_roots = []
        for s_addr in sorted(sites):
            if s_addr in tables_done:
                continue
            tables_done.add(s_addr)
            pos = s_addr + 1
            targets = []
            lo = len(rom)
            while pos + 1 < len(rom) and len(targets) < 128 and pos < lo:
                t = rom[pos] | (rom[pos + 1] << 8)
                if t < 0x100 or t >= len(rom) or s_addr <= t < pos + 2:
                    break
                targets.append(t)
                lo = min(lo, t)
                pos += 2
            new_roots += targets
        new_roots = [t for t in new_roots if t not in insns]
        if not new_roots:
            break
        for t in new_roots:
            labels.add(t)
            work.append(t)
        walk()
    return insns, labels


# ---------------------------------------------------------------------------
# Code emission helpers
# ---------------------------------------------------------------------------

def emit_bank_function(insns, labels, func_name, is_banked):
    """Emit a single bank function.

    is_banked=False: bank 0 (fixed), addresses 0x0000-0x3FFF.
    is_banked=True:  switchable bank, addresses 0x4000-0x7FFF.

    Returns (C code string, set of cross-bank targets found in this bank).
    """
    addrs = sorted(insns)
    if not addrs:
        return (f'/* empty bank */\nint {func_name}(void) {{ return 0; }}\n', set())

    nxt_phys = {a: (addrs[i + 1] if i + 1 < len(addrs) else None) for i, a in enumerate(addrs)}

    for a in addrs:
        ins = insns[a]
        flows = ins.kind in ('seq', 'halt') or ins.cond is not None
        if ins.kind == 'jp' and ins.cond is None:
            flows = False
        if ins.kind in ('reti', 'jphl', 'illegal') or (ins.kind == 'ret' and ins.cond is None):
            flows = False
        if ins.kind == 'rst':
            flows = True
        if flows:
            n = a + ins.length
            if n in insns and nxt_phys[a] != n:
                labels.add(n)

    # Cross-bank call targets (calls from bank0 into 0x4000-0x7FFF) — collect
    # them so the caller knows which banked addresses need to be discovered.
    cross_targets = set()

    def goto(t):
        if is_banked and 0 <= t < 0x4000:
            # returning to fixed bank: set PC and return 0 (re-dispatch)
            return f'cpu.pc = 0x{t:04X}; return 0;'
        if not is_banked and 0x4000 <= t < 0x8000:
            cross_targets.add(t)
            return f'cpu.pc = 0x{t:04X}; return 0;'
        if t in insns:
            return f'goto L_{t:04X};'
        return f'cpu.pc = 0x{t:04X}; return 0;'

    def fall(a, ins):
        n = (a + ins.length) & 0xFFFF
        if nxt_phys[a] == n and n in insns:
            return ''
        return goto(n)

    out = []
    w = out.append
    w(f'int {func_name}(void)')
    w('{')
    w('dispatch:')
    w('    switch (cpu.pc) {')
    for t in sorted(l for l in labels if l in insns):
        w(f'    case 0x{t:04X}: goto L_{t:04X};')
    w('    default: return 0;  /* unknown PC: re-dispatch at top level */')
    w('    }')
    for a in addrs:
        ins = insns[a]
        if a in labels:
            w(f'L_{a:04X}:;')
        nxt = (a + ins.length) & 0xFFFF
        w(f'    /* {a:04X}  {ins.text} */')
        k = ins.kind
        if k == 'illegal':
            w(f'    cpu_lockup(0x{ins.op:02X}, 0x{a:04X});')
        elif k == 'seq':
            if ins.body:
                w(f'    {ins.body}')
            w(f'    tick({ins.taken});')
            irq_check = f'    if (__builtin_expect(cpu_irq_check(), 0)) {{ cpu.pc = 0x{nxt:04X}; return 1; }}'
            w(irq_check)
            f = fall(a, ins)
            if f:
                w('    ' + f)
        elif k == 'halt':
            w(f'    tick(4); cpu_halt();')
            w(f'    if (__builtin_expect(cpu_irq_check(), 0)) {{ cpu.pc = 0x{nxt:04X}; return 1; }}')
            f = fall(a, ins)
            if f:
                w('    ' + f)
        elif k == 'jp':
            t2 = ins.target
            if ins.cond is None:
                w(f'    tick({ins.taken});')
                w(f'    if (__builtin_expect(cpu_irq_check(), 0)) {{ cpu.pc = 0x{t2:04X}; return 1; }}')
                w(f'    ' + goto(t2))
            else:
                w(f'    if ({ins.cond}) {{')
                w(f'        tick({ins.taken});')
                w(f'        if (__builtin_expect(cpu_irq_check(), 0)) {{ cpu.pc = 0x{t2:04X}; return 1; }}')
                w(f'        ' + goto(t2))
                w(f'    }}')
                w(f'    tick({ins.nt});')
                w(f'    if (__builtin_expect(cpu_irq_check(), 0)) {{ cpu.pc = 0x{nxt:04X}; return 1; }}')
                f = fall(a, ins)
                if f:
                    w('    ' + f)
        elif k in ('call', 'rst'):
            t2 = ins.target
            if ins.cond is None:
                w(f'    push16(0x{nxt:04X}); tick({ins.taken});')
                w(f'    if (__builtin_expect(cpu_irq_check(), 0)) {{ cpu.pc = 0x{t2:04X}; return 1; }}')
                w(f'    ' + goto(t2))
            else:
                w(f'    if ({ins.cond}) {{')
                w(f'        push16(0x{nxt:04X}); tick({ins.taken});')
                w(f'        if (__builtin_expect(cpu_irq_check(), 0)) {{ cpu.pc = 0x{t2:04X}; return 1; }}')
                w(f'        ' + goto(t2))
                w(f'    }}')
                w(f'    tick({ins.nt});')
                w(f'    if (__builtin_expect(cpu_irq_check(), 0)) {{ cpu.pc = 0x{nxt:04X}; return 1; }}')
                f = fall(a, ins)
                if f:
                    w('    ' + f)
        elif k == 'ret':
            if ins.cond is None:
                w(f'    cpu.pc = pop16(); tick({ins.taken});')
                w(f'    if (__builtin_expect(cpu_irq_check(), 0)) return 1;')
                w(f'    return 0;')
            else:
                w(f'    if ({ins.cond}) {{')
                w(f'        cpu.pc = pop16(); tick({ins.taken});')
                w(f'        if (__builtin_expect(cpu_irq_check(), 0)) return 1;')
                w(f'        return 0;')
                w(f'    }}')
                w(f'    tick({ins.nt});')
                w(f'    if (__builtin_expect(cpu_irq_check(), 0)) {{ cpu.pc = 0x{nxt:04X}; return 1; }}')
                f = fall(a, ins)
                if f:
                    w('    ' + f)
        elif k == 'reti':
            w(f'    cpu.ime = 1; cpu.pc = pop16(); tick({ins.taken});')
            w(f'    return 0;  /* reti: always re-dispatch (may have unmasked an IRQ) */')
        elif k == 'jphl':
            w(f'    cpu.pc = HL(); tick(4);')
            w(f'    return 0;')
        else:
            raise RuntimeError(k)
    w('    return 0;')
    w('}')
    return '\n'.join(out) + '\n', cross_targets


def emit_lifted(insns, labels, rom_len, prefix=''):
    """Original single-file emitter for 32 KB ROM-only cartridges."""
    addrs = sorted(insns)
    nxt_phys = {a: (addrs[i + 1] if i + 1 < len(addrs) else None) for i, a in enumerate(addrs)}

    for a in addrs:
        ins = insns[a]
        flows = ins.kind in ('seq', 'halt') or ins.cond is not None
        if ins.kind == 'jp' and ins.cond is None:
            flows = False
        if ins.kind in ('reti', 'jphl', 'illegal') or (ins.kind == 'ret' and ins.cond is None):
            flows = False
        if ins.kind == 'rst':
            flows = True
        if flows:
            n = a + ins.length
            if n in insns and nxt_phys[a] != n:
                labels.add(n)

    def goto(t):
        if t in insns:
            return f'goto L_{t:04X};'
        return f'cpu.pc = 0x{t:04X}; goto dispatch;'

    def fall(a, ins):
        n = (a + ins.length) & 0xFFFF
        if nxt_phys[a] == n and n in insns:
            return ''
        return goto(n)

    out = []
    w = out.append
    w('/* GENERATED by tools/recomp.py - do not edit. */')
    w('#include "sm83.h"')
    w('')
    w(f'void recomp_run_{prefix}(void)')
    w('{')
    w('dispatch:')
    w('    switch (cpu.pc) {')
    for t in sorted(l for l in labels if l in insns):
        w(f'    case 0x{t:04X}: goto L_{t:04X};')
    w('    default: goto miss;')
    w('    }')
    w('miss:')
    w('    recomp_miss(cpu.pc);')
    w('    cpu_step_checked();')
    w('    goto dispatch;')
    w('irq:')
    w('    cpu_service_irq();')
    w('    goto dispatch;')
    w('')
    for a in addrs:
        ins = insns[a]
        if a in labels:
            w(f'L_{a:04X}:;')
        nxt = (a + ins.length) & 0xFFFF
        c = f'    /* {a:04X}  {ins.text} */'
        w(c)
        k = ins.kind
        if k == 'illegal':
            w(f'    cpu_lockup(0x{ins.op:02X}, 0x{a:04X});')
        elif k == 'seq':
            if ins.body:
                w(f'    {ins.body}')
            w(f'    tick({ins.taken}); NEXT(0x{nxt:04X});')
            f = fall(a, ins)
            if f:
                w('    ' + f)
        elif k == 'halt':
            w(f'    tick(4); cpu_halt(); NEXT(0x{nxt:04X});')
            f = fall(a, ins)
            if f:
                w('    ' + f)
        elif k == 'jp':
            t = ins.target
            if ins.cond is None:
                w(f'    tick({ins.taken}); NEXT(0x{t:04X}); {goto(t)}')
            else:
                w(f'    if ({ins.cond}) {{ tick({ins.taken}); NEXT(0x{t:04X}); {goto(t)} }}')
                w(f'    tick({ins.nt}); NEXT(0x{nxt:04X});')
                f = fall(a, ins)
                if f:
                    w('    ' + f)
        elif k in ('call', 'rst'):
            t = ins.target
            pre = f'push16(0x{nxt:04X}); tick({ins.taken}); NEXT(0x{t:04X}); {goto(t)}'
            if ins.cond is None:
                w(f'    {pre}')
            else:
                w(f'    if ({ins.cond}) {{ {pre} }}')
                w(f'    tick({ins.nt}); NEXT(0x{nxt:04X});')
                f = fall(a, ins)
                if f:
                    w('    ' + f)
        elif k == 'ret':
            if ins.cond is None:
                w(f'    cpu.pc = pop16(); tick({ins.taken}); NEXT(cpu.pc); goto dispatch;')
            else:
                w(f'    if ({ins.cond}) {{ cpu.pc = pop16(); tick({ins.taken}); NEXT(cpu.pc); goto dispatch; }}')
                w(f'    tick({ins.nt}); NEXT(0x{nxt:04X});')
                f = fall(a, ins)
                if f:
                    w('    ' + f)
        elif k == 'reti':
            w(f'    cpu.ime = 1; cpu.pc = pop16(); tick({ins.taken}); NEXT(cpu.pc); goto dispatch;')
        elif k == 'jphl':
            w(f'    cpu.pc = HL(); tick(4); NEXT(cpu.pc); goto dispatch;')
        else:
            raise RuntimeError(k)
    w('}')
    # bitmap of ROM bytes covered by recompiled instructions
    mask = bytearray(0x1000)
    for a, ins in insns.items():
        for k in range(ins.length):
            if a + k < 0x8000:
                mask[(a + k) >> 3] |= 1 << ((a + k) & 7)
    w('')
    w(f'const uint8_t recomp_code_mask_{prefix}[0x1000] = {{')
    for i in range(0, 0x1000, 32):
        w('    ' + ','.join(str(b) for b in mask[i:i + 32]) + ',')
    w('};')
    return '\n'.join(out) + '\n'


def emit_interp(prefix=''):
    """Interpreter generated from the same opcode table."""
    suffix = f'_{prefix}' if prefix else ''
    out = []
    w = out.append
    w('/* GENERATED by tools/recomp.py - do not edit. */')
    w('#include "sm83.h"')
    w('')
    w(f'void cpu_step{suffix}(void)')
    w('{')
    w('    uint8_t op = rd8(cpu.pc);')
    w('    switch (op) {')

    def case_body(ins, ind, cb=False):
        L = []
        a = L.append
        pre = ''
        if ins.length == 2 and not cb:
            pre = 'uint8_t n8 = rd8((uint16_t)(cpu.pc + 1)); (void)n8; '
        elif ins.length == 3:
            pre = ('uint16_t n16 = (uint16_t)(rd8((uint16_t)(cpu.pc + 1)) | '
                   '(rd8((uint16_t)(cpu.pc + 2)) << 8)); ')
        k = ins.kind
        if k == 'illegal':
            a(f'cpu_lockup(op, cpu.pc);')
        elif k in ('seq', 'halt'):
            a(f'{pre}{ins.body} cpu.pc = (uint16_t)(cpu.pc + {ins.length}); tick({ins.taken});'
              + (' cpu_halt();' if k == 'halt' else ''))
        elif k == 'jp':
            t = ins.target
            if ins.cond is None:
                a(f'{pre}cpu.pc = {t}; tick({ins.taken});')
            else:
                a(f'{pre}if ({ins.cond}) {{ cpu.pc = {t}; tick({ins.taken}); }} '
                  f'else {{ cpu.pc = (uint16_t)(cpu.pc + {ins.length}); tick({ins.nt}); }}')
        elif k in ('call', 'rst'):
            t = ins.target
            t = f'0x{t:04X}' if isinstance(t, int) else t
            if ins.cond is None:
                a(f'{pre}cpu.pc = (uint16_t)(cpu.pc + {ins.length}); push16(cpu.pc); cpu.pc = {t}; tick({ins.taken});')
            else:
                a(f'{pre}cpu.pc = (uint16_t)(cpu.pc + {ins.length}); '
                  f'if ({ins.cond}) {{ push16(cpu.pc); cpu.pc = {t}; tick({ins.taken}); }} else tick({ins.nt});')
        elif k == 'ret':
            if ins.cond is None:
                a(f'cpu.pc = pop16(); tick({ins.taken});')
            else:
                a(f'if ({ins.cond}) {{ cpu.pc = pop16(); tick({ins.taken}); }} '
                  f'else {{ cpu.pc = (uint16_t)(cpu.pc + 1); tick({ins.nt}); }}')
        elif k == 'reti':
            a(f'cpu.ime = 1; cpu.pc = pop16(); tick({ins.taken});')
        elif k == 'jphl':
            a(f'cpu.pc = HL(); tick(4);')
        return ' '.join(L)

    for op in range(256):
        if op == 0xCB:
            w('    case 0xCB: {')
            w('        uint8_t n8 = rd8((uint16_t)(cpu.pc + 1));')
            w('        switch (n8) {')
            for cb in range(256):
                ins = decode(0xCB, cb, 0, dyn=True)
                w(f'        case 0x{cb:02X}: {ins.body} cpu.pc = (uint16_t)(cpu.pc + 2); tick({ins.taken}); break;')
            w('        }')
            w('        break; }')
            continue
        ins = decode(op, dyn=True)
        w(f'    case 0x{op:02X}: {{ {case_body(ins, 0)} break; }}')
    w('    }')
    w('}')
    return '\n'.join(out) + '\n'


# ---------------------------------------------------------------------------
# Multi-bank emitter: one .c per bank + a dispatcher
# ---------------------------------------------------------------------------

def emit_banked(rom, noreturn, extra_roots_b0, out_dir, prefix=''):
    """Emit banked recompilation for an MBC1 ROM.

    Generates:
      game_bank0.c  - fixed bank (0x0000-0x3FFF)
      game_bankN.c  - each switchable bank (0x4000-0x7FFF), N = 1..total_banks-1
      game_banked.c - top-level dispatcher: extern declarations + recomp_run()

    Returns a list of all generated file paths.
    """
    bank_size = 0x4000
    n_banks = len(rom) // bank_size
    print(f'MBC1 ROM: {n_banks} banks ({len(rom) // 1024} KB)')

    # ---- bank 0 ----
    bank0_slice = rom[0:bank_size]
    b0_insns, b0_labels = discover_bank0(rom, bank0_slice, extra_roots_b0, noreturn)
    print(f'  bank 0: {len(b0_insns)} instructions, {len(b0_labels)} labels')

    # Collect cross-bank targets (calls from bank0 into 0x4000+)
    cross_targets = set()
    for ins in b0_insns.values():
        if ins.kind in ('jp', 'call', 'rst') and 0x4000 <= ins.target < 0x8000:
            cross_targets.add(ins.target)

    # ---- switchable banks ----
    # For each bank, discover code starting from cross_targets (any bank can be mapped)
    # plus any extra roots the user supplied for that range.
    extra_banked = [e for e in extra_roots_b0 if 0x4000 <= e < 0x8000]
    all_roots_hi = cross_targets | set(extra_banked)

    bank_insns = {}
    bank_labels = {}
    for b in range(1, n_banks):
        bslice = rom[b * bank_size:(b + 1) * bank_size]
        bi, bl = discover_banked(bslice, b, all_roots_hi, noreturn)
        bank_insns[b] = bi
        bank_labels[b] = bl
        print(f'  bank {b}: {len(bi)} instructions, {len(bl)} labels')

    # ---- emit bank 0 ----
    b0_func = f'recomp_bank0_{prefix}' if prefix else 'recomp_bank0'
    b0_code, _ = emit_bank_function(b0_insns, b0_labels, b0_func, is_banked=False)
    p = os.path.join(out_dir, 'game_bank0.c')
    with open(p, 'w') as f:
        f.write('/* GENERATED by tools/recomp.py - do not edit. */\n')
        f.write('#include "sm83.h"\n\n')
        f.write(b0_code)
    print(f'  wrote {p}')

    # Build coverage mask for bank 0 (0x0000-0x3FFF → first 0x800 bytes of mask)
    mask = bytearray(0x1000)
    for a in b0_insns:
        for k in range(b0_insns[a].length):
            pos = a + k
            if pos < 0x4000:
                mask[pos >> 3] |= 1 << (pos & 7)

    # ---- emit switchable banks ----
    for b in range(1, n_banks):
        bi = bank_insns[b]
        bl = bank_labels[b]
        bfunc = f'recomp_bank{b}_{prefix}' if prefix else f'recomp_bank{b}'
        bcode, _ = emit_bank_function(bi, bl, bfunc, is_banked=True)
        p = os.path.join(out_dir, f'game_bank{b}.c')
        with open(p, 'w') as f:
            f.write('/* GENERATED by tools/recomp.py - do not edit. */\n')
            f.write('#include "sm83.h"\n\n')
            f.write(bcode)
        print(f'  wrote {p}')
        for a, ins in bi.items():
            for k in range(ins.length):
                pos = a + k
                if 0x4000 <= pos < 0x8000:
                    mask[pos >> 3] |= 1 << (pos & 7)

    # ---- emit dispatcher (game_banked.c) ----
    lines = []
    w = lines.append
    w('/* GENERATED by tools/recomp.py - do not edit. */')
    w('#include "sm83.h"')
    w('#include "cart.h"')
    w('')
    w('/* Forward declarations for all bank functions. */')
    w(f'int {b0_func}(void);')
    for b in range(1, n_banks):
        w(f'int {("recomp_bank" + str(b) + "_" + prefix) if prefix else "recomp_bank" + str(b)}(void);')
    w('')
    w('extern const uint8_t *cart_hi;')
    w('extern uint8_t *rom;')
    w('')
    w('/* Returns which ROM bank is currently mapped at 0x4000. */')
    w('static inline int current_bank(void)')
    w('{')
    w('    return (int)((cart_hi - rom) >> 14);')
    w('}')
    w('')
    pfx = f'_{prefix}' if prefix else ''
    w(f'void recomp_run{pfx}(void)')
    w('{')
    w('    for (;;) {')
    w('        int r;')
    w('        if (cpu.pc < 0x4000) {')
    w(f'            r = {b0_func}();')
    w('        } else if (cpu.pc < 0x8000) {')
    w('            switch (current_bank()) {')
    for b in range(1, n_banks):
        w(f'            case {b}: r = {("recomp_bank" + str(b) + "_" + prefix) if prefix else "recomp_bank" + str(b)}(); break;')
    w('            default: recomp_miss(cpu.pc); cpu_step_checked(); r = 0; break;')
    w('            }')
    w('        } else {')
    w('            /* PC in RAM / IO / etc.: fall back to interpreter step */')
    w('            recomp_miss(cpu.pc); cpu_step_checked(); r = 0;')
    w('        }')
    w('        if (r) cpu_service_irq();')
    w('    }')
    w('}')
    w('')
    w(f'const uint8_t recomp_code_mask{pfx}[0x1000] = {{')
    for i in range(0, 0x1000, 32):
        w('    ' + ','.join(str(b) for b in mask[i:i + 32]) + ',')
    w('};')
    dp = os.path.join(out_dir, 'game_banked.c')
    with open(dp, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'  wrote {dp}')

    return n_banks


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('rom')
    ap.add_argument('-o', '--out', default='generated')
    ap.add_argument('--roots', help='file of extra code addresses (hex, one per line / whitespace separated)')
    ap.add_argument('--noreturn-rst', default='', help='extra RST vectors (hex, comma-separated)')
    ap.add_argument('--listing', action='store_true')
    ap.add_argument('--name', help='display name for the generated program (default: ROM header title)')
    ap.add_argument('--prefix', default='', help='symbol prefix for multi-game builds (e.g. "sml")')
    args = ap.parse_args()

    data = open(args.rom, 'rb').read()
    mapper = data[0x147]
    is_mbc1 = mapper in (0x01, 0x02, 0x03)
    is_rom_only = mapper in (0x00, 0x08, 0x09)

    if not is_mbc1 and not is_rom_only:
        sys.exit(f'error: mapper type 0x{mapper:02X} not supported (only ROM-only and MBC1).')

    extra = []
    if args.roots and os.path.exists(args.roots):
        extra = [int(t, 16) for t in open(args.roots).read().split() if t.strip()]
        if not is_mbc1:
            extra = [e for e in extra if e < len(data)]

    noret = {int(t, 16) for t in args.noreturn_rst.split(',') if t.strip()}

    # ROM title / CRC
    title_raw = data[0x134:0x144]
    title = ''.join(chr(b) if 32 <= b <= 126 else '' for b in title_raw).rstrip('\x00')
    crc = zlib.crc32(data) & 0xFFFFFFFF
    name = args.name or title

    os.makedirs(args.out, exist_ok=True)

    if is_rom_only:
        # ---- 32 KB single-bank path (unchanged) ----
        rom32 = data + b'\xff' * (0x8000 - len(data)) if len(data) < 0x8000 else data[:0x8000]
        noret |= auto_noreturn(rom32)
        if noret:
            print('jump-table RST vectors: ' + ', '.join(f'${v:02X}' for v in sorted(noret)))

        insns, labels = discover(rom32, extra, noret)
        lifted = emit_lifted(insns, labels, len(rom32), prefix=args.prefix)
        with open(os.path.join(args.out, 'game.c'), 'w') as f:
            f.write(lifted)
        print(f'  {len(insns)} instructions discovered')

    else:
        # ---- MBC1 multi-bank path ----
        # Pad to a power-of-two number of 16 KB banks
        bank_size = 0x4000
        n_banks = max(2, len(data) // bank_size)
        need = n_banks * bank_size
        rom_full = data + b'\xff' * (need - len(data)) if len(data) < need else data[:need]

        noret |= auto_noreturn(rom_full[:bank_size])
        if noret:
            print('jump-table RST vectors: ' + ', '.join(f'${v:02X}' for v in sorted(noret)))

        emit_banked(rom_full, noret, extra, args.out, prefix=args.prefix)

    # interp.c (always generated)
    with open(os.path.join(args.out, 'interp.c'), 'w') as f:
        f.write(emit_interp(prefix=args.prefix))

    # game_info.h
    with open(os.path.join(args.out, 'game_info.h'), 'w') as f:
        f.write(f'/* GENERATED */\n')
        f.write(f'#define ROM_CRC32 0x{crc:08X}u\n')
        f.write(f'#define ROM_TITLE "{title}"\n')
        f.write(f'#define GAME_NAME "{name}"\n')
        # For multi-bank ROMs, also record bank count so runtime can verify
        if is_mbc1:
            f.write(f'#define ROM_N_BANKS {n_banks}\n')

    if args.listing and is_rom_only:
        # listing only for single-bank for now
        with open(os.path.join(args.out, 'listing.txt'), 'w') as f:
            for a in sorted(insns):
                ins = insns[a]
                f.write(f'{a:04X}  {ins.text}\n')

    print('done.')


if __name__ == '__main__':
    main()
