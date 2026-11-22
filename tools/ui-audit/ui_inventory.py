#!/usr/bin/env python3
"""LURVETA UI audit: inventory of layouts, view ids, view classes and menu ids.

Usage:
  ui_inventory.py snapshot <res_dir> <out.json>     # capture the contract
  ui_inventory.py compare  <before.json> <after.json>  # verify nothing functional changed

The comparison fails (exit 1) if any view id disappears from a layout, if the view class
behind an id changes, if a menu item id disappears, or if a layout/menu file is removed.
It is the guard used by the visual redesign: presentation may change, the contract may not.
"""
import json, os, sys, glob
import xml.etree.ElementTree as ET

ANDROID = '{http://schemas.android.com/apk/res/android}'

def parse_layouts(res):
    out = {}
    for path in sorted(glob.glob(os.path.join(res, 'layout*', '*.xml'))):
        name = os.path.relpath(path, res)
        root = ET.parse(path).getroot()
        ids = {}
        def walk(el, depth):
            vid = el.get(ANDROID + 'id')
            if vid and vid.startswith('@+id/'):
                ids[vid[5:]] = el.tag
            elif vid and vid.startswith('@id/'):
                ids[vid[4:]] = el.tag
            for c in el:
                walk(c, depth + 1)
        walk(root, 0)
        out[name] = {'root': root.tag, 'ids': ids}
    return out

def parse_menus(res):
    out = {}
    for path in sorted(glob.glob(os.path.join(res, 'menu*', '*.xml'))):
        name = os.path.relpath(path, res)
        root = ET.parse(path).getroot()
        items = []
        for el in root.iter('item'):
            vid = el.get(ANDROID + 'id')
            items.append({'id': vid, 'title': el.get(ANDROID + 'title'), 'icon': el.get(ANDROID + 'icon')})
        out[name] = items
    return out

def snapshot(res, out):
    data = {'layouts': parse_layouts(res), 'menus': parse_menus(res)}
    with open(out, 'w') as f:
        json.dump(data, f, indent=1, sort_keys=True)
    n_ids = sum(len(v['ids']) for v in data['layouts'].values())
    n_items = sum(len(v) for v in data['menus'].values())
    print(f"{len(data['layouts'])} layouts, {n_ids} view ids, {len(data['menus'])} menus, {n_items} menu items -> {out}")

def compare(a, b):
    A = json.load(open(a)); B = json.load(open(b))
    errors = []
    for lay, info in A['layouts'].items():
        if lay not in B['layouts']:
            errors.append(f'layout removed: {lay}'); continue
        nb = B['layouts'][lay]
        if info['root'] != nb['root']:
            errors.append(f'{lay}: root class changed {info["root"]} -> {nb["root"]}')
        for vid, cls in info['ids'].items():
            if vid not in nb['ids']:
                errors.append(f'{lay}: id removed @{vid} ({cls})')
            elif nb['ids'][vid] != cls:
                errors.append(f'{lay}: @{vid} class changed {cls} -> {nb["ids"][vid]}')
    for menu, items in A['menus'].items():
        if menu not in B['menus']:
            errors.append(f'menu removed: {menu}'); continue
        bids = [i['id'] for i in B['menus'][menu]]
        for it in items:
            if it['id'] is not None and it['id'] not in bids:
                errors.append(f'{menu}: item removed {it["id"]}')
        for it in items:
            if it['id'] is None: continue
            for jt in B['menus'][menu]:
                if jt['id'] == it['id'] and jt['title'] != it['title']:
                    errors.append(f'{menu}: title changed for {it["id"]}')
        # Items without an id (e.g. argument lists) are compared by position and title.
        if [i['title'] for i in items if i['id'] is None] != [i['title'] for i in B['menus'][menu] if i['id'] is None]:
            errors.append(f'{menu}: id-less items changed')
    added = []
    for lay, nb in B['layouts'].items():
        old = A['layouts'].get(lay, {'ids': {}})['ids']
        for vid in nb['ids']:
            if vid not in old:
                added.append(f'{lay}: +@{vid} ({nb["ids"][vid]})')
    if added:
        print('Ids added (decorative/structural, informative only):')
        for x in added: print('  ' + x)
    if errors:
        print('CONTRACT BROKEN:')
        for e in errors: print('  ' + e)
        sys.exit(1)
    print(f'OK: {len(A["layouts"])} layouts and {len(A["menus"])} menus keep every id, root class and view class.')

if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'snapshot':
        snapshot(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == 'compare':
        compare(sys.argv[2], sys.argv[3])
    else:
        print(__doc__); sys.exit(2)
