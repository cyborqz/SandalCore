#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

"""gen_node_constants.py: the city node constants the design database lacks (Nodes 1, stage 1).

The design DB (CacheDB.dbc, contentc's input) holds the node records but not the numbers the client keeps in
Config/DefaultAoCRecordConstants.ini: the XP a node holds at each level (CityNodeExpToLevel), the reputation thresholds
(CityNodeReputationToLevel), MaxNodeLevel, MinimumOperationalNodeLevel, the citizenship level, the vassalship switches, the
timers of the buildings and the mayor, the records the ini selects (VassalshipDefinition, ElectionConstants ...).  This script reads
the ini of the Cache.db export and writes the two files that need them:

    services/internal/citynode/constants_gen.go       the citynode service (Go)
    server/worldd/world/node_constants.h              worldd (C++)

Why files and not a content.db table: the citynode service does not open content.db, and worldd needs most of them as compile-time
numbers; contentc stays a function of CacheDB.dbc alone.  The ini is a research input outside the repository (like the retail
captures): `--check` is how a change in the generated files shows (and is skipped by scripts/generators.ps1 when the ini is not
there).  The service's own test pins the Go numbers to the ini too (internal/citynode).

    python tools/capture/gen_node_constants.py                        # rewrite both files, "unchanged" or "updated"
    python tools/capture/gen_node_constants.py --check                # write nothing, exit 1 when a file would change
    python tools/capture/gen_node_constants.py --ini <AOC_EXPORT_DIR>/AOC/Config/DefaultAoCRecordConstants.ini

The ini is --ini, then $AOC_CLIENT_CONFIG/DefaultAoCRecordConstants.ini.  Output is deterministic; a file keeps the line ending it
already has (a new one is written with LF).
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
GO_OUT = os.path.join(ROOT, 'services', 'internal', 'citynode', 'constants_gen.go')
CPP_OUT = os.path.join(ROOT, 'server', 'worldd', 'world', 'node_constants.h')
INI_NAME = 'DefaultAoCRecordConstants.ini'

LEVELS = ['Wilderness', 'Crossroads', 'Encampment', 'Village', 'Town', 'City', 'Metropolis']
DAYS = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday']

# C | Y | B + CoA

def parse_ini(path):
    """name -> list of values (strings): `Name=v` sets the list, `+Name=v` appends; the first `=` splits."""
    out = {}
    with open(path, encoding='utf-8-sig') as f:
        for raw in f:
            line = raw.strip()
            if not line or line[0] in ';[':
                continue
            if '=' not in line:
                continue
            name, value = line.split('=', 1)
            if name.startswith('+'):
                out.setdefault(name[1:], []).append(value)
            elif name.startswith('-') or name.startswith('!'):
                out.pop(name[1:], None)
            else:
                out[name] = [value]
    return out


def one(ini, name):
    v = ini.get(name)
    if not v or len(v) != 1:
        sys.exit('%s: %s is not set exactly once in the ini (%r)' % (INI_NAME, name, v))
    return v[0]


def as_int(ini, name):
    return int(one(ini, name))


def as_bool(ini, name):
    s = one(ini, name)
    if s not in ('True', 'False'):
        sys.exit('%s: %s=%s is no bool' % (INI_NAME, name, s))
    return s == 'True'


def as_guid(ini, name):
    m = re.search(r'Guid=(\d+)', one(ini, name))
    if not m:
        sys.exit('%s: %s has no Guid' % (INI_NAME, name))
    return int(m.group(1))


def as_ms(ini, name):
    m = re.fullmatch(r'\(Duration=(\d+)\)', one(ini, name))
    if not m:
        sys.exit('%s: %s is no (Duration=ms)' % (INI_NAME, name))
    return int(m.group(1))


def timespan_secs(s):
    """`+00000000.04:00:00.000000000` (days.hh:mm:ss.fraction) in whole seconds."""
    m = re.fullmatch(r'[+-]?(\d+)\.(\d+):(\d+):(\d+)\.(\d+)', s)
    if not m:
        sys.exit('%s: %r is no timespan' % (INI_NAME, s))
    d, h, mi, se, frac = (int(x) for x in m.groups())
    if frac:
        sys.exit('%s: the timespan %r has a fraction of a second' % (INI_NAME, s))
    return ((d * 24 + h) * 60 + mi) * 60 + se


def level(ini, name):
    s = one(ini, name)
    if s not in LEVELS:
        sys.exit('%s: %s=%s is no ENodeLevel' % (INI_NAME, name, s))
    return LEVELS.index(s)


def collect(ini):
    """The constants as (name, kind, value, comment): kind is int, ints, bool, guid, text."""
    exp = [int(x) for x in ini.get('CityNodeExpToLevel', [])]
    rep = [int(x) for x in ini.get('CityNodeReputationToLevel', [])]
    if len(exp) != 7 or exp[0] != 0 or exp != sorted(exp):
        sys.exit('%s: CityNodeExpToLevel is not 7 rising numbers from 0 (%r)' % (INI_NAME, exp))
    if len(rep) != 5 or rep[0] != 0 or rep != sorted(rep):
        sys.exit('%s: CityNodeReputationToLevel is not 5 rising numbers from 0 (%r)' % (INI_NAME, rep))
    refresh = [timespan_secs(x) for x in ini.get('MayoralCommissionHandRefreshTimes', [])]
    day = one(ini, 'ServiceBuildingMaintenanceDayOfTheWeek')
    if day not in DAYS:
        sys.exit('%s: ServiceBuildingMaintenanceDayOfTheWeek=%s is no weekday' % (INI_NAME, day))
    c = [
        ('ExpToLevel', 'ints', exp, 'CityNodeExpToLevel: the XP a node holds in total when it reaches the level at that index (ENodeLevel)'),
        ('ReputationToLevel', 'ints', rep, 'CityNodeReputationToLevel: the reputation a citizen needs for each ENodeReputationRank'),
        ('MaxNodeLevel', 'int', level(ini, 'MaxNodeLevel'), 'MaxNodeLevel=%s: the highest ENodeLevel a node may be set to' % one(ini, 'MaxNodeLevel')),
        ('MinimumOperationalNodeLevel', 'int', level(ini, 'MinimumOperationalNodeLevel'),
         'MinimumOperationalNodeLevel=%s: below it a node does not work' % one(ini, 'MinimumOperationalNodeLevel')),
        ('NodeCitizenshipPlayerLevelRequirement', 'int', as_int(ini, 'NodeCitizenshipPlayerLevelRequirement'), 'the character level a citizen needs'),
        ('NodePassiveIncomeSeconds', 'int', int(float(one(ini, 'NodePassiveIncomeTimeInterval'))), 'NodePassiveIncomeTimeInterval'),
        ('MaxNumberNodeNotifications', 'int', as_int(ini, 'MaxNumberNodeNotifications'), 'the notifications a node keeps'),
        ('MayoralCommissionHandRefreshSeconds', 'ints', refresh, 'MayoralCommissionHandRefreshTimes, in seconds'),
        ('VassalshipSystemEnabled', 'bool', as_bool(ini, 'bVassalshipSystemEnabled'), 'bVassalshipSystemEnabled'),
        ('VassalshipProtectionEnabled', 'bool', as_bool(ini, 'bVassalshipProtectionEnabled'), 'bVassalshipProtectionEnabled'),
        ('VassalshipDefinitionGuid', 'guid', as_guid(ini, 'VassalshipDefinition'), 'VassalshipDefinition: the VassalshipDef the game uses'),
        ('BuyOrderConstantsGuid', 'guid', as_guid(ini, 'BuyOrderConstants'), 'BuyOrderConstants'),
        ('ElectionConstantsGuid', 'guid', as_guid(ini, 'ElectionConstants'), 'ElectionConstants'),
        ('WarConstantsGuid', 'guid', as_guid(ini, 'WarConstants'), 'WarConstants'),
        ('CitizenshipRewardGuid', 'guid', as_guid(ini, 'CitizenshipReward'), 'CitizenshipReward: a reward table'),
        ('NodeCurrencyGuid', 'guid', as_guid(ini, 'NodeCurrencyId'), 'NodeCurrencyId'),
        ('NodeAlignmentGuid', 'guid', as_guid(ini, 'NodeAlignment'), 'NodeAlignment: citizenship is this alignment'),
        ('NodeWarDeclaringAlignmentGuid', 'guid', as_guid(ini, 'NodeWarDeclaringAlignment'), 'NodeWarDeclaringAlignment'),
        ('NodeWarTargetAlignmentGuid', 'guid', as_guid(ini, 'NodeWarTargetAlignment'), 'NodeWarTargetAlignment'),
        ('NodeSiegeDeclaringAlignmentGuid', 'guid', as_guid(ini, 'NodeSiegeDeclaringAlignment'), 'NodeSiegeDeclaringAlignment'),
        ('NodeSiegeTargetAlignmentGuid', 'guid', as_guid(ini, 'NodeSiegeTargetAlignment'), 'NodeSiegeTargetAlignment'),
        ('NodeLevelUpCountdownToastGuid', 'guid', as_guid(ini, 'NodeLevelUpCountdownToastId'), 'NodeLevelUpCountdownToastId'),
        ('NodeCitizenshipGainToastGuid', 'guid', as_guid(ini, 'NodeCitizenshipGainToastId'), 'NodeCitizenshipGainToastId'),
        ('ServiceBuildingDefaultRepairSeconds', 'int', as_int(ini, 'NodeServiceBuildingDefaultRepairTime'), 'NodeServiceBuildingDefaultRepairTime'),
        ('ServiceBuildingDefaultDemolitionSeconds', 'int', as_int(ini, 'NodeServiceBuildingDefaultDemolitionTime'), 'NodeServiceBuildingDefaultDemolitionTime'),
        ('ServiceBuildingDemolitionVotingEnabled', 'bool', as_bool(ini, 'NodeServiceBuildingDemolitionVotingEnabled'), 'NodeServiceBuildingDemolitionVotingEnabled'),
        ('ServiceBuildingMaintenanceFrequencyWeeks', 'int', as_int(ini, 'ServiceBuildingMaintenanceFrequencyInWeeks'), 'ServiceBuildingMaintenanceFrequencyInWeeks'),
        ('ServiceBuildingMaintenanceDayOfWeek', 'text', day, 'ServiceBuildingMaintenanceDayOfTheWeek'),
        ('ZoiPlayerCountRefreshSeconds', 'int', as_int(ini, 'ZoiPlayerCountRefreshInterval'), 'ZoiPlayerCountRefreshInterval'),
        ('MayorMessageCooldownSeconds', 'int', as_int(ini, 'MayorMessageCooldownSeconds'), 'MayorMessageCooldownSeconds'),
        ('MaxMayorMessageLength', 'int', as_int(ini, 'MaxMayorMessageLength'), 'MaxMayorMessageLength'),
        ('StaticHouseUpgradeSeconds', 'int', as_ms(ini, 'CityNodeStaticHouseUpgradeDuration') // 1000, 'CityNodeStaticHouseUpgradeDuration'),
        ('SiegeJoinRequestCooldownSeconds', 'int', as_ms(ini, 'SiegeJoinRequestCooldownDuration') // 1000, 'SiegeJoinRequestCooldownDuration'),
        ('SiegeInactiveTag', 'text', one(ini, 'SiegeInactiveStateGameplayTagName'), 'SiegeInactiveStateGameplayTagName'),
        ('SiegePrepTag', 'text', one(ini, 'SiegePrepStateGameplayTagName'), 'SiegePrepStateGameplayTagName'),
        ('SiegeActiveTag', 'text', one(ini, 'SiegeActiveStateGameplayTagName'), 'SiegeActiveStateGameplayTagName'),
        ('SiegePillageTag', 'text', one(ini, 'SiegePillageStateGameplayTagName'), 'SiegePillageStateGameplayTagName'),
    ]
    if as_ms(ini, 'CityNodeStaticHouseUpgradeDuration') % 1000 or as_ms(ini, 'SiegeJoinRequestCooldownDuration') % 1000:
        sys.exit('%s: a duration is not whole seconds' % INI_NAME)
    return c

# [C.O.A + C Y B]

def render_go(consts):
    out = ['// Code generated by tools/capture/gen_node_constants.py from DefaultAoCRecordConstants.ini; DO NOT EDIT.',
           '//',
           '// The city node constants the design database lacks (Nodes 1, stage 1).  Regenerate with',
           '// `python tools/capture/gen_node_constants.py`; scripts/generators.ps1 checks that this file is what the ini gives.',
           '',
           'package citynode',
           '']
    for name, kind, value, comment in consts:
        out.append('// %s.' % comment if not comment.endswith('.') else '// %s' % comment)
        if kind == 'int':
            out.append('const %s = %d' % (name, value))
        elif kind == 'guid':
            out.append('const %s int64 = %d' % (name, value))
        elif kind == 'bool':
            out.append('const %s = %s' % (name, 'true' if value else 'false'))
        elif kind == 'text':
            out.append('const %s = "%s"' % (name, value))
        elif kind == 'ints':
            out.append('var %s = [...]int64{%s}' % (name, ', '.join(str(v) for v in value)))
        out.append('')
    return '\n'.join(out)


def render_cpp(consts):
    out = ['// ============================================================================',
           '//  world/node_constants.h (Nodes 1, stage 1)',
           '//',
           '//  GENERATED by tools/capture/gen_node_constants.py from DefaultAoCRecordConstants.ini (the Cache.db export): do not edit.',
           '//  The city node constants the design database lacks.  The citynode service has the same numbers (services/internal/citynode/constants_gen.go).',
           '//  Regenerate with `python tools/capture/gen_node_constants.py`; scripts/generators.ps1 checks that this file is what the ini gives.',
           '// ============================================================================',
           '#pragma once',
           '',
           '#include <array>',
           '#include <cstdint>',
           '#include <string_view>',
           '',
           'namespace aoc::worldd::world::node_constants {',
           '']
    for name, kind, value, comment in consts:
        out.append('/// %s' % comment)
        k = 'k' + name
        if kind == 'int':
            out.append('inline constexpr int %s = %d;' % (k, value))
        elif kind == 'guid':
            out.append('inline constexpr int64_t %s = %dLL;' % (k, value))
        elif kind == 'bool':
            out.append('inline constexpr bool %s = %s;' % (k, 'true' if value else 'false'))
        elif kind == 'text':
            out.append('inline constexpr std::string_view %s = "%s";' % (k, value))
        elif kind == 'ints':
            out.append('inline constexpr std::array<int64_t, %d> %s = {%s};' % (len(value), k, ', '.join('%dLL' % v for v in value)))
        out.append('')
    out.append('}  // namespace aoc::worldd::world::node_constants')
    out.append('')
    return '\n'.join(out)


def write_if_changed(path, text, check):
    """Compare/write with the line ending the file already has (LF for a new one). Returns True when the file is as the text says."""
    crlf = False
    old = None
    if os.path.exists(path):
        with open(path, 'rb') as f:
            old = f.read()
        crlf = b'\r\n' in old
    data = text.replace('\r\n', '\n')
    if crlf:
        data = data.replace('\n', '\r\n')
    data = data.encode('utf-8')
    if old == data:
        print('%s: unchanged' % os.path.relpath(path, ROOT))
        return True
    if check:
        print('%s: OUT OF DATE (regenerate with `python tools/capture/gen_node_constants.py`)' % os.path.relpath(path, ROOT))
        return False
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)
    print('%s: updated' % os.path.relpath(path, ROOT))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--ini', help='DefaultAoCRecordConstants.ini (default $AOC_CLIENT_CONFIG/%s)' % INI_NAME)
    ap.add_argument('--check', action='store_true', help='write nothing; exit 1 when a generated file would change')
    ap.add_argument('--go-out', default=GO_OUT)
    ap.add_argument('--cpp-out', default=CPP_OUT)
    args = ap.parse_args()
    ini_path = args.ini
    if not ini_path and os.environ.get('AOC_CLIENT_CONFIG'):
        ini_path = os.path.join(os.environ['AOC_CLIENT_CONFIG'], INI_NAME)
    if not ini_path or not os.path.exists(ini_path):
        sys.exit('the ini is not at hand: pass --ini or set AOC_CLIENT_CONFIG to the Cache.db export\'s AOC/Config directory')
    consts = collect(parse_ini(ini_path))
    ok = write_if_changed(args.go_out, render_go(consts), args.check)
    ok = write_if_changed(args.cpp_out, render_cpp(consts), args.check) and ok
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
