#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""gen_tooltips.py: what the game client's ability tooltip says, for every ability of every class.

The client builds an ability's tooltip in `USkillToolTipWidget::BuildFinalTooltip` (exe 0x1469519e0) and
`USkillToolTipWidget::ProcessAbilityDescription` (0x146995490, token handler 0x146995bf0): the ability record's
`AbilityDescription` text is a template, `{...}` and `$...$` are tokens (hit, effect, linger, proj, statmod, formula,
equation, skill, target, charges, tick, duration, maxduration, flavor), and the numbers the header lines state
(cast time, charges, cost, range) come from the record's expressions, evaluated for the PLAYER.  See evidence.md.

This script reads dist/content/content.db (the design database, one gzip-JSON blob per record) and writes
server/worldd/tests/tooltip_key.h (plan E1, stage 3 fix, Part C: the answer key of test_worldd_tooltips, "the tooltip is the
truth"): for each ability of every class, the weapon combos, the general, consumable and profession abilities, what the tooltip
states for a level-LEVEL character with no gear, no skill bought and no effects: the cooldown (the data's; the skill tooltip has no
cooldown line), the charges, each cost with the number the line prints, the cast time where the data has one, the range and its
source, who it may be aimed at, the hits the text states a number for (a PERCENT: the stat mod's term x 100) and the effects the
text names.  With --out it also writes the full JSON (the template, the text, each token's source and value, the whole closure of
hits, effects, projectiles and zones; `text_all_skills` is the text with every skill-conditional part shown).  A number only the
player's stats give (a damage or heal amount) stays the expression plus its terms: the runtime evaluates it with the stat engine.

    python tools/capture/gen_tooltips.py                         # rewrite the header (about 1 s)
    python tools/capture/gen_tooltips.py --check                 # write nothing, exit 1 when the header would change
    python tools/capture/gen_tooltips.py --out tooltips.json --tsv tooltips_summary.tsv   # the research's files too
    python tools/capture/gen_tooltips.py --db <path>/content.db --level 25

It needs only Python and the content.db (the default is the repository's dist/content/content.db); nothing but the header (and the
files --out / --tsv name) is written.  Labels: [observed] read in the client's code or data, [data] the design data, [inferred] a
reading the code allows but this script could not prove.
"""
import argparse
import functools
import gzip
import json
import math
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
DEFAULT_DB = os.path.join(ROOT, 'dist', 'content', 'content.db')
HEADER = os.path.join(ROOT, 'server', 'worldd', 'tests', 'tooltip_key.h')

# ----------------------------------------------------------------------------------------------------------------
# record access

T_ABILITY, T_HIT, T_EFFECT, T_STATMOD, T_FORMULA, T_EQUATION, T_CURVE, T_TERM = (
    'AoCAbilityRecordBase', 'AbilityHitRecordBase', 'EffectRecordBase', 'StatModRecordBase',
    'StatFormulaTypeRecordBase', 'StatEquationTypeRecordBase', 'StatCurveRecordBase', 'TermRecordBase')
TYPE_SCHEMA = {
    1841048597116038327: T_ABILITY, 3672506661632452365: T_HIT, 3377240744094973029: T_EFFECT,
    676208982154769454: T_STATMOD, 4547065559311335368: T_FORMULA, 7180619218012719533: T_EQUATION,
    7037248826340794574: T_CURVE, 6137747221710071900: T_TERM, 979581256175327151: 'ProjectileRecordBase',
    7793593157135267051: 'LingeringEffectRecordBase', 3999912890578843546: 'AbilityTargetingRecordBase',
    4146438674247114715: 'SkillRecordBase', 297887639323616934: 'SkillTreeRecordBase',
    5365104778484440215: 'StatTypeDefRecordBase'}

# the stat a damage or heal hit changes (StatTypeDef "Health"); the other stats print their name
HEALTH_STAT = 108986352686712

# AoCRecordConstants' four term ids the stat-mod formatter (0x14697c280) reads: BaseValue, BaseValueMin, BaseValueMax,
# BaseValueAdd (the slot at +0x9f8, +0xa30, +0x9c0, +0xa68; the order is read from the code's min/max/add branches [inferred])
TERM_BASE, TERM_MIN, TERM_MAX, TERM_ADD = 6064631035951644675, 6064631120485154816, 6064631120485482497, 6064631210309517324


SCHEMA_TYPE = {}


class DB:
    def __init__(self, path):
        self.path = path
        self.c = sqlite3.connect('file:%s?mode=ro' % path.replace(chr(92), '/'), uri=True)
        self.cache = {}
        for t, sch in TYPE_SCHEMA.items():
            SCHEMA_TYPE[sch] = t
        # one scan: every name of the schemas the tooltips read -> guid (names are FNames: case-insensitive)
        self.names = {}
        marks = ','.join('?' * len(SCHEMA_TYPE))
        for sch, nm, g in self.c.execute('select schema,name,guid from record where schema in (%s)' % marks, list(SCHEMA_TYPE)):
            d = self.names.setdefault(sch, {})
            d.setdefault(nm, g)
            d.setdefault(nm.lower(), g)
# built with C.O.A and Cyb

    def rec(self, schema, guid):
        k = (schema, guid)
        if k not in self.cache:
            r = self.c.execute('select data,name from record where type_id=? and guid=?', (SCHEMA_TYPE[schema], guid)).fetchone()
            if r:
                j = json.loads(gzip.decompress(r[0]))
                j['__name'] = r[1]
                j['__guid'] = guid
                self.cache[k] = j
            else:
                self.cache[k] = None
        return self.cache[k]

    def ref(self, r):
        """a {Guid, TypeId} reference -> the record (None for the null reference)"""
        if not r or not r.get('Guid'):
            return None
        sch = TYPE_SCHEMA.get(r.get('TypeId'))
        return self.rec(sch, r['Guid']) if sch else None

    def by_name(self, schema, name):
        g = self.names.get(schema, {}).get(name)
        if g is None:
            g = self.names.get(schema, {}).get(name.lower())
        return self.rec(schema, g) if g is not None else None

    def stat_name(self, guid):
        r = self.rec('StatTypeDefRecordBase', guid)
        return r['__name'] if r else str(guid)

    def stat_display(self, guid):
        r = self.rec('StatTypeDefRecordBase', guid)
        if not r:
            return str(guid)
        dn = r.get('DisplayName')
        return (dn.get('Source') if isinstance(dn, dict) else dn) or r['__name']


# ----------------------------------------------------------------------------------------------------------------
# the expression language (services/internal/aocexpr): a small port, enough for the tooltip's numbers

class Unresolved(Exception):
    pass


class _Obj:
    def __init__(self, what):
        self.what = what


SELF, TARGET = _Obj('caster'), _Obj('target')
KW = ('const', 'var', 'set', 'if', 'else', 'return')
PUNCT = ['&&', '||', '==', '!=', '<=', '>=', '+', '-', '*', '/', '%', '<', '>', '!', '=', '(', ')', '{', '}', ',', ';']
BIN = {'||': 1, '&&': 2, '==': 3, '!=': 3, '<': 4, '<=': 4, '>': 4, '>=': 4, '+': 5, '-': 5, '*': 6, '/': 6, '%': 6}


def lex(src):
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c in ' \t\r\n':
            i += 1
        elif src.startswith('//', i):
            while i < n and src[i] != '\n':
                i += 1
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif c.isdigit() or (c == '.' and i + 1 < n and src[i + 1].isdigit()):
            j = i
            while j < n and src[j].isdigit():
                j += 1
            if j < n and src[j] == '.':
                j += 1
                while j < n and src[j].isdigit():
                    j += 1
            out.append(('num', float(src[i:j]), '.' in src[i:j]))
            i = j
        elif c.isalpha() or c == '_':
            j = i
            while j < n and (src[j].isalnum() or src[j] == '_'):
                j += 1
            last = i
            while j + 2 < n and src[j:j + 2] == '::' and (src[j + 2].isalpha() or src[j + 2] == '_'):
                last = j + 2
                j += 2
                while j < n and (src[j].isalnum() or src[j] == '_'):
                    j += 1
            if last != i:
                out.append(('enum', src[i:last - 2], src[last:j]))
            else:
                out.append(('id', src[i:j]))
            i = j
        elif c == '$':
            j = src.find('$', i + 1)
            body = src[i + 1:j]
            if body.startswith('#'):
                t, g = body[1:].split(':')
                out.append(('ref', int(t), int(g)))
            else:
                k, nm = body.split(':', 1)
                out.append(('nref', k, nm))
            i = j + 1
        elif c in '"\'':
            j = i + 1
            s = ''
            while src[j] != c:
                if src[j] == '\\':
                    j += 1
                s += src[j]
                j += 1
            out.append(('str', s))
            i = j + 1
        else:
            for p in PUNCT:
                if src.startswith(p, i):
                    out.append(('p', p))
                    i += len(p)
                    break
            else:
                raise Unresolved('lex %r' % c)
    out.append(('eof',))
    return out


class Parser:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self):
        return self.t[self.i]

    def nxt(self):
        t = self.t[self.i]
        self.i += 1
        return t

    def ispunct(self, s):
        t = self.peek()
        return t[0] == 'p' and t[1] == s

    def expect(self, s):
        if not self.ispunct(s):
            raise Unresolved('parse: expected %s' % s)
        self.i += 1

    def block(self, braced):
        stmts, res = [], None
        while True:
            t = self.peek()
            if t[0] == 'eof':
                return stmts, res
            if braced and self.ispunct('}'):
                self.i += 1
                return stmts, res
            if res is not None:
                raise Unresolved('parse: after final expression')
            s, e = self.statement()
            if e is not None:
                res = e
            else:
                stmts.append(s)
# [Cyb ~ C | O | A]

    def assignment(self):
        name = self.nxt()[1].lower()
        self.expect('=')
        x = self.expr(0)
        self.expect(';')
        return name, x

    def statement(self):
        t = self.peek()
        if t[0] == 'id':
            if t[1] in ('const', 'var'):
                self.i += 1
                n, x = self.assignment()
                return ('decl', n, x), None
            if t[1] == 'set':
                self.i += 1
                n, x = self.assignment()
                return ('set', n, x), None
            if t[1] == 'if':
                return self.ifstmt(), None
            if t[1] == 'return':
                self.i += 1
                x = self.expr(0)
                if self.ispunct(';'):
                    self.i += 1
                return None, x
        x = self.expr(0)
        if self.ispunct(';'):
            self.i += 1
            return ('expr', x), None
        return None, x

    def ifstmt(self):
        self.i += 1
        self.expect('(')
        cond = self.expr(0)
        self.expect(')')
        self.expect('{')
        then, r = self.block(True)
        els = []
        t = self.peek()
        if t[0] == 'id' and t[1] == 'else':
            self.i += 1
            t = self.peek()
            if t[0] == 'id' and t[1] == 'if':
                els = [self.ifstmt()]
            else:
                self.expect('{')
                els, r = self.block(True)
        return ('if', cond, then, els)

    def expr(self, mn):
        left = self.unary()
        while self.peek()[0] == 'p' and self.peek()[1] in BIN and BIN[self.peek()[1]] > mn:
            op = self.nxt()[1]
            right = self.expr(BIN[op])
            left = ('bin', op, left, right)
        return left

    def unary(self):
        if self.peek()[0] == 'p' and self.peek()[1] in ('-', '!'):
            op = self.nxt()[1]
            return ('un', op, self.unary())
        return self.primary()

    def primary(self):
        t = self.nxt()
        k = t[0]
        if k == 'num':
            return ('num', t[1])
        if k == 'ref':
            return ('ref', t[1], t[2])
        if k == 'nref':
            return ('nref', t[1], t[2])
        if k == 'str':
            return ('str', t[1])
        if k == 'enum':
            return ('enum', t[1] + '::' + t[2])
        if k == 'id':
            if t[1] in ('true', 'false'):
                return ('bool', t[1] == 'true')
            if not self.ispunct('('):
                return ('var', t[1].lower())
            self.i += 1
            args = []
            while not self.ispunct(')'):
                if args:
                    self.expect(',')
                args.append(self.expr(0))
            self.i += 1
            return ('call', t[1].lower(), args)
        if k == 'p' and t[1] == '(':
            x = self.expr(0)
            self.expect(')')
            return x
        raise Unresolved('parse: unexpected %r' % (t,))
# built with C-O-A and Cyb


class Env:
    """what an expression sees: the level, the skills bought, no effects, no stats; `terms` are the GetTerm values"""

    def __init__(self, db, level=25, skills='none', terms=None):
        self.db, self.level, self.skills, self.terms = db, level, skills, terms or {}
        self.depth = 0


def _num(v):
    if isinstance(v, bool):
        return 1.0 if v else 0.0
    if isinstance(v, (int, float)):
        return float(v)
    raise Unresolved('not a number: %r' % (v,))


def _trunc(v):
    return float(math.trunc(v))


class Interp:
    def __init__(self, env):
        self.env = env

    def run(self, src):
        stmts, res = Parser(lex(src)).block(False)
        if res is None and stmts and stmts[-1][0] == 'expr':
            res = stmts.pop()[1]
        if res is None:
            raise Unresolved('no value')
        sc = [{}]
        self.stmts(stmts, sc)
        return self.ev(res, sc)

    def stmts(self, lst, sc):
        for s in lst:
            if s[0] == 'decl':
                sc[-1][s[1]] = self.ev(s[2], sc)
            elif s[0] == 'set':
                for d in reversed(sc):
                    if s[1] in d:
                        d[s[1]] = self.ev(s[2], sc)
                        break
            elif s[0] == 'if':
                c = self.ev(s[1], sc)
                self.stmts(s[2] if c else s[3], sc + [{}])
            elif s[0] == 'expr':
                self.ev(s[1], sc)
# [Cyb - C O A]

    def ev(self, e, sc):
        k = e[0]
        if k == 'num':
            return e[1]
        if k == 'bool' or k == 'str' or k == 'enum':
            return e[1]
        if k == 'ref':
            return ('ref', e[1], e[2])
        if k == 'nref':
            return ('nref', e[1], e[2])
        if k == 'var':
            for d in reversed(sc):
                if e[1] in d:
                    return d[e[1]]
            raise Unresolved('variable ' + e[1])
        if k == 'un':
            x = self.ev(e[2], sc)
            return -_num(x) if e[1] == '-' else (not x)
        if k == 'bin':
            op = e[1]
            if op == '&&':
                return bool(self.ev(e[2], sc)) and bool(self.ev(e[3], sc))
            if op == '||':
                return bool(self.ev(e[2], sc)) or bool(self.ev(e[3], sc))
            a, b = self.ev(e[2], sc), self.ev(e[3], sc)
            if op == '==':
                return a == b
            if op == '!=':
                return a != b
            a, b = _num(a), _num(b)
            if op == '<':
                return a < b
            if op == '<=':
                return a <= b
            if op == '>':
                return a > b
            if op == '>=':
                return a >= b
            if op == '+':
                return a + b
            if op == '-':
                return a - b
            if op == '*':
                return a * b
            if op == '/':
                return a / b if b else 0.0
            return math.fmod(a, b) if b else 0.0
        if k == 'call':
            return self.call(e[1], [self.ev(a, sc) for a in e[2]], e[2], sc)
        raise Unresolved('expr ' + k)

    def call(self, name, a, raw, sc):
        env = self.env
        if name in ('float', 'int'):
            v = _num(a[0])
            return v if name == 'float' else _trunc(v)
        if name == 'round':
            return float(math.floor(_num(a[0]) + 0.5))
        if name in ('abs',):
            return abs(_num(a[0]))
        if name == 'max':
            return max(_num(x) for x in a)
        if name == 'min':
            return min(_num(x) for x in a)
        if name == 'clamp':
            return min(max(_num(a[0]), _num(a[1])), _num(a[2]))
        if name == 'lerp':
            return _num(a[0]) + (_num(a[1]) - _num(a[0])) * _num(a[2])
        if name in ('selectfloat', 'selectint', 'select'):
            return a[1] if a[0] else a[2]
        if name in ('getinstigator', 'getowner', 'getsource', 'getplayer', 'getself', 'getcreatureowner'):
            return SELF
        if name in ('gettarget', 'getactivetarget'):
            return TARGET
        if name in ('gethitinprogress', 'gethitrecord', 'gethitsourcerecord'):
            return None            # a tooltip is built with no hit in progress [inferred]
        if name == 'getabilityprogressratio':
            return 0.0             # no ability is in progress while a tooltip is read [inferred]
        if name == 'hithastag':
            return False
        if name == 'getrecordtypeid':
            return 0.0
        if name == 'getprojectiletypeid':
            return 1.0
        if name == 'getcurrentweaponskilltree':
            return ('ref', 297887639323616934, 0)   # the equipped weapon's tree: only GetSkillPurchased reads it
        if name in ('getcharacterlevel', 'getplayerlevel'):
            return float(env.level)
        if name == 'getskillpurchased':
            if env.skills == 'all':
                return True
            if env.skills == 'none':
                return False
            return a[2][2] in env.skills
        if name in ('geteffectcount', 'geteffectcountbytag', 'geteffectcountbyrecordandinstigator'):
            return 0.0
        if name == 'getterm':
            g = a[0][2]
            if g in env.terms:
                return env.terms[g]
            raise Unresolved('term %d not given' % g)
        if name in ('evalformula', 'evalequation'):
            return eval_formula(env, a[0])
        if name == 'evalcurve':
            return eval_curve(env.db, a[0], _num(a[1]))
        if name == 'getstat':
            raise Unresolved('GetStat(%s, %s)' % ('caster' if a[0] is SELF else 'target', env.db.stat_name(a[1][2]) if isinstance(a[1], tuple) else a[1]))
        raise Unresolved('function ' + name)


def eval_curve(db, ref, x):
    r = db.rec(T_CURVE, ref[2])
    if r is None:
        raise Unresolved('curve %d' % ref[2])
    keys = r['Curve']['EditorCurveData']['Keys']
    if not keys:
        return float(r['Curve']['EditorCurveData']['DefaultValue'])
    if x <= keys[0]['Time']:
        return float(keys[0]['Value'])
    for i in range(1, len(keys)):
        if x <= keys[i]['Time']:
            a, b = keys[i - 1], keys[i]
            t = (x - a['Time']) / (b['Time'] - a['Time']) if b['Time'] != a['Time'] else 0
            return float(a['Value'] + (b['Value'] - a['Value']) * t)
    return float(keys[-1]['Value'])


def eval_formula(env, ref):
    """EvalFormula($#type:guid$): the formula's equation over its InputTerms and the caller's terms"""
    if ref[0] != 'ref':
        raise Unresolved('formula ref')
    f = env.db.rec(T_FORMULA, ref[2])
    if f is None:
        raise Unresolved('formula %d' % ref[2])
    if env.depth > 12:
        raise Unresolved('formula depth')
    eq = env.db.ref(f['EquationId'])
    sub = Env(env.db, env.level, env.skills, dict(env.terms))
    sub.depth = env.depth + 1
    for t in f['InputTerms']:  # the formula's own terms (lazily resolved: an unresolvable term only fails if read)
        g = t['TermId']['Guid']
        if g not in sub.terms:
            sub.terms[g] = LazyTerm(sub, t['Value'])
    return run_expr(sub, eq['Equation'], lazy=True)
# Cyb + C.O.A


class LazyTerm:
    def __init__(self, env, src):
        self.env, self.src = env, src


def run_expr(env, src, lazy=False):
    if lazy:
        terms = {}
        for g, v in env.terms.items():
            terms[g] = v

        class E2(Env):
            pass
        e2 = E2(env.db, env.level, env.skills, terms)
        e2.depth = env.depth

        def resolve(g):
            v = terms[g]
            if isinstance(v, LazyTerm):
                v = Interp(Env(v.env.db, v.env.level, v.env.skills, {k: x for k, x in terms.items() if k != g})).run(v.src)
                terms[g] = v
            return v
        it = Interp(e2)
        orig = it.call

        def call(name, a, raw, sc):
            if name == 'getterm':
                g = a[0][2]
                if g in terms:
                    return resolve(g)
                raise Unresolved('term %d not given' % g)
            return orig(name, a, raw, sc)
        it.call = call
        return it.run(src)
    return Interp(env).run(src)


def try_eval(db, src, level, skills='none', terms=None):
    """-> (value, None) or (None, reason); an empty text is no expression"""
    if src is None or str(src).strip() == '':
        return None, 'empty'
    try:
        v = run_expr(Env(db, level, skills, terms or {}), src, lazy=True)
        if isinstance(v, bool):
            v = 1.0 if v else 0.0
        if not isinstance(v, float):
            return None, 'not a number'
        return float(v), None
    except Unresolved as e:
        return None, str(e)
    except (RecursionError, ZeroDivisionError, KeyError, IndexError, ValueError) as e:
        return None, '%s: %s' % (type(e).__name__, e)


# ----------------------------------------------------------------------------------------------------------------
# number and time formats of the client

def fmt_num(v, maxfrac=0):
    """FText::AsNumber with MinimumFractionalDigits 0, MaximumFractionalDigits maxfrac, grouping on, half to even [observed
    0x1469512a0: FNumberFormattingOptions(local_48): UseGrouping, MinIntegral 1, MaxIntegral 324, MinFrac 0, MaxFrac = 0, 1 ("F"), 2 ("FF")]"""
    q = round(v, maxfrac)  # python rounds half to even
    s = '{:,.{}f}'.format(q, maxfrac)
    if '.' in s:
        s = s.rstrip('0').rstrip('.')
    return '-0' if s == '0' and v < 0 and False else s


def fmt_seconds(v):
    """0x14697f160: under 60 s "N seconds" (one decimal at most), else minutes; 1 reads "1 second" [observed template swap on <= 1]"""
    if v < 60:
        s = fmt_num(v, 1)
        return '%s second%s' % (s, '' if s == '1' else 's')
    m = v / 60.0
    s = fmt_num(m, 1)
    return '%s minute%s' % (s, '' if s == '1' else 's')


def cost_display(x):
    """BuildFinalTooltip's cost line: -(round(-0.5 - 2x) >> 1) with round to nearest even [observed 0x1469519e0]"""
    r = round(-0.5 - 2.0 * x)
    return -(r >> 1)


def charges_display(x):
    """"%d charges", (int)ROUND(2x + 0.5) >> 1 (half up) [observed 0x146995bf0 / 0x146ba5d80 shown when > 1]"""
    return int(round(2.0 * x + 0.5)) >> 1


# ----------------------------------------------------------------------------------------------------------------
# the template
# [C O A + Cyb]

def split_template(text):
    """ProcessAbilityDescription's splitter (0x146966430): '$' toggles, '{' and '}' nest; the result alternates
    literal, token, literal ... [observed]"""
    segs, depth, lit0, tok0 = [], 0, 0, 0
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '$':
            if depth == 0:
                segs.append(text[lit0:i])
                tok0 = i + 1
                depth = 1
            else:
                depth -= 1
                if depth == 0:
                    lit0 = i + 1
                    segs.append(text[tok0:i])
        elif c == '{':
            if depth < 1:
                segs.append(text[lit0:i])
                tok0 = i + 1
            depth += 1
        elif c == '}':
            if depth > 0:
                depth -= 1
                if depth == 0:
                    lit0 = i + 1
                    segs.append(text[tok0:i])
        i += 1
    segs.append(text[lit0:n])
    return segs


def num_after(s, key):
    """FUN_14696e3b0: the digits right after `key` in s, -1 if none"""
    i = s.lower().find(key.lower())
    if i < 0:
        return -1
    j = i + len(key)
    k = j
    while k < len(s) and s[k].isdigit():
        k += 1
    return int(s[j:k]) if k > j else -1


def keymap(lst):
    return {x['Key']: x['Value'] for x in lst}


class Renderer:
    def __init__(self, db, level=25, skills='none'):
        self.db, self.level, self.skills = db, level, skills
        self.tokens = []
        self.cond = []   # skill-conditional parts shown or skipped
        self.unres = []

    def env(self, terms=None):
        return Env(self.db, self.level, self.skills, terms or {})

    # --- numbers -----------------------------------------------------------------------------------------------
    def ev(self, src, terms=None):
        return try_eval(self.db, src, self.level, self.skills, terms)

    def term_values(self, sm):
        out = {}
        for t in sm.get('ValueInputTerms', []):
            out[t['TermId']['Guid']] = LazyTerm(self.env(), t['Value'])
        return out

    def statmod_value(self, sm, flags, tok):
        """0x14697c280: the number a stat mod shows. onlystat: the stat's name. minmax / max / min / add: the BaseValueMin /
        BaseValueMax / BaseValueAdd term; otherwise the BaseValue term when it is not 0, else the stat mod's whole Value
        (evaluated with the player as the caster). Flags: %: x100 and a percent sign; -1: minus 1; abs; F and FF: one or two
        decimals; nostat: no stat name [observed]; which slot is which term [inferred]"""
        f = flags.lower()
        stat = self.db.stat_display(sm['StatRefId']['Guid'])
        if 'onlystat' in f:
            tok['value_kind'] = 'stat name'
            return stat
        terms = {t['TermId']['Guid']: t['Value'] for t in sm.get('ValueInputTerms', [])}

        def term(g):
            if g not in terms:
                return 0.0, None
            v, why = self.ev(terms[g])
            if v is None:
                return None, why
            return v, None
        if 'minmax' in f:
            lo, w1 = term(TERM_MIN)
            hi, w2 = term(TERM_MAX)
            vals, why = [lo, hi], w1 or w2
            kind = 'BaseValueMin-BaseValueMax'
        else:
            slot, kind = ((TERM_MAX, 'BaseValueMax') if 'max' in f else (TERM_MIN, 'BaseValueMin') if 'min' in f
                          else (TERM_ADD, 'BaseValueAdd') if 'add' in f else (TERM_BASE, 'BaseValue'))
            v, why = term(slot)
            if v == 0.0 and slot == TERM_BASE:
                kind = 'Value (the whole stat mod expression, evaluated for the player)'
                v, why = self.ev(sm['Value'], self.term_values(sm))
            vals = [v]
        tok['value_kind'] = kind
        tok['value_expr'] = sm['Value'] if kind.startswith('Value') else {k: terms[k] for k in terms if k in (TERM_BASE, TERM_MIN, TERM_MAX, TERM_ADD)}
        if any(x is None for x in vals):
            tok['needs_stats'] = why
            self.unres.append(tok['raw'] + ': ' + str(why))
            return None
        pct = '%' in f
        out = []
        for x in vals:
            if '-1' in f:
                x -= 1.0
            if 'abs' in f:
                x = abs(x)
            if pct:
                x *= 100.0
            digits = 2 if 'ff' in f else 1 if 'f' in f.replace('ff', '') or 'f' in f else 0
            out.append(fmt_num(x, digits) + ('%' if pct else ''))
        tok['value'] = vals if len(vals) > 1 else vals[0]
        txt = '-'.join(out)
        if 'nostat' in f:
            return txt
        if 'by' in f.replace('nostat', ''):
            neg = txt.startswith('-')
            return '%s %s by %s' % (stat, 'decreased' if neg else 'increased', txt.lstrip('-'))
        return '%s %s' % (txt, stat)

    # --- hit and effect text ------------------------------------------------------------------------------------
    def hit_kind(self, h):
        tags = [t['TagName'] for t in h.get('EventTags', [])]
        for t in tags:
            if t.startswith('Element.Healing'):
                return 'healing', tags
        for t in tags:
            if t.startswith('Element.Physical'):
                return 'physical', tags
            if t.startswith('Element.Magical'):
                return 'magical', tags
        return '', tags
# ~ CoA :: C Y B ~

    def hit_text(self, h, suffix, ab, tok):
        """0x14695f880: a hit's text: statmodN / applyN / applyselfN / applyNinline / dur / for ... else its first stat mod's
        value with the damage type"""
        s = suffix or ''
        low = s.lower()
        tok['hit'] = h['__name']
        if low.startswith('statmod'):
            n = num_after(s, 'statmod')
            sms = h.get('StatModsIds', [])
            if 0 <= n < len(sms):
                sm = self.db.ref(sms[n])
                flags = s[len('statmod') + len(str(n)):]
                txt = self.statmod_value(sm, flags, tok)
                return txt if txt is not None else '?'
            return '<ParseFail>hit statmod index error</>'
        if low.startswith('apply') or low.startswith('hideapply') or low.startswith('applyself'):
            selfmode = 'applyself' in low
            key = 'applyself' if selfmode else 'apply'
            n = num_after(s, key)
            lst = h.get('EffectsAppliedToSelf' if selfmode else 'ApplyEffects', [])
            if not (0 <= n < len(lst)):
                return ''
            ap = lst[n]
            eff = self.db.ref(ap.get('EffectId') or ap.get('Effect'))
            inline = 'inline' in low
            name = self.effect_name(eff)
            tok.setdefault('applies', []).append({'effect': eff['__name'], 'name': name})
            out = name
            if inline:
                out = '%s: %s' % (name, self.effect_desc(eff, ab))
            if 'hide' in low:
                return ''
            if 'dur' in low and 'for' not in low.replace('fordur', 'fordur'):
                pass
            if 'dur' in low or 'for' in low:
                d, why = self.ev(ap.get('EffectDuration', ''))
                if not d or d <= 0:
                    d, why = self.ev(eff.get('EffectDuration', ''))
                if d and not eff.get('bInfiniteDuration'):
                    tok.setdefault('durations', []).append(d)
                    out = (out + ' for ' + fmt_seconds(d)) if 'for' in low else (fmt_seconds(d))
            st, why = self.ev(ap.get('StackCount', ''))
            if st and st >= 1.5:
                out = '%d stacks of %s' % (int(st), out)
            return out
        # default: the first stat mod, "%nostat", then the damage type (0x1469624c0)
        sms = h.get('StatModsIds', [])
        if not sms:
            return ''
        sm = self.db.ref(sms[0])
        txt = self.statmod_value(sm, s + '%nostat', tok)
        kind, tags = self.hit_kind(h)
        tok['damage_kind'] = kind or 'other'
        tok['event_tags'] = tags
        leaf = tags[0].split('.')[-1] if tags and kind and tags[0].count('.') > 1 else ''
        label = leaf or {'healing': 'Healing', 'physical': 'Physical', 'magical': 'Magical'}.get(kind, '')
        tok['label'] = label
        if txt is not None:
            return txt + (' ' + label if label else '')
        if txt is None:
            coef, _ = self.ev(dict((t['TermId']['Guid'], t['Value']) for t in sm.get('ValueInputTerms', [])).get(TERM_BASE, ''))
            lab = {'healing': 'healing', 'physical': 'physical damage', 'magical': 'magical damage'}.get(kind, 'effect')
            leaf = tags[0].split('.')[-1] if tags and kind and tags[0].count('.') > 1 else ''
            if coef is not None:
                tok['coefficient'] = coef
                return '[%sx %s%s]' % (fmt_num(coef, 3), (leaf + ' ') if leaf else '', lab)
            return '[%s%s]' % ((leaf + ' ') if leaf else '', lab)
        return txt

    def effect_name(self, eff):
        n = eff.get('EffectName', {}).get('Source') or eff['__name']
        return n

    def effect_desc(self, eff, ab):
        src = eff.get('EffectDescription', {}).get('Source', '')
        r = Renderer(self.db, self.level, self.skills)
        r.frame = eff
        r.tokens = self.tokens
        r.unres = self.unres
        r.cond = self.cond
        return r.render(src, ab, eff)

    # --- tokens -------------------------------------------------------------------------------------------------
    def token(self, raw, ab, frame):
        t = raw
        tok = {'raw': raw}
        head, _, rest = t.partition(':')
        low = head.lower()
        out = None
        if low.startswith('!skill') or low.startswith('skill'):
            neg = low.startswith('!')
            parts = t.split(':', 3)
            if len(parts) < 4:
                return raw
            _, cls, skill, body = parts
            has = (self.skills == 'all')
            tok.update({'kind': 'skill', 'skill': skill, 'class': cls, 'negated': neg, 'shown': has != neg})
            self.cond.append({'skill': skill, 'negated': neg, 'shown': has != neg})
            self.tokens.append(tok)
            if has != neg:
                return self.render(body, ab, frame)
            return ''
        if low.startswith('formula') or low.startswith('!formula'):
            neg = low.startswith('!')
            m = re.match(r'!?formula:([^:.]+)(?:\.([^:]*))?(?::(.*))?$', t, re.S)
            if not m:
                return raw
            name, suf, body = m.group(1), m.group(2) or '', m.group(3)
            f = self.db.by_name(T_FORMULA, name)
            v = None
            why = 'no formula ' + name
            if f:
                v, why = self.ev('EvalFormula($#4547065559311335368:%d$)' % f['__guid'])
            tok.update({'kind': 'formula', 'formula': name, 'value': v, 'negated': neg})
            self.tokens.append(tok)
            if body is None:  # {formula:Name.%}: the number
                if v is None:
                    tok['needs_stats'] = why
                    self.unres.append(raw + ': ' + str(why))
                    return '?'
                pct = '%' in suf
                return fmt_num(v * 100 if pct else v, 1 if 'F' in suf else 0) + ('%' if pct else '')
            if v is None:
                tok['needs_stats'] = why
                tok['condition'] = 'unresolved'
                self.unres.append(raw[:60] + ': ' + str(why))
                return '[if %s: %s]' % (name, self.render(body, ab, frame))
            shown = (v != 0.0) != neg
            tok['shown'] = shown
            return self.render(body, ab, frame) if shown else ''
        if low == 'charges' and not rest:
            v, why = self.ev(ab.get('CooldownCharges', ''))
            if v is None:
                v = 1.0 if why == 'empty' else None
            tok.update({'kind': 'charges', 'value': v})
            self.tokens.append(tok)
            if v is None:
                self.unres.append(raw + ': ' + str(why))
                return '?'
            return '%d charges' % charges_display(v)
        if low in ('duration', 'maxduration', 'tick') and frame is not None and not rest:
            return self.frame_value(low, frame, tok)
        if low == 'flavor' or low.startswith('flavor'):
            tok['kind'] = 'flavor'
            self.tokens.append(tok)
            return '<Flavor>%s</>' % rest
        # the rest: hit, effect, linger, proj, statmod, equation, target, with an index or a name
        base = t
        name_part = None
        kind = None
        for k in ('hit', 'effect', 'linger', 'proj', 'statmod', 'equation', 'target', 'tick', 'apply'):
            if low.startswith(k):
                kind = k
                break
        if kind is None:
            tok['kind'] = 'literal'
            return raw
        tok['kind'] = kind
        self.tokens.append(tok)
        # head is "hit", "hit1", "effect2.inline"...; a '.' suffix may be in the head or after the ':'
        h_main, _, h_suf = head.partition('.')
        idx_s = h_main[len(kind):]
        idx = int(idx_s) if idx_s.isdigit() else None
        sub = rest  # after ':'
        if idx is None and not rest and '.' in head:
            sub = h_suf
        elif idx is not None and not rest:
            sub = h_suf
        elif idx is not None and rest:
            sub = rest if not h_suf else h_suf + rest
        name = None
        if idx is None and rest:
            m2 = re.match(r'([^:.]+)[:.]?(.*)$', rest, re.S)
            name, sub = m2.group(1), m2.group(2)
        try:
            return self.resolve(kind, idx, name, sub, tok, ab, frame)
        except Unresolved as e:
            tok['needs_stats'] = str(e)
            self.unres.append(raw + ': ' + str(e))
            return '?'
# ~ Cyb & C-O-A ~

    def frame_value(self, what, eff, tok):
        tok['kind'] = what
        self.tokens.append(tok)
        if what == 'tick':
            v = float(eff.get('TickTimer', 0) or 0)
        elif what == 'maxduration':
            v, why = self.ev(str(eff.get('ExtendedMaximum', '')))
            v = v or 0.0
        else:
            v, why = self.ev(eff.get('EffectDuration', ''))
            if v is None:
                self.unres.append('duration of %s: %s' % (eff['__name'], why))
                tok['needs_stats'] = why
                return '?'
        tok['value'] = v
        return fmt_seconds(v)

    def resolve(self, kind, idx, name, sub, tok, ab, frame):
        db = self.db
        sub = sub or ''
        if kind == 'hit':
            h = None
            if idx is not None:
                m = keymap(ab.get('HitsIds', [])) if frame is None or frame.get('__kind') == 'ability' else {}
                ref = m.get(idx)
                if ref is None and frame is not None:
                    pass
                h = db.ref(ref) if ref else None
            elif name:
                h = db.by_name(T_HIT, name)
            if h is None:
                tok['unresolved'] = 'no hit'
                self.unres.append(tok['raw'] + ': no such hit')
                return '?'
            return self.hit_text(h, sub, ab, tok)
        if kind == 'effect':
            eff = None
            if idx is not None:
                ref = keymap(ab.get('EffectsIds', [])).get(idx)
                eff = db.ref(ref) if ref else None
            elif name:
                eff = db.by_name(T_EFFECT, name)
            if eff is None:
                tok['unresolved'] = 'no effect'
                self.unres.append(tok['raw'] + ': no such effect')
                return '?'
            tok['effect'] = eff['__name']
            low = sub.lower()
            nm = self.effect_name(eff)
            if 'description' in low:
                return self.effect_desc(eff, ab)
            if 'inline' in low:
                return '%s: %s' % (nm, self.effect_desc(eff, ab))
            if 'hide' in low:
                return nm
            if low.startswith('statmod') or '.statmod' in low:
                pass
            return nm
        if kind in ('linger', 'proj'):
            if kind == 'linger':
                ref = keymap(ab.get('LingeringEffectsIds', [])).get(idx) if idx is not None else None
                z = db.ref(ref) if ref else (db.by_name('LingeringEffectRecordBase', name) if name else None)
            else:
                ref = keymap(ab.get('ProjectilesIds', [])).get(idx) if idx is not None else None
                z = db.ref(ref) if ref else None
            if z is None:
                tok['unresolved'] = 'no ' + kind
                self.unres.append(tok['raw'] + ': no such ' + kind)
                return '?'
            tok[kind] = z['__name']
            s2 = sub.lower()
            if kind == 'linger':
                if s2 == 'duration' or s2.startswith('duration'):
                    tok['value'] = z['LifeTime']
                    return fmt_seconds(float(z['LifeTime']))
                if s2 == 'tick' or s2.startswith('tick'):
                    tok['value'] = z['TickRate']
                    return fmt_seconds(float(z['TickRate']))
                m = re.match(r'(init|linger|beginoverlap|endoverlap|endhit)(\d+)(?:\.(.*))?', sub, re.I)
                if m:
                    key = {'init': 'InitialHitsIds', 'linger': 'LingeringHitsIds', 'beginoverlap': 'OnBeginOverlapHitsIds',
                           'endoverlap': 'OnEndOverlapHitsIds', 'endhit': 'EndHitsIds'}[m.group(1).lower()]
                    lst = z.get(key, [])
                    n = int(m.group(2))
                    if n < len(lst):
                        return self.hit_text(db.ref(lst[n]), m.group(3) or '', ab, tok)
                return ''
            m = re.match(r'hit(\d+)\.?(.*)$', sub, re.S)
            lst = z.get('HitsIds', [])
            if m and int(m.group(1)) < len(lst):   # the projectile's hits are an array: hit0 is the first [observed 0x146961030]
                return self.hit_text(db.ref(lst[int(m.group(1))]), m.group(2), ab, tok)
            tok['unresolved'] = 'no projectile hit'
            self.unres.append(tok['raw'] + ': no such projectile hit')
            return '?'
        if kind in ('statmod', 'tick', 'apply') and frame is not None:
            n = idx if idx is not None else 0
            if kind == 'statmod':
                sms = frame.get('StatModsIds', [])
                if n < len(sms):
                    flags = sub if sub else ''
                    txt = self.statmod_value(db.ref(sms[n]), flags, tok)
                    return txt if txt is not None else '?'
            else:
                key = {'tick': 'TickHitsIds', 'apply': 'ApplyHitsIds'}[kind]
                lst = frame.get(key, [])
                if n < len(lst):
                    return self.hit_text(db.ref(lst[n]), sub, ab, tok)
            return ''
        if kind == 'target':
            tok['needs_stats'] = 'target radius'
            return '?'
        if kind == 'equation':
            tok['needs_stats'] = 'equation'
            return '?'
        return '?'

    def render(self, text, ab, frame=None):
        out = []
        segs = split_template(text)
        for i, s in enumerate(segs):
            if i % 2 == 0:
                out.append(s)
            else:
                out.append(self.token(s, ab, frame))
        return ''.join(out)


def plain(s):
    """the visible words of a rich text: tags dropped, <img id=X/> as [X]"""
    s = re.sub(r'<img id="([^"]+)"\s*/>', r'[\1]', s)
    s = re.sub(r'<a [^>]*>', '', s)
    s = re.sub(r'</>', '', s)
    s = re.sub(r'<(?:Highlight|Bold|Flavor|Physical|Magical|Healing|NodeBuffStat|TooltipRegular|Red)>', '', s)
    s = s.replace('\r\n', '\n')
    return s


# ----------------------------------------------------------------------------------------------------------------
# the header facts

TARGET_BITS = [(1, 'Self'), (2, 'Enemy'), (4, 'Ally'), (8, 'Party'), (16, 'Raid')]


def target_names(mask):
    return [n for b, n in TARGET_BITS if mask & b]


def hit_facts(db, h, level, skills):
    sm = db.ref(h['StatModsIds'][0]) if h.get('StatModsIds') else None
    f = {'name': h['__name'], 'applicable_targets': target_names(h.get('ApplicableTargets', 0)),
         'applicable_mask': h.get('ApplicableTargets', 0),
         'fallback': h.get('DefaultFallbackTargetType', '').split('::')[-1],
         'net_type': h.get('NetworkType', '').split('::')[-1],
         'event_tags': [t['TagName'] for t in h.get('EventTags', [])]}
    tags = f['event_tags']
    f['kind'] = ('healing' if any(t.startswith('Element.Healing') for t in tags) else
                 'physical damage' if any(t.startswith('Element.Physical') for t in tags) else
                 'magical damage' if any(t.startswith('Element.Magical') for t in tags) else
                 '' if sm is None else 'stat change')
    if sm is not None:
        f['stat'] = db.stat_name(sm['StatRefId']['Guid'])
        f['stat_mod_value'] = sm['Value']
        terms = {}
        for t in sm.get('ValueInputTerms', []):
            g = t['TermId']['Guid']
            nm = db.rec(T_TERM, g)['__name'] if db.rec(T_TERM, g) else str(g)
            v, why = try_eval(db, t['Value'], level, skills)
            terms[nm] = {'expr': t['Value'], 'value': v, 'unresolved': why if v is None else None}
        f['terms'] = terms
        # what the tooltip states: the term x 100 and a percent sign ("%nostat" holds '%'); the amount the hit really does
        # is GlobalMod * (BaseValue * Damage + BaseValueAdd) * ... (the stat mod's formula) and needs the stat engine
        shown = {}
        for nm, key in (('BaseValue', 'percent'), ('BaseValueMin', 'min_percent'), ('BaseValueMax', 'max_percent')):
            t = terms.get(nm)
            if t and t['value'] is not None:
                shown[key] = round(t['value'] * 100.0, 3)
        if 'BaseValueAdd' in terms and terms['BaseValueAdd']['value'] is not None:
            shown['add_flat'] = terms['BaseValueAdd']['value']
        f['tooltip_shows'] = shown
        f['absolute_amount'] = 'needs the stat engine (the stat mod formula over the caster stats)'
    f['applies'] = []
    for ap in h.get('ApplyEffects', []):
        e = db.ref(ap['EffectId'])
        if e:
            d, why = try_eval(db, ap.get('EffectDuration', ''), level, skills)
            ed, why2 = try_eval(db, e.get('EffectDuration', ''), level, skills)
            f['applies'].append({'effect': e['__name'], 'duration_s': d if d else ('infinite' if e.get('bInfiniteDuration') else ed),
                                 'stacks': try_eval(db, ap.get('StackCount', ''), level, skills)[0]})
    f['applies_to_self'] = [db.ref(x.get('Effect'))['__name'] for x in h.get('EffectsAppliedToSelf', []) if db.ref(x.get('Effect'))]
    return f


def effect_facts(db, e, level, skills):
    d, why = try_eval(db, e.get('EffectDuration', ''), level, skills)
    return {'name': e['__name'], 'display_name': e.get('EffectName', {}).get('Source', ''),
            'infinite': bool(e.get('bInfiniteDuration')), 'duration_s': d, 'duration_expr': e.get('EffectDuration', ''),
            'tick_s': e.get('TickTimer', 0), 'max_duration_s': try_eval(db, str(e.get('ExtendedMaximum', '')), level, skills)[0],
            'stack_limit': e.get('InPlaceStackLimit'), 'dispellable': bool(e.get('bDispellable')),
            'description': e.get('EffectDescription', {}).get('Source', '')}
# ~ C | Y | B :: CoA ~


def facts(db, ab, level, skills):
    f = {}
    # cooldown: no line in the skill tooltip (the consumables' text has $cd$, an item tooltip token); the design value
    cd = {}
    for sk in ('none', 'all'):
        v, why = try_eval(db, ab.get('Cooldown', ''), level, sk)
        cd[sk] = v if v is not None else (0.0 if why == 'empty' else 'unresolved: %s' % why)
    f['cooldown_s'] = {'expr': ab.get('Cooldown', ''), 'by_skills': cd, 'in_tooltip': False,
                       'shared_tag': ab.get('SharedCooldownTag', ''), 'disable_during_cooldown': bool(ab.get('bDisableDuringCooldown'))}
    ch = {}
    for sk in ('none', 'all'):
        v, why = try_eval(db, ab.get('CooldownCharges', ''), level, sk)
        ch[sk] = (charges_display(v) if v is not None else (1 if why == 'empty' else 'unresolved: %s' % why))
    f['charges'] = {'expr': ab.get('CooldownCharges', ''), 'by_skills': ch,
                    'shown_when_over_1': True}
    costs = []
    for g in ab.get('Costs', []):
        for sc in g.get('StatCosts', []):
            row = {'stat': db.stat_name(sc['Stat']['Guid']), 'expr': sc['Value'], 'enabled_expr': g.get('Enabled', ''),
                   'charge_cost_expr': g.get('ChargeCost', ''), 'triggers_cooldown': bool(g.get('bTriggersCooldown'))}
            for sk in ('none', 'all'):
                v, why = try_eval(db, sc['Value'], level, sk)
                if v is None and why == 'empty':
                    v = 0.0   # no expression: nothing is spent
                row['value_' + sk] = v
                if v is not None:
                    row['shown_' + sk] = cost_display(v) if cost_display(v) > 0 else None
                else:
                    row['unresolved'] = why
            costs.append(row)
        for ec in g.get('EffectCosts', []):
            costs.append({'effect_cost': ec.get('EffectFilter', ec), 'enabled_expr': g.get('Enabled', '')})
    f['cost'] = costs
    dc = ab.get('DynamicCastDuration', '')
    v, why = try_eval(db, dc, level, 'none')
    f['cast'] = {'dynamic_expr': dc, 'dynamic_s': v,
                 'montage': ((ab.get('AnimMontages') or [{}])[0].get('AbilityAnimationSoftPaths') or [''])[0],
                 'note': 'the cast time line is the montage notify time over the cast speed (UUILibrary::SetAbilityCastTime, 0x1469bf..): not in the design data' if not dc else 'DynamicCastDuration'}
    f['held'] = {'behavior': ab.get('HeldAbilityBehavior', '').split('::')[-1], 'max_hold_s': ab.get('MaximumHoldTime', 0),
                 'bard_music': 'bard_music' in json.dumps(ab.get('AbilityTags', '')).lower()}
    # range (0x146ba4110): CustomTooltipRange, else AreaTargetRange for a clicked AoE, else the targeting record's, else ValidDistanceExp
    if ab.get('bOverrideRangeInTooltip'):
        r, why = try_eval(db, ab.get('CustomTooltipRange', ''), level, 'none')
        src = 'CustomTooltipRange'
    elif ab.get('bIsAOEClickedAbility'):
        r, why, src = float(ab.get('AreaTargetRange', 0)), None, 'AreaTargetRange'
    elif ab.get('bUseTargetingRecord'):
        t = db.ref(ab.get('TargetingRecordId') or {})
        r = float(t['LockOnInfo']['ValidDistance']) if t else None
        why, src = None, 'targeting record ValidDistance'
    elif ab.get('bCheckTargetValidity') and ab.get('bPerformConeCheck'):
        r, why = try_eval(db, ab.get('ValidDistanceExp', ''), level, 'none')
        src = 'ValidDistanceExp'
    else:
        r, why, src = 0.0, None, 'none'
    f['range'] = {'source': src, 'cm': r, 'shown_m': (math.floor(r * 0.01) if r and r * 0.01 > 1e-8 else None),
                  'unresolved': why if r is None else None}
    mask = ab.get('ApplicableTargets', 0)
    f['targets'] = {'applicable': target_names(mask), 'mask': mask,
                    'requirement': ab.get('TargetActivationRequirement', '').split('::')[-1],
                    'fallback': ab.get('DefaultFallbackTargetType', '').split('::')[-1]}
    f['weapon_requirement'] = ab.get('WeaponRequirement', '').split('::')[-1]
    return f


def closure(db, ab, level, skills):
    """the hits, effects, zones and projectiles the ability reaches, with who each hit applies to (the answer key for
    "which hit goes to which target")"""
    seen, hits, effects, zones, projs = set(), [], [], [], []
    ek = {}

    def add_hit(ref, role):
        h = db.ref(ref)
        if h is None or (h['__guid'], role) in seen:
            return
        seen.add((h['__guid'], role))
        hf = hit_facts(db, h, level, skills)
        hf['role'] = role
        hits.append(hf)
        for ap in h.get('ApplyEffects', []):
            add_effect(ap['EffectId'])
        for x in h.get('EffectsAppliedToSelf', []):
            add_effect(x.get('Effect'))
# CoA / C-Y-B

    def add_effect(ref):
        e = db.ref(ref)
        if e is None or e['__guid'] in ek:
            return
        ek[e['__guid']] = 1
        effects.append(effect_facts(db, e, level, skills))
        for key, role in (('ApplyHitsIds', 'effect apply'), ('TickHitsIds', 'effect tick'), ('ExpireHitsIds', 'effect expire'),
                          ('EndHitsIds', 'effect end'), ('RemoveHitsIds', 'effect remove'), ('PreApplyHitsIds', 'effect preapply'),
                          ('ConsumeHitsIds', 'effect consume')):
            for h in e.get(key, []):
                add_hit(h, role + ' of ' + e['__name'])

    for x in ab.get('HitsIds', []):
        add_hit(x['Value'], 'ability hit %d' % x['Key'])
    for x in ab.get('EffectsIds', []):
        add_effect(x['Value'])
    for x in ab.get('ProjectilesIds', []):
        p = db.ref(x['Value'])
        if p is None:
            continue
        projs.append({'key': x['Key'], 'name': p['__name'], 'type': p['ProjectileType'].split('::')[-1], 'speed': p['Speed'],
                      'range': p['Range'], 'life_s': p['LifeTime'], 'max_hits': p['MaxProjectileHits']})
        for h in p.get('HitsIds', []):
            add_hit(h, 'projectile %d' % x['Key'])
    for x in ab.get('LingeringEffectsIds', []):
        z = db.ref(x['Value'])
        if z is None:
            continue
        zones.append({'key': x['Key'], 'name': z['__name'], 'life_s': z['LifeTime'], 'tick_s': z['TickRate'],
                      'initial_hits': len(z.get('InitialHitsIds', [])), 'lingering_hits': len(z.get('LingeringHitsIds', []))})
        for key in ('InitialHitsIds', 'LingeringHitsIds', 'OnBeginOverlapHitsIds', 'OnEndOverlapHitsIds', 'EndHitsIds'):
            for h in z.get(key, []):
                add_hit(h, 'zone %d %s' % (x['Key'], key[:-3]))
    return hits, effects, projs, zones


# ----------------------------------------------------------------------------------------------------------------
# the roster: every ability of every class

CLASSES = ['Bard', 'Cleric', 'Fighter', 'Mage', 'Ranger', 'Rogue', 'Summoner', 'Tank']


def roster(db):
    """-> {group: [(ability name, source note)]}: the 8 classes' tree abilities (+ their Stage tree), the weapons'
    combo and attack abilities, the general abilities and the consumables' abilities; the NPC kits are counted apart"""
    c = db.c
    groups = {k: {} for k in CLASSES}
    for tree, kind, an in c.execute("""select distinct t.name, t.kind, r.ability_name from skill_tree t join skill_node n on n.tree_guid=t.guid
            join skill_rank r on r.skill_guid=n.skill_guid where r.ability_guid<>0 and t.kind in ('class','other') order by 1,3"""):
        cls = tree.split('_')[0]
        if cls in groups:
            groups[cls].setdefault(an, 'class tree' if kind == 'class' else 'Stage tree')
    prof = {}
    for tree, an in c.execute("""select distinct t.name, r.ability_name from skill_tree t join skill_node n on n.tree_guid=t.guid
            join skill_rank r on r.skill_guid=n.skill_guid where r.ability_guid<>0 and t.kind='profession'"""):
        prof.setdefault('Profession', {})[an] = tree
    weapon, general, items, npc = {}, {}, {}, []
    for a in c.execute("select a.name from ab_ability a join item_weapon w on w.attack_ability_guid=a.guid group by a.guid"):
        weapon[a[0]] = 'item_weapon attack ability'
    tsv = os.path.join(os.path.dirname(os.path.abspath(db.path)), '..', '..', 'services', 'cmd', 'contentc', 'ability_names.tsv')
    cats = {}
    if os.path.exists(tsv):
        for line in open(tsv, encoding='utf-8'):
            if line.startswith('#') or not line.strip():
                continue
            nm, cat = line.rstrip('\r\n').split('\t')
            cats[nm] = cat
    for nm, cat in cats.items():
        if cat == 'weapon_combo':
            weapon[nm] = 'ability_names.tsv weapon_combo'
        elif cat == 'general':
            general[nm] = 'ability_names.tsv general'
        elif cat == 'item':
            items[nm] = 'ability_names.tsv item'
        elif cat == 'npc':
            npc.append(nm)
    return groups, prof.get('Profession', {}), weapon, general, items, npc


def build(db, level):
    groups, prof, weapon, general, items, npc = roster(db)
    out = {'level': level, 'character': 'level %d, no gear, no skill bought (text) / every skill bought (text_all_skills), no effects' % level,
           'groups': {}}

    def entry(name, note):
        ab = db.by_name(T_ABILITY, name)
        if ab is None:
            return {'ability': name, 'source': note, 'error': 'no AoCAbilityRecordBase named so'}
        ab['__kind'] = 'ability'
        desc = ab.get('AbilityDescription', {}).get('Source', '')
        e = {'ability': name, 'guid': ab['__guid'], 'display_name': ab.get('AbilityName', {}).get('Source', ''), 'source': note,
             'template': desc}
        for variant, sk in (('text', 'none'), ('text_all_skills', 'all')):
            r = Renderer(db, level, sk)
            r.frame = None
            txt = r.render(desc, ab, None) if desc else ''
            e[variant] = plain(txt).strip()
            if sk == 'none':
                e['tokens'] = r.tokens
                e['unresolved'] = sorted(set(r.unres))
                e['conditional_parts'] = r.cond
            else:
                e['unresolved_all_skills'] = sorted(set(r.unres))
        if e['text'] == e['text_all_skills']:
            del e['text_all_skills']
        if not e['unresolved_all_skills'] or e['unresolved_all_skills'] == e['unresolved']:
            del e['unresolved_all_skills']
        e.update(facts(db, ab, level, 'none'))
        hits, effects, projs, zones = closure(db, ab, level, 'none')
        e['hits'], e['effects'], e['projectiles'], e['zones'] = hits, effects, projs, zones
        # who the ability's own hits (not its effects' ticks) land on: the union of their ApplicableTargets
        own = [h for h in hits if h['role'].startswith(('ability hit', 'projectile', 'zone'))]
        e['targets']['hits_union'] = target_names(functools.reduce(lambda a, b: a | b, (h['applicable_mask'] for h in own), 0))
        return e
# built with CoA and C.Y.B

    for cls in CLASSES:
        out['groups'][cls] = [entry(n, s) for n, s in sorted(groups[cls].items())]
    out['groups']['Weapons'] = [entry(n, s) for n, s in sorted(weapon.items())]
    out['groups']['General'] = [entry(n, s) for n, s in sorted(general.items())]
    out['groups']['Consumables'] = [entry(n, s) for n, s in sorted(items.items())]
    out['groups']['Professions'] = [entry(n, s) for n, s in sorted(prof.items())]
    nd = 0
    for n in npc:
        ab = db.by_name(T_ABILITY, n)
        if ab and ab.get('AbilityDescription', {}).get('Source'):
            nd += 1
    out['npc_abilities'] = {'count': len(npc), 'with_description': nd,
                            'note': 'NPC abilities are not player tooltips; counted only'}
    return out


# ----------------------------------------------------------------------------------------------------------------
# the test header (server/worldd/tests/tooltip_key.h): what test_worldd_tooltips compares the runtime with

TERM_SLOT = {'BaseValue': TERM_BASE, 'BaseValueMin': TERM_MIN, 'BaseValueMax': TERM_MAX, 'BaseValueAdd': TERM_ADD}


def cstr(s):
    """a C string literal of `s` (UTF-8; the bytes outside ASCII as octal escapes, so the header is plain ASCII)"""
    out = []
    for ch in s or '':
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\r':
            continue
        elif ch == '\t':
            out.append('\\t')
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            out.append(''.join('\\%03o' % b for b in ch.encode('utf-8')))
    return '"' + ''.join(out) + '"'


def cnum(v, none=-1.0):
    if v is None or isinstance(v, str):
        return '%r' % float(none)
    return '%r' % float(v)


def key_hits(e):
    """the hits the text states a number for, as `hit|term|value|closure|term text` (value "?" when it needs the player's stats; closure 1 when the
    hit is one the ability itself reaches, 0 when the text describes another ability's hit: a pet's, a bounce of another record), once each"""
    out, seen = [], set()
    own = {h['name'] for h in e['hits']}
    for t in e['tokens']:
        if 'hit' not in t or 'value_kind' not in t or t['value_kind'] == 'stat name':
            continue
        kind = t['value_kind']
        kinds = kind.split('-') if '-' in kind else [kind]
        vals = t.get('value')
        if not isinstance(vals, list):
            vals = [vals] * len(kinds)
        for k, v in zip(kinds, vals):
            key = (t['hit'], k)
            if key in seen:
                continue
            seen.add(key)
            ex = t.get('value_expr')
            if isinstance(ex, dict):
                term = ex.get(str(TERM_SLOT.get(k, 0)), ex.get(TERM_SLOT.get(k, 0), ''))
            else:
                term = ex or ''
            out.append('%s|%s|%s|%d|%s' % (t['hit'], 'Value' if k.startswith('Value') else k, '?' if v is None else '%r' % float(v),
                                           1 if t['hit'] in own else 0, ' '.join(str(term).split()).replace(';', ',').replace('|', '/')))
    return ';'.join(out)


def key_applies(e):
    """the effects the text names that the ability applies (no skill bought), as `effect=duration;...` in the text's order: the `applyK` tokens' effects
    and the `effect:` tokens whose record the ability's own closure reaches (an `effect:` token outside it is a condition or a description the text
    links to: "if the target is Demoralized", the weapon finisher's description).  The duration is the record's EffectDuration at the level ("inf" for an
    infinite one, "?" when only the stat engine gives it)"""
    closure = {x['name']: x for x in e['effects']}
    own = {h['name'] for h in e['hits']}
    out = []
    for t in e['tokens']:
        names = []
        if t['kind'] == 'effect' and t['effect'] in closure:
            names = [t['effect']]
        # an `applyK` of a hit the ability itself does not fire (a reaction's hit, another ability's) is marked "!": the text promises it, the ability's own
        # HitsIds, projectiles and zones do not carry it
        ref = 'hit' in t and t['hit'] not in own
        for a in t.get('applies', []) or []:
            names.append(('!' if ref else '') + a['effect'])
        for n in names:
            if n and n not in out and n.lstrip('!') not in out:
                out.append(n)
    parts = []
    for n in out:
        x = closure.get(n.lstrip('!'))
        if x is None:
            dur = '?'
        elif x['infinite']:
            dur = 'inf'
        elif x['duration_s'] is None:
            dur = '?'
        else:
            dur = '%r' % float(x['duration_s'])
        parts.append('%s=%s' % (n, dur))
    return ';'.join(parts)


def key_costs(db, e, level):
    """`stat=value|shown|enabled;...`: every stat cost (value "?" when only the stat engine can give it; shown "" when the line is not printed;
    enabled 1/0/? is the cost group's Enabled for the player: the client prints, and the runtime spends, the enabled groups only)"""
    out = []
    for c in e['cost']:
        if 'stat' not in c or c['stat'] == '0':
            continue
        v = c.get('value_none')
        shown = c.get('shown_none')
        en = '1'
        if c.get('enabled_expr'):
            ev, why = try_eval(db, c['enabled_expr'], level, 'none')
            en = '?' if ev is None else ('1' if ev else '0')
        out.append('%s=%s|%s|%s' % (c['stat'], '?' if v is None else '%r' % float(v), '' if shown is None else str(shown), en))
    return ';'.join(out)

# ~ C.Y.B & C | O | A ~

HEADER_BANNER = """// ============================================================================
//  tooltip_key.h
//
//  GENERATED by tools/capture/gen_tooltips.py; do not edit.
//
//  What the game client's ability tooltip states, for every ability of every class, the weapon combos and the general, consumable and profession
//  abilities (plan E1, stage 3 fix, Part C: the tooltip is the truth; test_worldd_tooltips.cpp holds the runtime to it).  The client builds the tooltip
//  in USkillToolTipWidget::BuildFinalTooltip (exe 0x1469519e0; the description's tokens in 0x146995490) from the ability record's AbilityDescription
//  template and the record's expressions evaluated for the PLAYER; this table is that evaluation for a level-%(level)d character with no gear, no skill
//  bought and no effects (research: the scratchpad's research/tooltips/evidence.md).  One Key per ability:
//
//    cooldown_s          the data's Cooldown (the skill tooltip prints no cooldown line); -1 when the generator could not evaluate it
//    charges             CooldownCharges rounded half up (the line is printed when it is over 1); charges_all: with every skill bought
//    costs               "stat=value|shown|enabled;..." each stat cost: the value, the number the line prints (about ceil) and whether the cost
//                        group is enabled for the player (1/0/?); "?" needs the stat engine
//    cast_s              DynamicCastDuration; -1: the cast time is the montage's notify time over the cast speed (client-side, not in the data)
//    range_cm, range_source   CustomTooltipRange, else AreaTargetRange (a clicked AoE), else the targeting record's ValidDistance, else ValidDistanceExp;
//                        the line prints floor(cm / 100) meters when over 0
//    target_mask         ApplicableTargets (Self 1, Enemy 2, Ally 4, Party 8, Raid 16); target_requirement: Always (a target is needed) / Never
//    hits                "hit|term|value|closure|term text;..." every hit the text states a number for: the value is the stat mod's term (BaseValue,
//                        BaseValueMin/Max/Add, or the whole Value when BaseValue is 0), which the tooltip prints x 100 with a percent sign; closure 0
//                        when the text describes another ability's hit (a pet's)
//    applies             "effect=duration;..." the effects the text names that the ability applies (the applyK tokens, and the effect: tokens the data's
//                        closure reaches; the rest are conditions or descriptions the text links to), with the record's duration (inf, ?); "!effect" is
//                        applied by a hit the ability itself does not fire (a reaction's hit, another ability's)
//    text                the tooltip's description as the client prints it (no skill bought)
// ============================================================================
#pragma once

#include <cstdint>

namespace tooltip_key {

constexpr int kLevel = %(level)d;

struct Key {
    const char* group;
    const char* name;
    int64_t     guid;
    double      cooldown_s;
    double      cooldown_all_s;
    const char* shared_tag;
    int         charges;
    int         charges_all;
    const char* costs;
    double      cast_s;
    double      range_cm;
    const char* range_source;
    int         target_mask;
    const char* target_requirement;
    const char* held;
    const char* weapon_requirement;
    const char* hits;
    const char* applies;
    const char* text;
};

const Key kKeys[] = {
"""


def render_header(db, res):
    lines = [HEADER_BANNER % {'level': res['level']}]
    n = 0
    for grp, lst in res['groups'].items():
        for e in lst:
            if 'error' in e:
                continue
            cd = e['cooldown_s']['by_skills']
            ch = e['charges']['by_skills']
            rng = e['range']
            lines.append('    {%s, %s, %dll, %s, %s, %s, %d, %d, %s, %s, %s, %s, %d, %s, %s, %s, %s, %s, %s},\n' % (
                cstr(grp), cstr(e['ability']), e['guid'], cnum(cd['none']), cnum(cd['all']), cstr(e['cooldown_s']['shared_tag']),
                ch['none'] if isinstance(ch['none'], int) else -1, ch['all'] if isinstance(ch['all'], int) else -1, cstr(key_costs(db, e, res['level'])),
                cnum(e['cast']['dynamic_s']), cnum(rng['cm']), cstr(rng['source']), e['targets']['mask'], cstr(e['targets']['requirement']),
                cstr(e['held']['behavior']), cstr(e['weapon_requirement']), cstr(key_hits(e)), cstr(key_applies(e)), cstr(e['text'])))
            n += 1
    lines.append('};\n\nconstexpr int kKeyCount = %d;\n\n}  // namespace tooltip_key\n' % n)
    return ''.join(lines)


def write_if_changed(path, text, check):
    """Write `text` (LF) to `path` with the line ending the file already has; with `check` write nothing. Returns True when nothing changed."""
    old = open(path, 'rb').read() if os.path.exists(path) else b''
    crlf = old.count(b'\r\n') * 2 > old.count(b'\n') > 0
    new = text.replace('\n', '\r\n' if crlf else '\n').encode('utf-8')
    try:
        rel = os.path.relpath(path, ROOT).replace(os.sep, '/')
    except ValueError:   # another drive
        rel = path
    if new == old:
        print('%s: unchanged (%d bytes)' % (rel, len(new)))
        return True
    if check:
        print('%s: DIFFERS from the regenerated text (%d bytes on disk, %d regenerated)' % (rel, len(old), len(new)))
        return False
    with open(path, 'wb') as f:
        f.write(new)
    print('%s: updated (%d bytes)' % (rel, len(new)))
    return False
# -- C Y B x CoA --


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--db', default=DEFAULT_DB, help='the content.db (default: the repository\'s dist/content/content.db)')
    ap.add_argument('--header', default=HEADER, help='the test header to write (default: server/worldd/tests/tooltip_key.h)')
    ap.add_argument('--out', help='also write the full JSON table here')
    ap.add_argument('--tsv', help='also write a one-line-per-ability summary here')
    ap.add_argument('--check', action='store_true', help='write nothing; exit 1 when the header (or the --out JSON) would change')
    ap.add_argument('--level', type=int, default=25)
    a = ap.parse_args()
    if not os.path.exists(a.db):
        sys.exit('no content.db at %s (cd services && go run ./cmd/contentc)' % a.db)
    db = DB(a.db)
    res = build(db, a.level)
    ok = write_if_changed(a.header, render_header(db, res), a.check)
    if a.out:
        text = json.dumps(res, indent=1, sort_keys=False, ensure_ascii=False) + '\n'
        ok = write_if_changed(a.out, text, a.check) and ok
    if a.tsv and not a.check:
        with open(a.tsv, 'w', encoding='utf-8', newline='\n') as f:
            f.write('\t'.join(['group', 'ability', 'display', 'cooldown_s', 'charges(none/all)', 'cost', 'range_m', 'targets', 'holds', 'text']) + '\n')
            for g, lst in res['groups'].items():
                for e in lst:
                    if 'error' in e:
                        continue
                    cost = ';'.join('%s %s' % (c.get('shown_none'), c.get('stat')) for c in e['cost'] if 'stat' in c)
                    row = [g, e['ability'], e['display_name'], str(e['cooldown_s']['by_skills']['none']),
                           '%s/%s' % (e['charges']['by_skills']['none'], e['charges']['by_skills']['all']), cost,
                           str(e['range']['shown_m'] or ''), '+'.join(e['targets']['applicable']),
                           e['held']['behavior'], e['text'].replace('\n', ' / ')]
                    f.write('\t'.join(row) + '\n')
    n = sum(len(v) for v in res['groups'].values())
    print('%d abilities in %d groups' % (n, len(res['groups'])))
    return 0 if ok or not a.check else 1


if __name__ == '__main__':
    sys.exit(main())
