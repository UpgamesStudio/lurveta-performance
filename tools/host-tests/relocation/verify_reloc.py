import os, sys, stat
OLD=b"/data/data/com.winlator/files/rootfs"; NEW=b"/data/data/com.lurveta.co/files/rtfs"
assert len(OLD)==len(NEW)
orig_root, new_root = sys.argv[1], sys.argv[2]
files=changed=bad=0; left=[]
for dp, dn, fn in os.walk(orig_root):
    for n in fn:
        o=os.path.join(dp,n); r=os.path.join(new_root, os.path.relpath(o, orig_root))
        so=os.lstat(o)
        if stat.S_ISLNK(so.st_mode):
            t_o=os.readlink(o); t_n=os.readlink(r)
            if t_o.replace(OLD.decode(), NEW.decode()) != t_n: bad+=1; print("SYMLINK MISMATCH", r, t_o, t_n)
            continue
        if not stat.S_ISREG(so.st_mode): continue
        a=open(o,'rb').read(); b=open(r,'rb').read(); files+=1
        if b"com.winlator" in b: left.append(r)
        if a==b: continue
        changed+=1
        if len(a)!=len(b) or a.replace(OLD,NEW)!=b: bad+=1; print("CONTENT MISMATCH", r)
print(f"files={files} changed={changed} mismatches={bad} files_still_containing_com.winlator={len(left)}")
for x in left[:10]: print("  LEFT", x)
