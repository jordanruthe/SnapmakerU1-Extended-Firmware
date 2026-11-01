#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

"""Browser test for the filament library in the Filament Manager web UI.

Serves the static UI with a mocked Moonraker (WebSocket JSON-RPC and HTTP) and
drives add, Spoolman import, apply, edit and delete. Needs Python Playwright.

Usage: test_library.py [html_dir] [screenshot_dir]
"""
import json, os, sys, re, tempfile
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
HTML = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '../root/usr/local/filament-ui/html')
OUT = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix='filament-ui-')
os.makedirs(OUT, exist_ok=True)
print('screenshots:', OUT)
state = {'library': None, 'gcode': [], 'uploads': 0, 'restarts': 0, 'restart_404': False}

ptc = {
    'filament_vendor': ['Snapmaker', 'Generic', 'NONE', 'NONE'],
    'filament_type': ['PLA', 'PETG', 'NONE', 'NONE'],
    'filament_sub_type': ['Basic', '', 'NONE', 'NONE'],
    'filament_color_rgba': ['FF0000FF', '00FF00FF', 'FFFFFFFF', 'FFFFFFFF'],
    'filament_official': [True, False, False, False],
    'filament_exist': [True, True, False, False],
    'filament_spool_id': [0, 0, 0, 0],
    'filament_soft': [False]*4, 'filament_edit': [False, True, True, True],
}
fd = {'info': [{'VENDOR': 'Snapmaker', 'MAIN_TYPE': 'PLA', 'SUB_TYPE': 'Basic', 'RGB_1': 0xFF0000, 'OFFICIAL': True, 'CARD_UID': [1,2,3,4]}, {}, {}, {}], 'state': [0,0,0,0]}
sm_filaments = [
    {'id': 7, 'name': 'PLA+ Black', 'material': 'pla+', 'color_hex': '111111', 'settings_extruder_temp': 215, 'vendor': {'name': 'eSun'}, 'extra': {}},
    {'id': 9, 'name': 'Silk Dual', 'material': 'PLA', 'multi_color_hexes': 'FFD700,C0C0C0', 'vendor': {'name': 'Sunlu'}, 'extra': {'variant': '"Silk"'}},
    {'id': 11, 'name': 'Bambu PLA', 'material': 'PLA', 'color_hex': 'FF8800', 'vendor': {'name': 'Bambu Lab'}, 'extra': {}},
]

def ws_handler(ws):
    def on_msg(msg):
        m = json.loads(msg); method = m['method']; p = m.get('params', {})
        res = {}
        if method in ('printer.objects.subscribe', 'printer.objects.query'):
            res = {'status': {'filament_detect': fd, 'print_task_config': ptc}}
        elif method == 'server.spoolman.status':
            res = {'spoolman_connected': True}
        elif method == 'server.config':
            res = {'config': {'spoolman': {'server': 'http://sm.test'}}}
        elif method == 'server.spoolman.proxy':
            res = sm_filaments if p['path'] == '/v1/filament' else []
        elif method == 'printer.gcode.script':
            state['gcode'].append(p['script']); res = 'ok'
        ws.send(json.dumps({'jsonrpc': '2.0', 'id': m['id'], 'result': res}))
    ws.on_message(on_msg)

def route(route):
    url = route.request.url
    path = re.sub(r'^https?://[^/]+', '', url).split('?')[0]
    if path.startswith('/filament/'):
        f = os.path.join(HTML, path[len('/filament/'):] or 'index.html')
        ctype = {'.js': 'application/javascript', '.css': 'text/css', '.html': 'text/html'}[os.path.splitext(f)[1]]
        return route.fulfill(status=200, body=open(f, 'rb').read(), content_type=ctype)
    if path == '/server/files/config/extended/filament_library.json':
        if state['library'] is None: return route.fulfill(status=404, body='{}')
        return route.fulfill(status=200, body=state['library'], content_type='application/json')
    if path == '/server/files/upload':
        body = route.request.post_data_buffer.decode()
        assert 'name="root"' in body and 'config' in body and 'name="path"' in body
        state['library'] = body[body.index('{'):body.rindex('}')+1]; state['uploads'] += 1
        return route.fulfill(status=201, body='{}', content_type='application/json')
    if path == '/firmware-config/api/action/restart-screen':
        assert route.request.method == 'POST'
        if state['restart_404']: return route.fulfill(status=404, body='')
        state['restarts'] += 1
        return route.fulfill(status=200, body='=== Restart Screen ===\n\nSUCCESS: Completed successfully (exit code: 0)\n', content_type='text/plain')
    return route.fulfill(status=404, body='')

with sync_playwright() as pw:
    b = pw.chromium.launch()
    page = b.new_page(viewport={'width': 1200, 'height': 900})
    errors = []
    page.on('pageerror', lambda e: errors.append(str(e)))
    page.on('console', lambda m: m.type == 'error' and errors.append(m.text))
    page.route_web_socket(re.compile(r'.*/websocket'), ws_handler)
    page.route('**/*', route)
    page.goto('http://printer.test/filament/')
    page.wait_for_selector('text=Connected')
    page.wait_for_selector('.channel-action-btn:has-text("Library")')

    # 1. Manage: add custom PLA+ filament
    page.click('#open-library')
    page.wait_for_selector('text=No saved filaments yet')
    page.click('#library-add')
    page.fill('#library-edit-form [name=name]', 'Overture PLA+ Grey')
    page.fill('#library-edit-form [name=vendor]', 'Overture')
    page.fill('#le-type', 'pla+')
    assert page.is_visible('#le-base-type-row')
    assert page.input_value('#le-base-type') == 'PLA', page.input_value('#le-base-type')
    page.click('#le-color-add')
    page.fill('#library-edit-form [name=load_temp]', '230')
    page.fill('#library-edit-form [name=unload_temp]', '220')
    assert page.inner_text('#le-preview') == 'On the touchscreen: Overture → PLA+', page.inner_text('#le-preview')
    assert not page.is_visible('#le-warning')
    page.screenshot(path=f'{OUT}/1-edit.png')
    page.click('#library-edit-form button[type=submit]')
    page.wait_for_selector('#library-list .spoolman-name:has-text("Overture PLA+ Grey")')
    saved = json.loads(state['library'])
    print('saved:', json.dumps(saved['filaments']))
    assert saved['filaments'][0]['type'] == 'PLA+' and saved['filaments'][0]['base_type'] == 'PLA'

    # Restart notice after a save; 404 means firmware-config is disabled
    assert page.is_visible('#library-restart')
    assert 'Restart the screen to show changes' in page.inner_text('#library-restart')
    page.screenshot(path=f'{OUT}/8-restart-notice.png')
    state['restart_404'] = True
    page.click('#library-restart-btn'); page.click('#confirm-accept')
    page.wait_for_selector('#status-message:has-text("not available")')
    assert state['restarts'] == 0 and page.is_visible('#library-restart')
    state['restart_404'] = False
    page.click('#library-restart-btn')
    page.click('#confirm-cancel')
    assert state['restarts'] == 0
    page.click('#library-restart-btn'); page.click('#confirm-accept')
    page.wait_for_selector('#status-message:has-text("Screen restarting")')
    assert state['restarts'] == 1 and not page.is_visible('#library-restart')

    # 2. Import from Spoolman
    page.click('#library-import')
    page.wait_for_selector('#library-list .spoolman-name:has-text("Silk Dual")')
    page.screenshot(path=f'{OUT}/2-spoolman.png')
    assert page.locator('#library-list .spoolman-item:has-text("Bambu PLA") .library-note').count() == 1
    assert page.locator('#library-list .spoolman-item:has-text("Silk Dual") .library-note').count() == 0
    page.click('#library-import-all')
    page.wait_for_selector('#library-list .spoolman-id:has-text("#9 ✓")')
    page.click('#library-import')  # back to library
    page.wait_for_selector('#library-list .spoolman-name:has-text("PLA+ Black")')
    page.screenshot(path=f'{OUT}/3-library.png')
    lib = json.loads(state['library'])['filaments']
    print('after import:', json.dumps(lib))
    page.click('#library-close')

    # 3. Apply to extruder 2 (non-official) - no confirm
    page.locator('.channel-action-btn:has-text("Library")').nth(1).click()
    page.wait_for_selector('#library-modal-title:has-text("Extruder 2")')
    page.click('#library-list .spoolman-item:has-text("Overture PLA+ Grey")')
    page.wait_for_function('document.getElementById("library-modal").style.display === "none"')
    # 4. Apply to extruder 1 (official RFID) - confirm
    page.locator('.channel-action-btn:has-text("Library")').nth(0).click()
    page.click('#library-list .spoolman-item:has-text("Silk Dual")')
    page.wait_for_selector('#confirm-modal:not([style*="none"])')
    page.screenshot(path=f'{OUT}/4-confirm.png')
    page.click('#confirm-accept')
    page.wait_for_function('document.getElementById("library-modal").style.display === "none"')
    print('gcode:', *state['gcode'], sep='\n  ')
    assert len(state['gcode']) == 2
    assert 'CONFIG_EXTRUDER=1 VENDOR="Overture" FILAMENT_TYPE="PLA+" FILAMENT_SUBTYPE=""' in state['gcode'][0]
    assert 'CONFIG_EXTRUDER=0 VENDOR="Sunlu" FILAMENT_TYPE="PLA" FILAMENT_SUBTYPE="Silk"' in state['gcode'][1]

    # 5. Re-import keeps hand-set load temp; edit + delete
    page.click('#open-library')
    page.click('#library-list .spoolman-item:has-text("PLA+ Black")')
    page.fill('#library-edit-form [name=load_temp]', '240')
    page.click('#library-edit-form button[type=submit]')
    page.wait_for_selector('#library-edit-modal[style*="none"]', state='attached')
    page.click('#library-import'); page.wait_for_selector('#library-list .spoolman-name:has-text("PLA+ Black")')
    page.click('#library-list .spoolman-item:has-text("PLA+ Black")')
    page.wait_for_timeout(300)
    e = [f for f in json.loads(state['library'])['filaments'] if f.get('spoolman_filament_id') == 7]
    print('reimported #7:', json.dumps(e))
    assert len(e) == 1 and e[0]['load_temp'] == 240 and e[0]['print_temp'] == 215
    page.click('#library-import')
    page.click('#library-list .spoolman-item:has-text("Overture PLA+ Grey")')
    page.click('#library-delete'); page.click('#confirm-accept')
    page.wait_for_selector('#library-list .spoolman-name:has-text("Overture")', state='detached')
    assert [f['name'] for f in json.loads(state['library'])['filaments']] == ['PLA+ Black', 'Silk Dual', 'Bambu PLA']
    print('after delete:', [f['name'] for f in json.loads(state['library'])['filaments']], 'uploads:', state['uploads'])

    # 6. Narrow viewport
    page.set_viewport_size({'width': 390, 'height': 844})
    page.click('#library-add'); page.screenshot(path=f'{OUT}/5-mobile-edit.png')
    page.set_viewport_size({'width': 1200, 'height': 900})
    page.click('#library-edit-form .library-edit-close')

    # 7. Touchscreen warnings and validation
    page.click('#library-add')
    form = '#library-edit-form '
    page.fill(form + '[name=vendor]', 'Bambu Lab')
    assert page.inner_text('#le-preview').startswith('Not shown on the touchscreen'), page.inner_text('#le-preview')
    assert page.is_enabled(form + 'button[type=submit]')
    page.screenshot(path=f'{OUT}/6-not-on-touchscreen.png')
    page.fill(form + '[name=vendor]', 'Over"ture')
    assert page.is_visible('#le-error') and page.is_disabled(form + 'button[type=submit]')
    page.fill(form + '[name=vendor]', 'Overture')
    page.fill('#le-type', 'PL"A')
    assert page.is_disabled(form + 'button[type=submit]')
    page.fill('#le-type', 'PLA')
    assert page.is_enabled(form + 'button[type=submit]') and not page.is_visible('#le-error')
    assert page.inner_text('#le-preview') == 'On the touchscreen: Overture → PLA'
    page.fill(form + '[name=sub_type]', 'Mat te')
    assert page.inner_text('#le-preview').startswith('On the touchscreen: Overture → PLA#Mat te')
    page.fill(form + '[name=sub_type]', 'Mat#te')
    assert page.inner_text('#le-preview').startswith('Not shown')

    # 8. 23 entries for one vendor -> limit warning
    state['library'] = json.dumps({'version': 1, 'filaments': [
        {'id': f'a{i}', 'name': f'Acme {i}', 'vendor': 'Acme', 'type': 'PLA'} for i in range(23)]})
    page.reload()
    page.wait_for_selector('text=Connected')
    page.click('#open-library')
    page.wait_for_selector('#library-list .spoolman-item')
    page.click('#library-add')
    assert not page.is_visible('#le-warning')
    page.fill(form + '[name=vendor]', 'acme')
    assert page.is_visible('#le-warning') and 'first 23' in page.inner_text('#le-warning')
    page.screenshot(path=f'{OUT}/7-vendor-limit.png')

    assert page.evaluate('document.documentElement.scrollWidth <= window.innerWidth')
    # The library 404 on first load is expected.
    errors = [e for e in errors if '404' not in e]
    print('errors:', errors)
    assert not errors
    b.close()
    print('OK')
