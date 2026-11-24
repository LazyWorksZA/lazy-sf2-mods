import sys, os, json, datetime, pefile
root = sys.argv[1]; out = sys.argv[2]
mods = []
for dp, _, fs in os.walk(root):
    if 'DX_redist' in dp: continue
    for f in fs:
        if f.lower().endswith(('.dll', '.exe')) and not f.lower().startswith('uninst'):
            p = os.path.join(dp, f)
            pe = pefile.PE(p, fast_load=True)
            pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'], pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT'], pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT']])
            imps = {}
            for e in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []) + getattr(pe, 'DIRECTORY_ENTRY_DELAY_IMPORT', []):
                imps[e.dll.decode()] = [(i.name.decode() if i.name else f'ord{i.ordinal}') for i in e.imports]
            exps = [(e.name.decode() if e.name else f'ord{e.ordinal}') for e in getattr(pe, 'DIRECTORY_ENTRY_EXPORT', type('x',(object,),{'symbols':[]})).symbols]
            mods.append(dict(path=os.path.relpath(p, root), machine=hex(pe.FILE_HEADER.Machine),
                ts=datetime.datetime.fromtimestamp(pe.FILE_HEADER.TimeDateStamp, datetime.UTC).strftime('%Y-%m-%d'),
                base=hex(pe.OPTIONAL_HEADER.ImageBase), size=os.path.getsize(p), imports=imps, exports=exps))
json.dump(mods, open(out, 'w'), indent=1)
print(len(mods), 'modules')
