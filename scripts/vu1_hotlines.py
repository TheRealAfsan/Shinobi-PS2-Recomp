"""Where the time goes inside translated VU1 programs, by source line and by kind of generated code.

usage: python -I vu1_hotlines.py <raw samples> <exe .map> <listing.asm> [<listing2.asm> ...] [--thread NAME] [--top N]

  raw samples: written by the profiler with PS2X_PROFILE_RAW=<file> ("<thread> <rva hex> <count>" lines)
  listing:     the program's .cpp compiled with the build's exact flags plus /FAsc (machine code with source lines)

Each sample is placed in its function (by the map's start address), then at the listing instruction at that offset,
which carries the source file and line. Lines of the generated program are classified by what they do.
"""
import bisect
import collections
import os
import re
import sys

PROC_RE = re.compile(r'^(\S+)\s+PROC\b')
ENDP_RE = re.compile(r'^(\S+)\s+ENDP\b')
LINE_RE = re.compile(r'^; (\d+)\s+:')
FILE_RE = re.compile(r'^; File (.+)$')
INSN_RE = re.compile(r'^\s+([0-9a-f]{5,8})\s')


def load_map(path):
    base = 0x140000000
    names = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        if 'Preferred load address is' in line:
            base = int(line.split('is')[-1].strip(), 16)
            continue
        parts = line.split()
        if len(parts) >= 3 and len(parts[0]) == 13 and parts[0][4] == ':' and len(parts[2]) == 16:
            try:
                value = int(parts[2], 16)
            except ValueError:
                continue
            if value >= base:
                names.setdefault(parts[1], value - base)
    return names


def load_listing(path):
    """{mangled function: [(offset, file, line), ...]} in offset order."""
    functions = {}
    current = None
    file = None
    line_no = None
    main_file = None
    for raw in open(path, encoding='utf-8', errors='replace'):
        text = raw.rstrip('\n')
        m = FILE_RE.match(text)
        if m:
            file = m.group(1).strip()
            if main_file is None and file.lower().endswith('.cpp'):
                main_file = file
            continue
        m = PROC_RE.match(text)
        if m:
            current = m.group(1)
            functions[current] = []
            continue
        if ENDP_RE.match(text):
            current = None
            continue
        m = LINE_RE.match(text)
        if m:
            line_no = int(m.group(1))
            continue
        if current is not None:
            m = INSN_RE.match(text)
            if m and line_no is not None:
                functions[current].append((int(m.group(1), 16), file, line_no))
    return functions


def classify(text):
    t = text.strip()
    if '>= budget' in t:
        return 'budget check + exit'
    if 'std::max(t' in t or 'li_' in t and 'Ready' in t or 'vfStall' in t or 'accStall' in t:
        return 'stall timing'
    if 'Ready[' in t or 'noteReady' in t or 'lrl = ' in t or 'vfWritten' in t or 'accWritten' in t:
        return 'ready-cycle bookkeeping'
    if 'fmacLite' in t:
        return 'FMAC (lite: sticky flags only)'
    if 'fmacVec' in t:
        return 'FMAC (full flags)'
    if 'commitTo' in t or 'commitQ' in t or 'clipFlag' in t or 'fcset' in t or 'fsset' in t:
        return 'flag/Q commit'
    if 'x.load(' in t or 'loadVi' in t or 'x.store' in t or 'storeVf' in t or 'storeVi' in t or 'x.address' in t:
        return 'VU memory load/store'
    if 'xg' in t and ('Kick' in t or 'kick' in t or 'xgProgress' in t):
        return 'XGKICK'
    if '_mm_storeu_ps(x.vf' in t or '_mm_loadu_ps(x.vf' in t or '= _mm_loadu_ps(' in t or 'x.vf[' in t:
        return 'VF register load/store'
    if 'x.vi[' in t or 'lvi' in t:
        return 'integer (VI) op'
    if 'vbValid' in t or 'vbReg' in t:
        return 'branch-delay VI backup'
    if t.startswith('goto') or t.startswith('if (taken)') or 'switch' in t or t.startswith('case'):
        return 'control flow'
    if t.startswith('t = ') or t.startswith('uint64_t t') or t.startswith('cyc = '):
        return 'cycle counter'
    return 'other: ' + t[:60]


def main():
    args = sys.argv[1:]
    thread = None
    top = 40
    if '--thread' in args:
        i = args.index('--thread')
        thread = args[i + 1]
        del args[i:i + 2]
    if '--top' in args:
        i = args.index('--top')
        top = int(args[i + 1])
        del args[i:i + 2]
    if len(args) < 3:
        print(__doc__)
        return 2
    raw_path, map_path, listings = args[0], args[1], args[2:]
    samples = collections.Counter()
    for line in open(raw_path):
        parts = line.split()
        if len(parts) != 3 or (thread and parts[0] != thread):
            continue
        samples[int(parts[1], 16)] += int(parts[2])
    total = sum(samples.values())
    names = load_map(map_path)
    starts = sorted(names.values())
    by_start = {}
    for name, rva in names.items():
        by_start.setdefault(rva, name)
    functions = {}
    for listing in listings:
        functions.update(load_listing(listing))
    sources = {}

    def source_line(file, line):
        if file not in sources:
            try:
                sources[file] = open(file, encoding='utf-8', errors='replace').read().splitlines()
            except OSError:
                sources[file] = []
        lines = sources[file]
        return lines[line - 1] if 0 < line <= len(lines) else ''

    def pair_of(file, line):
        lines = sources.get(file) or []
        for i in range(min(line, len(lines)) - 1, max(0, line - 400), -1):
            m = re.search(r'// (?:0x)?([0-9a-f]{4}) up=', lines[i]) or re.match(r'\s*([FP][0-9a-f]{4}):', lines[i])
            if m:
                return m.group(1)
        return '?'

    covered = 0
    by_kind = collections.Counter()
    by_line = collections.Counter()
    by_pair = collections.Counter()
    for rva, count in samples.items():
        i = bisect.bisect_right(starts, rva) - 1
        if i < 0:
            continue
        name = by_start[starts[i]]
        entries = functions.get(name)
        if not entries:
            continue
        offset = rva - starts[i]
        j = bisect.bisect_right([e[0] for e in entries], offset) - 1
        if j < 0:
            continue
        _, file, line = entries[j]
        covered += count
        text = source_line(file, line)
        if file.lower().endswith('.cpp'):
            kind = classify(text)
            by_pair[(os.path.basename(file)[8:14], pair_of(file, line))] += count
        else:
            kind = 'inlined helper: ' + os.path.basename(file)
        by_kind[kind] += count
        by_line[(os.path.basename(file), line, text.strip()[:90])] += count
    print('samples: %d total, %d (%.1f%%) inside the listed programs' % (total, covered, 100.0 * covered / max(total, 1)))
    print('\nBy kind of generated code (share of the listed programs\' time):')
    for kind, count in by_kind.most_common(25):
        print('  %5.1f%%  %s' % (100.0 * count / max(covered, 1), kind))
    print('\nHottest source lines:')
    for (file, line, text), count in by_line.most_common(top):
        print('  %5.2f%%  %s:%d  %s' % (100.0 * count / max(covered, 1), file, line, text))
    print('\nHottest VU1 pairs:')
    for (prog, pc), count in by_pair.most_common(15):
        print('  %5.2f%%  %s pair %s' % (100.0 * count / max(covered, 1), prog, pc))
    return 0


if __name__ == '__main__':
    sys.exit(main())
