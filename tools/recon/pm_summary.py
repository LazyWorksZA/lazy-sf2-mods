import csv, sys, collections, re

rows = list(csv.DictReader(open(sys.argv[1], encoding='utf-8-sig', errors='replace')))
print('events', len(rows), 'ops', collections.Counter(r['Operation'] for r in rows).most_common(12))

GAME = 'f:\\program files (x86)\\thirdwire\\strike fighters 2\\'
USERS = re.compile(r'(?i)^c:\\users\\[^\\]+')


def short(p):
    if p.lower().startswith(GAME):
        return '<GAME>\\' + p[len(GAME):]
    return USERS.sub('%USERPROFILE%', p)


seen, order = set(), []
for r in rows:
    if r['Operation'] in ('CreateFile', 'Load Image') and r['Result'] in ('SUCCESS', 'NAME NOT FOUND', 'PATH NOT FOUND'):
        p = r['Path']; pl = p.lower()
        if re.search(r'\.(ini|cat|dll|lst|bmp|exe)$', pl) and (r['Operation'] == 'Load Image' or '\\windows\\' not in pl):
            k = (r['Operation'], pl, r['Result'])
            if k not in seen:
                seen.add(k)
                order.append(f"{r['Time of Day'][:12]} {r['Operation']:<10} {r['Result']:<14} {short(p)}")
print('\n'.join(order))

print('\n--- network')
for r in rows:
    if r['Operation'].startswith(('TCP', 'UDP')) and r['Operation'].endswith(('Connect', 'Send')):
        print(r['Time of Day'][:12], r['Operation'], r['Path'])

print('\n--- registry (ThirdWire / Direct3D / DXGI)')
rk = {}
for r in rows:
    if r['Operation'].startswith('Reg') and re.search(r'(?i)thirdwire|direct3d|dxgi', r['Path']):
        rk.setdefault((r['Operation'], r['Path'], r['Result']), 1)
for k in list(rk)[:30]:
    print(*k)
