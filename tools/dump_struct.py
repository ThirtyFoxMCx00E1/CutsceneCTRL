import subprocess, sys, re

LIB = "gamelibs/arm64-v8a/libGTASA.so"

def dump(offset_hex):
    off = int(offset_hex, 16)
    out = subprocess.run(
        ["readelf", "--debug-dump=info", "-W", f"--dwarf-start={off}", LIB],
        capture_output=True, text=True, errors="replace"
    ).stdout
    lines = out.splitlines()
    # first line is depth <1> (our struct). Collect until next depth-<1> line (exclusive), after the first.
    result = []
    started = False
    cur = {}
    class_name = None
    byte_size = None
    for i, l in enumerate(lines):
        m1 = re.match(r'\s*<1><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_\w+)\)', l)
        if m1:
            if not started:
                started = True
                continue
            else:
                break  # reached next top-level DIE
        if not started:
            continue
        mname = re.search(r'DW_AT_name\s*:.*:\s*(\S.*)$', l)
        mloc = re.search(r'DW_AT_data_member_location:\s*\(data1\)\s*(\d+)', l)
        mloc2 = re.search(r'DW_AT_data_member_location:\s*\(data2\)\s*(\d+)', l)
        mtype = re.search(r'DW_AT_type\s*:.*,\s*([^,]+)$', l)
        msize = re.search(r'DW_AT_byte_size\s*:.*?(\d+)\s*$', l)
        mtag = re.match(r'\s*<2><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_member)\)', l)
        if mtag:
            if cur:
                result.append(cur)
            cur = {}
        if mname and 'name' not in cur:
            cur['name'] = mname.group(1)
        if mloc:
            cur['loc'] = int(mloc.group(1))
        if mloc2:
            cur['loc'] = int(mloc2.group(1))
        if mtype:
            cur['type'] = mtype.group(1).strip()
        if msize and byte_size is None and re.search(r'DW_AT_byte_size', l):
            byte_size = msize.group(1)
        if re.search(r'DW_AT_name.*:\s*(\S.*)$', l) and class_name is None and re.match(r'\s*<1>', "") :
            pass
    if cur:
        result.append(cur)
    return result, byte_size, "\n".join(lines[:3])

if __name__ == "__main__":
    off = sys.argv[1]
    members, size, header = dump(off)
    print(header)
    print("byte_size (raw, may be from any nested subitem, verify manually):", size)
    for m in members:
        print(f"  +{m.get('loc','?'):<4} {m.get('type','?'):<25} {m.get('name','?')}")
