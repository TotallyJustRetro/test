"""SM83 (Game Boy CPU) opcode table.

Single source of truth for instruction semantics.  `decode()` returns an Insn
whose `body` is a C statement string.  The lifter (recomp.py) calls it with
concrete immediates ("static" mode); the interpreter generator calls it with
symbolic immediates `n8` / `n16` ("dyn" mode).  Both therefore share exactly the
same semantics, and differ only in operand fetch and control flow.
"""

R8N = ['B', 'C', 'D', 'E', 'H', 'L', '(HL)', 'A']
R8C = ['cpu.b', 'cpu.c', 'cpu.d', 'cpu.e', 'cpu.h', 'cpu.l', None, 'cpu.a']
RPN = ['BC', 'DE', 'HL', 'SP']
RP2N = ['BC', 'DE', 'HL', 'AF']
CCN = ['NZ', 'Z', 'NC', 'C']
CCE = ['!(cpu.f & FZ)', '(cpu.f & FZ)', '!(cpu.f & FC)', '(cpu.f & FC)']
ALUN = ['ADD', 'ADC', 'SUB', 'SBC', 'AND', 'XOR', 'OR', 'CP']
ALUF = ['alu_add', 'alu_adc', 'alu_sub', 'alu_sbc', 'alu_and', 'alu_xor', 'alu_or', 'alu_cp']
ROTN = ['RLC', 'RRC', 'RL', 'RR', 'SLA', 'SRA', 'SWAP', 'SRL']
ROTF = ['cb_rlc', 'cb_rrc', 'cb_rl', 'cb_rr', 'cb_sla', 'cb_sra', 'cb_swap', 'cb_srl']

ILLEGAL = {0xD3, 0xDB, 0xDD, 0xE3, 0xE4, 0xEB, 0xEC, 0xED, 0xF4, 0xFC, 0xFD}


def rget(i):
    return 'rd8(HL())' if i == 6 else R8C[i]


def rset(i, v):
    return f'wr8(HL(), {v});' if i == 6 else f'{R8C[i]} = {v};'


def rpget(i):
    return ['BC()', 'DE()', 'HL()', 'cpu.sp'][i]


def rpset(i, v):
    return ['SET_BC(%s);', 'SET_DE(%s);', 'SET_HL(%s);', 'cpu.sp = %s;'][i] % v


class Insn:
    """kind: seq | jp | call | ret | reti | rst | jphl | halt | illegal
    For jp/call/ret: `cond` is None or a C expression.
    `taken`/`nt` are T-cycles when the branch is taken / not taken."""
    __slots__ = ('op', 'length', 'text', 'kind', 'cond', 'taken', 'nt', 'body', 'target')

    def __init__(self, op):
        self.op = op
        self.length = 1
        self.text = '?'
        self.kind = 'seq'
        self.cond = None
        self.taken = 4
        self.nt = 4
        self.body = ''
        self.target = None


def _s8(b):
    return b - 256 if b >= 128 else b


def decode(op, b1=0, b2=0, pc=None, dyn=False):
    """Decode one instruction.  For op == 0xCB, `b1` is the CB-prefixed opcode
    (always a concrete int, even in dyn mode)."""
    I = Insn(op)
    if dyn:
        n8, n16, e8 = 'n8', 'n16', '(int8_t)n8'
    else:
        n8 = f'0x{b1:02X}'
        n16 = f'0x{(b1 | (b2 << 8)):04X}'
        e8 = str(_s8(b1))
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1

    def jr_target():
        if dyn:
            return '(uint16_t)(cpu.pc + 2 + (int8_t)n8)'
        return (pc + 2 + _s8(b1)) & 0xFFFF

    def set_text(t):
        if not dyn:
            I.text = t

    if op in ILLEGAL:
        I.kind = 'illegal'
        set_text(f'DB ${op:02X}')
        return I

    if op == 0xCB:
        cb = b1
        cx, cy, cz = cb >> 6, (cb >> 3) & 7, cb & 7
        I.length = 2
        hl = (cz == 6)
        if cx == 0:
            I.body = rset(cz, f'{ROTF[cy]}({rget(cz)})')
            I.taken = I.nt = 16 if hl else 8
            set_text(f'{ROTN[cy]} {R8N[cz]}')
        elif cx == 1:
            I.body = f'cb_bit({cy}, {rget(cz)});'
            I.taken = I.nt = 12 if hl else 8
            set_text(f'BIT {cy},{R8N[cz]}')
        elif cx == 2:
            I.body = rset(cz, f'{rget(cz)} & 0x{(~(1 << cy)) & 0xFF:02X}')
            I.taken = I.nt = 16 if hl else 8
            set_text(f'RES {cy},{R8N[cz]}')
        else:
            I.body = rset(cz, f'{rget(cz)} | 0x{1 << cy:02X}')
            I.taken = I.nt = 16 if hl else 8
            set_text(f'SET {cy},{R8N[cz]}')
        return I

    def seq(body, cyc, length=1, text='?'):
        I.body, I.taken, I.nt, I.length = body, cyc, cyc, length
        set_text(text)

    def branch(kind, cond_i, target, taken, nt, length, text):
        I.kind = kind
        I.cond = None if cond_i is None else CCE[cond_i]
        I.target = target
        I.taken, I.nt, I.length = taken, nt, length
        set_text(text)

    if x == 0:
        if z == 0:
            if y == 0:
                seq('', 4, 1, 'NOP')
            elif y == 1:
                seq(f'wr8({n16}, cpu.sp & 0xFF); wr8((uint16_t)({n16} + 1), cpu.sp >> 8);', 20, 3, f'LD (${b1 | (b2 << 8):04X}),SP')
            elif y == 2:
                seq('', 4, 2, 'STOP')
            elif y == 3:
                t = jr_target()
                branch('jp', None, t, 12, 12, 2, f'JR ${t:04X}' if not dyn else 'JR')
            else:
                t = jr_target()
                branch('jp', y - 4, t, 12, 8, 2, f'JR {CCN[y - 4]},${t:04X}' if not dyn else 'JR cc')
        elif z == 1:
            if q == 0:
                seq(rpset(p, n16), 12, 3, f'LD {RPN[p]},${b1 | (b2 << 8):04X}')
            else:
                seq(f'alu_add_hl({rpget(p)});', 8, 1, f'ADD HL,{RPN[p]}')
        elif z == 2:
            names = ['(BC)', '(DE)', '(HL+)', '(HL-)']
            if q == 0:
                src = ['wr8(BC(), cpu.a);', 'wr8(DE(), cpu.a);',
                       'wr8(HL(), cpu.a); SET_HL(HL() + 1);', 'wr8(HL(), cpu.a); SET_HL(HL() - 1);'][p]
                seq(src, 8, 1, f'LD {names[p]},A')
            else:
                src = ['cpu.a = rd8(BC());', 'cpu.a = rd8(DE());',
                       'cpu.a = rd8(HL()); SET_HL(HL() + 1);', 'cpu.a = rd8(HL()); SET_HL(HL() - 1);'][p]
                seq(src, 8, 1, f'LD A,{names[p]}')
        elif z == 3:
            if q == 0:
                seq(rpset(p, f'(uint16_t)({rpget(p)} + 1)'), 8, 1, f'INC {RPN[p]}')
            else:
                seq(rpset(p, f'(uint16_t)({rpget(p)} - 1)'), 8, 1, f'DEC {RPN[p]}')
        elif z == 4:
            seq(rset(y, f'alu_inc({rget(y)})'), 12 if y == 6 else 4, 1, f'INC {R8N[y]}')
        elif z == 5:
            seq(rset(y, f'alu_dec({rget(y)})'), 12 if y == 6 else 4, 1, f'DEC {R8N[y]}')
        elif z == 6:
            seq(rset(y, n8), 12 if y == 6 else 8, 2, f'LD {R8N[y]},${b1:02X}')
        else:
            ops = [('rlca();', 'RLCA'), ('rrca();', 'RRCA'), ('rla();', 'RLA'), ('rra();', 'RRA'),
                   ('alu_daa();', 'DAA'), ('cpu.a = ~cpu.a; cpu.f = (cpu.f & (FZ | FC)) | FN | FH;', 'CPL'),
                   ('cpu.f = (cpu.f & FZ) | FC;', 'SCF'),
                   ('cpu.f = (cpu.f & FZ) | ((cpu.f & FC) ^ FC);', 'CCF')]
            seq(ops[y][0], 4, 1, ops[y][1])
    elif x == 1:
        if op == 0x76:
            I.kind = 'halt'
            seq('', 4, 1, 'HALT')
            I.kind = 'halt'
        else:
            seq(rset(y, rget(z)), 8 if (y == 6 or z == 6) else 4, 1, f'LD {R8N[y]},{R8N[z]}')
    elif x == 2:
        seq(f'{ALUF[y]}({rget(z)});', 8 if z == 6 else 4, 1, f'{ALUN[y]} A,{R8N[z]}')
    else:
        if z == 0:
            if y < 4:
                branch('ret', y, None, 20, 8, 1, f'RET {CCN[y]}')
            elif y == 4:
                seq(f'wr8((uint16_t)(0xFF00 + {n8}), cpu.a);', 12, 2, f'LDH (${b1:02X}),A')
            elif y == 5:
                seq(f'cpu.sp = alu_add_sp({e8});', 16, 2, f'ADD SP,{e8}')
            elif y == 6:
                seq(f'cpu.a = rd8((uint16_t)(0xFF00 + {n8}));', 12, 2, f'LDH A,(${b1:02X})')
            else:
                seq(f'SET_HL(alu_ld_hl_sp({e8}));', 12, 2, f'LD HL,SP+{e8}')
        elif z == 1:
            if q == 0:
                if p == 3:
                    seq('SET_AF(pop16());', 12, 1, 'POP AF')
                else:
                    seq(rpset(p, 'pop16()'), 12, 1, f'POP {RP2N[p]}')
            else:
                if p == 0:
                    branch('ret', None, None, 16, 16, 1, 'RET')
                elif p == 1:
                    branch('reti', None, None, 16, 16, 1, 'RETI')
                elif p == 2:
                    branch('jphl', None, None, 4, 4, 1, 'JP HL')
                else:
                    seq('cpu.sp = HL();', 8, 1, 'LD SP,HL')
        elif z == 2:
            if y < 4:
                t = (b1 | (b2 << 8)) if not dyn else n16
                branch('jp', y, t, 16, 12, 3, f'JP {CCN[y]},${b1 | (b2 << 8):04X}')
            elif y == 4:
                seq('wr8((uint16_t)(0xFF00 + cpu.c), cpu.a);', 8, 1, 'LD (C),A')
            elif y == 5:
                seq(f'wr8({n16}, cpu.a);', 16, 3, f'LD (${b1 | (b2 << 8):04X}),A')
            elif y == 6:
                seq('cpu.a = rd8((uint16_t)(0xFF00 + cpu.c));', 8, 1, 'LD A,(C)')
            else:
                seq(f'cpu.a = rd8({n16});', 16, 3, f'LD A,(${b1 | (b2 << 8):04X})')
        elif z == 3:
            if y == 0:
                t = (b1 | (b2 << 8)) if not dyn else n16
                branch('jp', None, t, 16, 16, 3, f'JP ${b1 | (b2 << 8):04X}')
            elif y == 6:
                seq('cpu.ime = 0; cpu.ei_pending = 0;', 4, 1, 'DI')
            elif y == 7:
                seq('cpu.ei_pending = 2;', 4, 1, 'EI')
        elif z == 4:
            t = (b1 | (b2 << 8)) if not dyn else n16
            branch('call', y, t, 24, 12, 3, f'CALL {CCN[y]},${b1 | (b2 << 8):04X}')
        elif z == 5:
            if q == 0:
                if p == 3:
                    seq('push16(AF());', 16, 1, 'PUSH AF')
                else:
                    seq(f'push16({rpget(p)});', 16, 1, f'PUSH {RP2N[p]}')
            else:
                t = (b1 | (b2 << 8)) if not dyn else n16
                branch('call', None, t, 24, 24, 3, f'CALL ${b1 | (b2 << 8):04X}')
        elif z == 6:
            seq(f'{ALUF[y]}({n8});', 8, 2, f'{ALUN[y]} A,${b1:02X}')
        else:
            branch('rst', None, y * 8, 16, 16, 1, f'RST ${y * 8:02X}')
    return I
