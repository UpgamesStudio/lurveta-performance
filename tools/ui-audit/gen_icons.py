#!/usr/bin/env python3
"""LURVETA icon system generator.

Original line icons drawn on a 24x24 grid (1.8 stroke, round caps and joins) plus a duotone file set.
Every icon is emitted as an Android VectorDrawable that keeps the resource name of the PNG it
replaces, so all references in Java and XML keep working. Glyphs are black by default, as the PNGs
were, and are tinted by the theme where they are used. Action-bar glyphs are light because the
toolbar does not tint them.

Usage:
  gen_icons.py <res_dir> [preview.html]
"""
import math, os, sys

S = 1.8  # stroke width on the 24 grid

# ---------------------------------------------------------------- primitives -> path data
def f(v):
    s = ('%.2f' % v).rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s

def circle(cx, cy, r):
    return f'M{f(cx - r)},{f(cy)}a{f(r)},{f(r)} 0 1,0 {f(2 * r)},0a{f(r)},{f(r)} 0 1,0 {f(-2 * r)},0Z'

def rect(x, y, w, h, rx=0):
    if rx <= 0:
        return f'M{f(x)},{f(y)}h{f(w)}v{f(h)}h{f(-w)}Z'
    rx = min(rx, w / 2, h / 2)
    return (f'M{f(x + rx)},{f(y)}h{f(w - 2 * rx)}a{f(rx)},{f(rx)} 0 0,1 {f(rx)},{f(rx)}'
            f'v{f(h - 2 * rx)}a{f(rx)},{f(rx)} 0 0,1 {f(-rx)},{f(rx)}h{f(-(w - 2 * rx))}'
            f'a{f(rx)},{f(rx)} 0 0,1 {f(-rx)},{f(-rx)}v{f(-(h - 2 * rx))}a{f(rx)},{f(rx)} 0 0,1 {f(rx)},{f(-rx)}Z')

def line(x1, y1, x2, y2):
    return f'M{f(x1)},{f(y1)}L{f(x2)},{f(y2)}'

def poly(*pts, close=False):
    d = 'M' + ' L'.join(f'{f(x)},{f(y)}' for x, y in pts)
    return d + ('Z' if close else '')

def star(cx, cy, ro, ri, n=5, rot=-90):
    pts = []
    for i in range(n * 2):
        r = ro if i % 2 == 0 else ri
        a = math.radians(rot + i * 180 / n)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return poly(*pts, close=True)

def gear(cx, cy, ro=9.6, ri=7.4, teeth=8, hole=3.0):
    pts = []
    step = 360 / teeth
    for i in range(teeth):
        a0 = i * step
        for da, r in ((-step * 0.20, ro), (step * 0.20, ro), (step * 0.30, ri), (step * 0.70, ri)):
            a = math.radians(a0 + da - 90)
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return [('s', poly(*pts, close=True)), ('s', circle(cx, cy, hole))]

def arc_arrow_reset():
    # two arcs with arrow heads (refresh)
    return [('s', 'M20,11a8,8 0 0,0 -14.3,-4.9L4,8'), ('s', poly((4, 3.5), (4, 8), (8.5, 8))),
            ('s', 'M4,13a8,8 0 0,0 14.3,4.9L20,16'), ('s', poly((20, 20.5), (20, 16), (15.5, 16)))]

# element: ('s', d) stroke, ('f', d) fill, ('a', d) accent fill (duotone), ('b', d) body fill
GAMEPAD = ('M7.2,6.5h9.6a5,5 0 0,1 4.9,5.9l-0.9,4.8a2.6,2.6 0 0,1 -4.6,1.1L14.6,15.9H9.4l-1.6,2.4'
           'a2.6,2.6 0 0,1 -4.6,-1.1l-0.9,-4.8A5,5 0 0,1 7.2,6.5Z')
FOLDER = 'M3,7.2a2.2,2.2 0 0,1 2.2,-2.2h4.1l2,2.4h7.5a2.2,2.2 0 0,1 2.2,2.2v8.2a2.2,2.2 0 0,1 -2.2,2.2H5.2A2.2,2.2 0 0,1 3,17.8Z'
TRASH = [('s', line(4, 6.5, 20, 6.5)), ('s', 'M9,6.5V4.6a1.1,1.1 0 0,1 1.1,-1.1h3.8a1.1,1.1 0 0,1 1.1,1.1V6.5'),
         ('s', 'M6.2,6.5l0.9,12.6a1.8,1.8 0 0,0 1.8,1.7h6.2a1.8,1.8 0 0,0 1.8,-1.7l0.9,-12.6'),
         ('s', line(10, 10.5, 10, 16.5)), ('s', line(14, 10.5, 14, 16.5))]
PENCIL = [('s', 'M14.8,4.6a2.1,2.1 0 0,1 3,0l1.6,1.6a2.1,2.1 0 0,1 0,3L9.2,19.4L4.3,20.5l1.1,-4.9Z'),
          ('s', line(13, 6.4, 17.6, 11))]
INFO = [('s', circle(12, 12, 9)), ('s', line(12, 11, 12, 16.5)), ('f', circle(12, 7.8, 1.15))]
CHIP = [('s', rect(6, 6, 12, 12, 2.2)), ('s', rect(9.5, 9.5, 5, 5, 1)),
        ('s', line(9.5, 2.8, 9.5, 6)), ('s', line(14.5, 2.8, 14.5, 6)), ('s', line(9.5, 18, 9.5, 21.2)), ('s', line(14.5, 18, 14.5, 21.2)),
        ('s', line(2.8, 9.5, 6, 9.5)), ('s', line(2.8, 14.5, 6, 14.5)), ('s', line(18, 9.5, 21.2, 9.5)), ('s', line(18, 14.5, 21.2, 14.5))]
COPY = [('s', rect(8.5, 8.5, 12, 12, 2.4)), ('s', 'M15.5,8.5V5.9a2.4,2.4 0 0,0 -2.4,-2.4H5.9A2.4,2.4 0 0,0 3.5,5.9v7.2a2.4,2.4 0 0,0 2.4,2.4h2.6')]
DUPLICATE = COPY + [('s', line(14.5, 11.6, 14.5, 17.4)), ('s', line(11.6, 14.5, 17.4, 14.5))]
DOWNLOAD = [('s', line(12, 3.5, 12, 14.5)), ('s', poly((7.5, 10), (12, 14.5), (16.5, 10))), ('s', 'M4,15.5v2.3A2.7,2.7 0 0,0 6.7,20.5h10.6a2.7,2.7 0 0,0 2.7,-2.7v-2.3')]
PLAY_FILLED = 'M8.2,5.6a1.2,1.2 0 0,1 1.8,-1l9.4,6.4a1.2,1.2 0 0,1 0,2L10,19.4a1.2,1.2 0 0,1 -1.8,-1Z'
SLIDERS = [('s', line(4, 7, 20, 7)), ('s', line(4, 12, 20, 12)), ('s', line(4, 17, 20, 17)),
           ('fs', circle(9, 7, 2.1)), ('fs', circle(15.5, 12, 2.1)), ('fs', circle(7.5, 17, 2.1))]
MONITOR = [('s', rect(2.8, 4, 18.4, 12.5, 2.4)), ('s', line(8.5, 20.5, 15.5, 20.5)), ('s', line(12, 16.5, 12, 20.5))]
WINDOWS2 = [('s', rect(7.5, 3.5, 13, 10.5, 2)), ('s', line(7.5, 6.8, 20.5, 6.8)),
            ('s', 'M4.5,9.2v8.6a2,2 0 0,0 2,2h10.3'), ('s', line(4.5, 9.2, 4.5, 9.2))]

ICONS = {
    # navigation
    'icon_shortcut': [('s', rect(3, 8, 18, 13, 3)), ('s', line(5.5, 5, 18.5, 5)), ('s', line(8, 2.4, 16, 2.4)),
                      ('f', 'M10.4,11.6a0.7,0.7 0 0,1 1.05,-0.6l3.9,2.4a0.7,0.7 0 0,1 0,1.2l-3.9,2.4a0.7,0.7 0 0,1 -1.05,-0.6Z')],
    'icon_container': [('s', 'M12,2.8L20.4,7.4V16.6L12,21.2L3.6,16.6V7.4Z'), ('s', poly((3.6, 7.4), (12, 12), (20.4, 7.4))), ('s', line(12, 12, 12, 21.2))],
    'icon_input_controls': [('s', GAMEPAD), ('s', line(7.6, 9.6, 7.6, 13.2)), ('s', line(5.8, 11.4, 9.4, 11.4)),
                            ('f', circle(15.6, 10.3, 1.05)), ('f', circle(17.6, 12.5, 1.05))],
    'icon_settings': gear(12, 12),
    'icon_about': INFO,
    'icon_info': INFO,
    # actions
    'icon_add': [('s', line(12, 5, 12, 19)), ('s', line(5, 12, 19, 12))],
    'icon_confirm': [('s', poly((4.5, 12.5), (9.5, 17.5), (19.5, 6.5)))],
    'icon_remove': TRASH,
    'icon_remove2': [('s', line(6, 6, 18, 18)), ('s', line(18, 6, 6, 18))],
    'icon_edit': PENCIL,
    'icon_duplicate': DUPLICATE,
    'icon_reset': arc_arrow_reset(),
    'icon_install': DOWNLOAD,
    'icon_export': [('s', 'M14,4.5H18a2.5,2.5 0 0,1 2.5,2.5V17a2.5,2.5 0 0,1 -2.5,2.5H6A2.5,2.5 0 0,1 3.5,17V13'),
                    ('s', line(3.5, 11, 13, 11)), ('s', poly((9.5, 7.5), (13, 11), (9.5, 14.5))), ('s', 'M3.5,8V7A2.5,2.5 0 0,1 6,4.5h2')],
    'icon_open': [('s', 'M13,4.5H6.5a2,2 0 0,0 -2,2v11a2,2 0 0,0 2,2h11a2,2 0 0,0 2,-2V11'), ('s', line(11, 13, 19.5, 4.5)), ('s', poly((14, 4.5), (19.5, 4.5), (19.5, 10)))],
    'icon_paste': [('s', rect(5, 4.8, 14, 16.2, 2.4)), ('s', rect(8.8, 2.8, 6.4, 3.8, 1.2)), ('s', line(8.8, 11, 15.2, 11)), ('s', line(8.8, 15, 13.2, 15))],
    'icon_run': [('f', PLAY_FILLED)],
    'icon_play': [('s', circle(12, 12, 9)), ('f', 'M10,8.9a0.8,0.8 0 0,1 1.2,-0.7l5,3.1a0.8,0.8 0 0,1 0,1.4l-5,3.1A0.8,0.8 0 0,1 10,15.1Z')],
    'icon_pause': [('f', rect(6.5, 5, 4, 14, 1.4)), ('f', rect(13.5, 5, 4, 14, 1.4))],
    'icon_power': [('s', 'M7.4,6.6a7.8,7.8 0 1,0 9.2,0'), ('s', line(12, 3, 12, 11.5))],
    'icon_exit': [('s', 'M10,4.5H6.5a2,2 0 0,0 -2,2v11a2,2 0 0,0 2,2H10'), ('s', line(9.5, 12, 20, 12)), ('s', poly((16, 8), (20, 12), (16, 16)))],
    'icon_list_item_menu': [('f', circle(12, 5.5, 1.7)), ('f', circle(12, 12, 1.7)), ('f', circle(12, 18.5, 1.7))],
    'icon_menu': [('s', line(4, 6.5, 20, 6.5)), ('s', line(4, 12, 20, 12)), ('s', line(4, 17.5, 20, 17.5))],
    'icon_help': [('s', circle(12, 12, 9)), ('s', 'M9.4,9.3a2.7,2.7 0 0,1 5.2,0.9c0,1.8 -2.6,2.3 -2.6,4'), ('f', circle(12, 17.1, 1.15))],
    'icon_hide': [('s', 'M2.6,12C4.6,8.2 8,5.8 12,5.8s7.4,2.4 9.4,6.2C19.4,15.8 16,18.2 12,18.2S4.6,15.8 2.6,12Z'),
                  ('s', circle(12, 12, 2.8)), ('s', line(4.5, 19.5, 19.5, 4.5))],
    'icon_move': [('s', line(12, 3, 12, 21)), ('s', line(3, 12, 21, 12)), ('s', poly((9.2, 5.8), (12, 3), (14.8, 5.8))), ('s', poly((9.2, 18.2), (12, 21), (14.8, 18.2))),
                  ('s', poly((5.8, 9.2), (3, 12), (5.8, 14.8))), ('s', poly((18.2, 9.2), (21, 12), (18.2, 14.8)))],
    'icon_zoom_plus': [('s', circle(10.5, 10.5, 6.8)), ('s', line(15.4, 15.4, 20.5, 20.5)), ('s', line(10.5, 7.6, 10.5, 13.4)), ('s', line(7.6, 10.5, 13.4, 10.5))],
    'icon_zoom_minus': [('s', circle(10.5, 10.5, 6.8)), ('s', line(15.4, 15.4, 20.5, 20.5)), ('s', line(7.6, 10.5, 13.4, 10.5))],
    'icon_magnifier': [('s', circle(10.5, 10.5, 6.8)), ('s', line(15.4, 15.4, 20.5, 20.5))],
    'icon_folder_search': [('s', 'M11,19H5.2A2.2,2.2 0 0,1 3,16.8V7.2A2.2,2.2 0 0,1 5.2,5h4.1l2,2.4h7.5a2.2,2.2 0 0,1 2.2,2.2v1.4'),
                           ('s', circle(16.5, 15.5, 3.2)), ('s', line(18.8, 17.8, 21, 20))],
    'icon_fullscreen': [('s', poly((4, 9), (4, 4), (9, 4))), ('s', poly((15, 4), (20, 4), (20, 9))), ('s', poly((20, 15), (20, 20), (15, 20))), ('s', poly((9, 20), (4, 20), (4, 15)))],
    'icon_pip_mode': [('s', rect(2.8, 4.5, 18.4, 15, 2.4)), ('f', rect(12, 12, 6.5, 4.8, 1.2))],
    # system / tech
    'icon_cpu': CHIP,
    'icon_memory': [('s', rect(2.8, 7, 18.4, 9, 1.8)), ('s', line(7, 10, 7, 13)), ('s', line(10.3, 10, 10.3, 13)), ('s', line(13.7, 10, 13.7, 13)), ('s', line(17, 10, 17, 13)),
                    ('s', line(6, 16, 6, 19)), ('s', line(10, 16, 10, 19)), ('s', line(14, 16, 14, 19)), ('s', line(18, 16, 18, 19))],
    'icon_temperature': [('s', 'M14.5,13.8V5a2.5,2.5 0 0,0 -5,0v8.8a4.5,4.5 0 1,0 5,0Z'), ('f', circle(12, 17.3, 1.7)), ('s', line(12, 9, 12, 15.5))],
    'icon_debug': [('s', rect(7, 8, 10, 12.5, 5)), ('s', 'M9,8V7a3,3 0 0,1 6,0v1'), ('s', line(12, 11, 12, 20.5)),
                   ('s', line(3.5, 10, 7, 11.5)), ('s', line(20.5, 10, 17, 11.5)), ('s', line(3, 15, 7, 15)), ('s', line(21, 15, 17, 15)),
                   ('s', line(3.8, 20, 7.4, 18)), ('s', line(20.2, 20, 16.6, 18))],
    'icon_task_manager': [('s', rect(2.8, 4, 18.4, 16, 2.4)), ('s', poly((5.5, 13), (8.5, 13), (10.5, 9), (13.5, 16), (15.5, 12), (18.5, 12)))],
    'icon_active_windows': WINDOWS2,
    'icon_screen_effect': MONITOR + [('s', 'M12,7.2l0.9,2.1l2.1,0.9l-2.1,0.9l-0.9,2.1l-0.9,-2.1l-2.1,-0.9l2.1,-0.9Z')],
    'icon_display_settings': SLIDERS,
    'icon_audio_settings': [('s', 'M4,9.5h3l4.5,-4v13l-4.5,-4H4Z'), ('s', 'M15.2,9a4.2,4.2 0 0,1 0,6'), ('s', 'M17.8,6.4a7.8,7.8 0 0,1 0,11.2')],
    'icon_keyboard': [('s', rect(2.5, 6, 19, 12, 2.4)), ('f', circle(6.5, 10, 0.95)), ('f', circle(9.5, 10, 0.95)), ('f', circle(12.5, 10, 0.95)), ('f', circle(15.5, 10, 0.95)),
                      ('f', circle(18, 10, 0.95)), ('s', line(8, 14.2, 16, 14.2))],
    'icon_piano': [('s', rect(3, 4.5, 18, 15, 2.2)), ('s', line(9, 4.5, 9, 19.5)), ('s', line(15, 4.5, 15, 19.5)),
                   ('f', rect(7.4, 4.5, 3.2, 8, 0.6)), ('f', rect(13.4, 4.5, 3.2, 8, 0.6))],
    'icon_env_var': [('s', 'M8,4.5H7A2,2 0 0,0 5,6.5V10l-1.5,2L5,14v3.5A2,2 0 0,0 7,19.5h1'), ('s', 'M16,4.5h1a2,2 0 0,1 2,2V10l1.5,2L19,14v3.5a2,2 0 0,1 -2,2h-1'),
                     ('s', line(9.6, 9.6, 14.4, 14.4)), ('s', line(14.4, 9.6, 9.6, 14.4))],
    'icon_gamepad': [('s', GAMEPAD), ('s', line(7.6, 9.6, 7.6, 13.2)), ('s', line(5.8, 11.4, 9.4, 11.4)), ('f', circle(15.6, 10.3, 1.05)), ('f', circle(17.6, 12.5, 1.05))],
    'icon_wine': [('s', 'M7.5,3.5h9l0.4,4.2a4.9,4.9 0 0,1 -9.8,0Z'), ('s', line(12, 12.6, 12, 20)), ('s', line(8.5, 20.5, 15.5, 20.5)), ('s', line(7.6, 7.4, 16.4, 7.4))],
    # file manager / misc
    'number_picker_increment': [('s', line(12, 5, 12, 19)), ('s', line(5, 12, 19, 12))],
    'number_picker_decrement': [('s', line(5, 12, 19, 12))],
    'content_dialog_type_alert': [('s', circle(12, 12, 9)), ('s', line(12, 7.3, 12, 13)), ('f', circle(12, 16.6, 1.15))],
    'content_dialog_type_confirm': [('s', circle(12, 12, 9)), ('s', 'M9.4,9.3a2.7,2.7 0 0,1 5.2,0.9c0,1.8 -2.6,2.3 -2.6,4'), ('f', circle(12, 17.1, 1.15))],
    # popup menu (tinted with the accent)
    'icon_popup_menu_add_favorite': [('s', star(11, 12.2, 8.3, 3.7)), ('s', line(19.5, 2.8, 19.5, 7.2)), ('s', line(17.3, 5, 21.7, 5))],
    'icon_popup_menu_bring_to_front': [('s', rect(3.5, 3.5, 11, 11, 2)), ('fs', rect(9.5, 9.5, 11, 11, 2))],
    'icon_popup_menu_copy': COPY,
    'icon_popup_menu_cpu': CHIP,
    'icon_popup_menu_cut': [('s', circle(6.5, 17.5, 2.8)), ('s', circle(17.5, 17.5, 2.8)), ('s', line(8.4, 15.4, 18, 3.5)), ('s', line(15.6, 15.4, 6, 3.5))],
    'icon_popup_menu_download': DOWNLOAD,
    'icon_popup_menu_duplicate': DUPLICATE,
    'icon_popup_menu_edit': PENCIL,
    'icon_popup_menu_file_manager': [('s', FOLDER)],
    'icon_popup_menu_folder_list': [('s', FOLDER), ('s', line(8, 12, 16, 12)), ('s', line(8, 15.5, 13, 15.5))],
    'icon_popup_menu_folder_open': [('s', 'M3,17V7.2A2.2,2.2 0 0,1 5.2,5h4.1l2,2.4h6.5a2.2,2.2 0 0,1 2.2,2.2v0.9'),
                                    ('s', 'M3,17.6l2.2,-6.1a1.8,1.8 0 0,1 1.7,-1.2h13.3a1.2,1.2 0 0,1 1.1,1.6l-2.2,6.2a1.8,1.8 0 0,1 -1.7,1.2H4.6A1.6,1.6 0 0,1 3,17.6Z')],
    'icon_popup_menu_info': INFO,
    'icon_popup_menu_remove': TRASH,
    'icon_popup_menu_settings': gear(12, 12),
}

# Action bar glyphs: drawn light (#EEF3F8), the toolbar does not tint them.
ACTION_BAR = {
    'icon_action_bar_add': [('s', line(12, 5, 12, 19)), ('s', line(5, 12, 19, 12))],
    'icon_action_bar_back': [('s', line(19.5, 12, 4.8, 12)), ('s', poly((11, 5.8), (4.8, 12), (11, 18.2)))],
    'icon_action_bar_grid': [('s', rect(3.5, 3.5, 7, 7, 1.8)), ('s', rect(13.5, 3.5, 7, 7, 1.8)), ('s', rect(3.5, 13.5, 7, 7, 1.8)), ('s', rect(13.5, 13.5, 7, 7, 1.8))],
    'icon_action_bar_home': [('s', 'M3.5,10.6L12,3.8l8.5,6.8V19a1.6,1.6 0 0,1 -1.6,1.6H5.1A1.6,1.6 0 0,1 3.5,19Z'), ('s', 'M9.5,20.6v-5.4a1,1 0 0,1 1,-1h3a1,1 0 0,1 1,1v5.4')],
    'icon_action_bar_list': [('s', line(9, 6.5, 20.5, 6.5)), ('s', line(9, 12, 20.5, 12)), ('s', line(9, 17.5, 20.5, 17.5)),
                             ('f', circle(4.6, 6.5, 1.3)), ('f', circle(4.6, 12, 1.3)), ('f', circle(4.6, 17.5, 1.3))],
    'icon_action_bar_menu': [('s', line(3.5, 6.5, 20.5, 6.5)), ('s', line(3.5, 12, 14.5, 12)), ('s', line(3.5, 17.5, 20.5, 17.5))],
    'icon_action_bar_new_folder': [('s', FOLDER), ('s', line(12, 10.6, 12, 16.2)), ('s', line(9.2, 13.4, 14.8, 13.4))],
}

# Duotone file icons (not tinted by the views that show them): graphite body + cyan accent.
BODY, BODY_STROKE, ACC, ACC_DIM, PAPER = '#243142', '#3A4A5E', '#00C8F5', '#0E6F87', '#D8E3EE'
FILE_SHAPE = 'M6.5,2.5h7.3l5.2,5.2V19.3a2.2,2.2 0 0,1 -2.2,2.2H6.5a2.2,2.2 0 0,1 -2.2,-2.2V4.7A2.2,2.2 0 0,1 6.5,2.5Z'
FOLDER_BACK = 'M2.5,6.4a2.2,2.2 0 0,1 2.2,-2.2h4.6l2.1,2.4h7.9a2.2,2.2 0 0,1 2.2,2.2v9.6a2.2,2.2 0 0,1 -2.2,2.2H4.7a2.2,2.2 0 0,1 -2.2,-2.2Z'
FOLDER_FRONT = 'M2.5,9.6a1.8,1.8 0 0,1 1.8,-1.8h15.4a1.8,1.8 0 0,1 1.8,1.8v8.8a2.2,2.2 0 0,1 -2.2,2.2H4.7a2.2,2.2 0 0,1 -2.2,-2.2Z'
DUO = {
    'container_folder': [('c', FOLDER_BACK, ACC_DIM), ('c', FOLDER_FRONT, BODY), ('k', FOLDER_FRONT, BODY_STROKE), ('c', rect(5, 11, 5, 1.4, 0.7), ACC)],
    'container_folder_documents': [('c', FOLDER_BACK, ACC_DIM), ('c', rect(6, 5.6, 12, 9, 1), PAPER), ('c', FOLDER_FRONT, BODY), ('k', FOLDER_FRONT, BODY_STROKE),
                                   ('c', rect(5, 11, 5, 1.4, 0.7), ACC)],
    'container_folder_favorites': [('c', FOLDER_BACK, ACC_DIM), ('c', FOLDER_FRONT, BODY), ('k', FOLDER_FRONT, BODY_STROKE), ('c', star(12, 14.6, 4.2, 1.9), ACC)],
    'container_drive': [('c', rect(2.5, 6.5, 19, 11, 2.6), BODY), ('k', rect(2.5, 6.5, 19, 11, 2.6), BODY_STROKE), ('c', rect(2.5, 13, 19, 4.5, 2), '#1B2532'),
                        ('c', circle(17.4, 15.2, 1.1), ACC), ('c', rect(5, 14.6, 6, 1.2, 0.6), BODY_STROKE)],
    'container_file': [('c', FILE_SHAPE, BODY), ('k', FILE_SHAPE, BODY_STROKE), ('c', 'M13.8,2.5v3.4a1.8,1.8 0 0,0 1.8,1.8h3.4Z', BODY_STROKE),
                       ('c', rect(7.3, 11, 9.4, 1.3, 0.65), PAPER), ('c', rect(7.3, 14, 9.4, 1.3, 0.65), PAPER), ('c', rect(7.3, 17, 6, 1.3, 0.65), ACC)],
    'container_file_library': [('c', FILE_SHAPE, BODY), ('k', FILE_SHAPE, BODY_STROKE), ('c', 'M13.8,2.5v3.4a1.8,1.8 0 0,0 1.8,1.8h3.4Z', BODY_STROKE),
                               ('c', gear(11.7, 14.6, 4.4, 3.3, 8, 1.4)[0][1], ACC), ('c', circle(11.7, 14.6, 1.3), BODY)],
    'container_file_link': [('c', rect(3, 3, 18, 18, 4.5), BODY), ('k', rect(3, 3, 18, 18, 4.5), BODY_STROKE),
                            ('c', 'M9.6,8.1a0.9,0.9 0 0,1 1.35,-0.78l6.1,3.9a0.9,0.9 0 0,1 0,1.56l-6.1,3.9A0.9,0.9 0 0,1 9.6,15.9Z', ACC)],
    'container_file_window': [('c', rect(2.5, 4, 19, 16, 2.6), BODY), ('k', rect(2.5, 4, 19, 16, 2.6), BODY_STROKE), ('c', 'M2.5,6.6a2.6,2.6 0 0,1 2.6,-2.6h13.8a2.6,2.6 0 0,1 2.6,2.6v1.6h-19Z', ACC_DIM),
                              ('c', circle(16.4, 6.1, 0.8), ACC), ('c', circle(18.8, 6.1, 0.8), PAPER), ('c', rect(5, 11, 6, 6, 1.2), ACC), ('c', rect(12.6, 11, 6.4, 1.3, 0.65), PAPER),
                              ('c', rect(12.6, 14, 4.6, 1.3, 0.65), PAPER)],
    'remove_button': [('c', circle(12, 12, 10), '#FF5A6A'), ('ks', line(8.4, 8.4, 15.6, 15.6), '#FFFFFF'), ('ks', line(15.6, 8.4, 8.4, 15.6), '#FFFFFF')],
}

HEADER = '<?xml version="1.0" encoding="utf-8"?>\n<!-- LURVETA icon system (tools/ui-audit/gen_icons.py). Original artwork. -->\n'

def vector(elements, color='#FF000000', size=24):
    out = [HEADER, '<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
           f'    android:width="{size}dp"\n    android:height="{size}dp"\n    android:viewportWidth="24"\n    android:viewportHeight="24">\n']
    for kind, d in elements:
        if kind == 's':
            out.append(f'    <path android:pathData="{d}" android:strokeColor="{color}" android:strokeWidth="{S}" '
                       'android:strokeLineCap="round" android:strokeLineJoin="round" />\n')
        elif kind == 'f':
            out.append(f'    <path android:pathData="{d}" android:fillColor="{color}" />\n')
        elif kind == 'fs':
            out.append(f'    <path android:pathData="{d}" android:fillColor="{color}" android:strokeColor="{color}" android:strokeWidth="{S}" '
                       'android:strokeLineJoin="round" />\n')
    out.append('</vector>\n')
    return ''.join(out)

def vector_duo(elements, size=24):
    out = [HEADER, '<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
           f'    android:width="{size}dp"\n    android:height="{size}dp"\n    android:viewportWidth="24"\n    android:viewportHeight="24">\n']
    for kind, d, color in elements:
        if kind == 'c':
            out.append(f'    <path android:pathData="{d}" android:fillColor="{color}" />\n')
        elif kind == 'k':
            out.append(f'    <path android:pathData="{d}" android:strokeColor="{color}" android:strokeWidth="1" />\n')
        elif kind == 'ks':
            out.append(f'    <path android:pathData="{d}" android:strokeColor="{color}" android:strokeWidth="2" android:strokeLineCap="round" />\n')
    out.append('</vector>\n')
    return ''.join(out)

def svg(elements, color='#0b0f14', duo=False):
    parts = []
    if duo:
        for kind, d, c in elements:
            if kind == 'c':
                parts.append(f'<path d="{d}" fill="{c}"/>')
            elif kind == 'k':
                parts.append(f'<path d="{d}" fill="none" stroke="{c}" stroke-width="1"/>')
            else:
                parts.append(f'<path d="{d}" fill="none" stroke="{c}" stroke-width="2" stroke-linecap="round"/>')
    else:
        for kind, d in elements:
            if kind == 's':
                parts.append(f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{S}" stroke-linecap="round" stroke-linejoin="round"/>')
            elif kind == 'f':
                parts.append(f'<path d="{d}" fill="{color}"/>')
            else:
                parts.append(f'<path d="{d}" fill="{color}" stroke="{color}" stroke-width="{S}" stroke-linejoin="round"/>')
    return '<svg viewBox="0 0 24 24" width="48" height="48">' + ''.join(parts) + '</svg>'

def main():
    res = sys.argv[1]
    preview = sys.argv[2] if len(sys.argv) > 2 else None
    dst = os.path.join(res, 'drawable')
    written = []
    for name, el in ICONS.items():
        open(os.path.join(dst, name + '.xml'), 'w').write(vector(el)); written.append(name)
    for name, el in ACTION_BAR.items():
        open(os.path.join(dst, name + '.xml'), 'w').write(vector(el, '#FFEEF3F8')); written.append(name)
    for name, el in DUO.items():
        open(os.path.join(dst, name + '.xml'), 'w').write(vector_duo(el)); written.append(name)
    # remove the raster versions that the vectors replace
    removed = 0
    for name in written:
        for folder in ('drawable-hdpi', 'drawable-xxhdpi', 'drawable-xhdpi', 'drawable-mdpi'):
            for ext in ('.png', '.9.png'):
                p = os.path.join(res, folder, name + ext)
                if os.path.exists(p):
                    os.remove(p); removed += 1
    print(f'{len(written)} vector icons written, {removed} PNG files replaced')
    if preview:
        cells = []
        for name, el in ICONS.items():
            cells.append(f'<figure><div class="t dark">{svg(el, "#00C8F5")}</div><div class="t light">{svg(el)}</div><figcaption>{name}</figcaption></figure>')
        for name, el in ACTION_BAR.items():
            cells.append(f'<figure><div class="t dark">{svg(el, "#EEF3F8")}</div><div class="t light">{svg(el)}</div><figcaption>{name}</figcaption></figure>')
        for name, el in DUO.items():
            cells.append(f'<figure><div class="t dark">{svg(el, duo=True)}</div><div class="t light">{svg(el, duo=True)}</div><figcaption>{name}</figcaption></figure>')
        html = ('<!doctype html><meta charset="utf-8"><style>body{background:#080B10;color:#8D9AAA;font:11px sans-serif;margin:16px;display:flex;flex-wrap:wrap;gap:8px}'
                'figure{margin:0;width:118px}.t{display:inline-flex;width:56px;height:56px;align-items:center;justify-content:center;border-radius:10px}'
                '.dark{background:#151C25}.light{background:#F2F4F7}figcaption{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}</style>' + ''.join(cells))
        open(preview, 'w').write(html)

if __name__ == '__main__':
    main()
