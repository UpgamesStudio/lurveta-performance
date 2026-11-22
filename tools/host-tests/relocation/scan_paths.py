# Scan extracted trees / files for occurrences of b"com.winlator" and classify them.
import os, sys, re, collections, stat
NEEDLE = b"com.winlator"
PRINT = re.compile(rb"[\x21-\x7e]*")
def ctx(data, i):
    s = i
    while s > 0 and 0x21 <= data[s-1] <= 0x7e and i - s < 80: s -= 1
    e = i + len(NEEDLE)
    while e < len(data) and 0x21 <= data[e] <= 0x7e and e - i < 160: e += 1
    return data[s:e]
def kind(path, data):
    if data[:4] == b"\x7fELF": return "elf"
    if b"\x00" in data[:4096]: return "binary"
    return "text"
results = collections.defaultdict(set)
symlinks = []
roots = sys.argv[1:]
for root in roots:
    paths = []
    if os.path.isfile(root): paths = [root]
    else:
        for dp, dn, fn in os.walk(root):
            for n in fn + dn:
                paths.append(os.path.join(dp, n))
    for p in paths:
        st = os.lstat(p)
        if stat.S_ISLNK(st.st_mode):
            t = os.readlink(p)
            if "com.winlator" in t: symlinks.append((p, t))
            continue
        if not stat.S_ISREG(st.st_mode): continue
        with open(p, "rb") as f: data = f.read()
        idx = data.find(NEEDLE)
        if idx < 0: continue
        k = kind(p, data)
        while idx >= 0:
            results[(k, ctx(data, idx).decode("latin1"))].add(os.path.relpath(p, root) if os.path.isdir(root) else os.path.basename(p))
            idx = data.find(NEEDLE, idx + 1)
by_prefix = collections.Counter()
for (k, c), files in sorted(results.items()):
    m = re.search(r"/data/data/com\.winlator(/[A-Za-z0-9_.-]*(?:/[A-Za-z0-9_.-]*)?)?", c)
    pre = m.group(0) if m else "(no /data/data prefix) " + c[:60]
    by_prefix[(k, pre)] += len(files)
print("== prefixes (kind, prefix up to 2 components) -> file count")
for (k, pre), n in sorted(by_prefix.items(), key=lambda x: -x[1]): print(f"{n:5d}  {k:6s}  {pre}")
print("== symlinks with com.winlator:", len(symlinks))
for p, t in symlinks[:20]: print("  ", p, "->", t)
print("== distinct full strings (first 60):")
for (k, c), files in list(sorted(results.items()))[:60]: print(f"  [{k}] {c[:150]}  ({len(files)} files, e.g. {sorted(files)[0]})")
