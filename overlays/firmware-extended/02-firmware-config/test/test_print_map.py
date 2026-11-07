#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
# SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

"""Browser test for print-map.js, the filament-per-tool dialog for Fluidd/Mainsail.

Loads the script into a stub page that starts prints the way Fluidd does
(JSON-RPC "printer.print.start" on /websocket), with a mocked Moonraker, and
checks the dialog, the start_local_print request and the reply to the UI.
Needs Python Playwright.

Usage: test_print_map.py [html_dir] [screenshot_dir]
"""
import json, os, sys, re, tempfile
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
HTML = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '../root/usr/local/share/firmware-config/html')
OUT = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix='print-map-')
os.makedirs(OUT, exist_ok=True)
print('screenshots:', OUT)

STUB = '''<!DOCTYPE html><html><body><div id="app"></div>
<script src="/firmware-config/print-map.js" defer></script>
<script>
// Just enough of Fluidd's Vue root and Vuex store
const subs = [];
window.store = {
    state: {config: {apiUrl: 'http://printer.test'}, dialogs: {}},
    subscribe: f => subs.push(f),
    commit(type, payload) { this.state.dialogs[type] = payload; subs.slice().forEach(f => f({type, payload})); },
};
window.routes = [];
document.getElementById('app').__vue__ = {
    $store: store, $route: {name: 'files'},
    $router: {push: r => { routes.push(r.name); return Promise.resolve(); }},
};
window.replies = [];
function connect(url) {
    const ws = new WebSocket(url);
    ws.onmessage = m => replies.push(JSON.parse(m.data));
    return new Promise(r => ws.addEventListener('open', () => r(ws)));
}
window.start = async (url, filename, id) => {
    const ws = url === 'local' ? (window.local = window.local || await connect('ws://printer.test/websocket'))
                               : await connect(url);
    ws.send(JSON.stringify({jsonrpc: '2.0', method: 'printer.print.start', params: {filename}, id}));
};
</script></body></html>'''

# Filament 1-3 used, 4 not (0 g); extruder 4 empty
ORCA = {
    'filament_type': 'PLA;PLA;PETG;PLA',
    'filament_colors': ['#FF0000', '#0000FF', '#00FF00', '#FFFFFF'],
    'filament_weight': [10.0, 5.0, 3.0, 0.0],
}
ptc = {
    'filament_vendor': ['Snapmaker', 'Generic', 'Snapmaker', 'NONE'],
    'filament_type': ['PLA', 'PETG', 'PLA', 'NONE'],
    'filament_sub_type': ['Basic', 'NONE', 'Matte', 'NONE'],
    'filament_color_rgba': ['0000FFFF', '00FF00FF', 'FF0000FF', 'FFFFFFFF'],
    'filament_exist': [True, True, True, False],
    'auto_bed_leveling': True, 'flow_calibrate': False, 'time_lapse_camera': False,
}
state = {'frames': [], 'starts': [], 'plain_starts': [], 'queries': 0, 'print_state': 'standby', 'start_result': None,
         'metadata': {'orca.gcode': ORCA, 'plain.gcode': {'estimated_time': 60},
                      'abs.gcode': {'filament_type': 'ABS', 'filament_colour': '#111111', 'filament_weight': [4.0]}}}

def ws_handler(ws):
    def on_msg(msg):
        m = json.loads(msg)
        if m['method'] == 'server.files.start_local_print':
            assert ws.url == 'ws://printer.test/websocket', ws.url
            state['starts'].append(m['params'])
            ws.send(json.dumps({'jsonrpc': '2.0', 'method': 'notify_status_update', 'params': []}))
            ws.send(json.dumps({'jsonrpc': '2.0', 'id': m['id'],
                                'result': state['start_result'] or {'state': 'success', 'message': 'Print started'}}))
            return
        state['frames'].append(m)
        ws.send(json.dumps({'jsonrpc': '2.0', 'id': m['id'], 'result': 'native'}))
    ws.on_message(on_msg)

def route(route):
    req = route.request
    path = re.sub(r'^https?://[^/]+', '', req.url).split('?')[0]
    query = req.url.split('?', 1)[1] if '?' in req.url else ''
    ok = lambda result: route.fulfill(status=200, content_type='application/json', body=json.dumps({'result': result}))
    if path == '/':
        return route.fulfill(status=200, body=STUB, content_type='text/html')
    if path.startswith('/firmware-config/'):
        return route.fulfill(status=200, body=open(os.path.join(HTML, path[len('/firmware-config/'):]), 'rb').read(),
                             content_type='application/javascript')
    if path == '/server/files/metadata':
        name = re.search(r'filename=([^&]*)', query).group(1)
        meta = state['metadata'].get(name)
        if meta is None:
            return route.fulfill(status=404, content_type='application/json', body='{"error": {"message": "nope"}}')
        return ok(dict(meta, filename=name))
    if path == '/printer/objects/query':
        state['queries'] += 1
        return ok({'status': {'print_task_config': ptc, 'print_stats': {'state': state['print_state']}}})
    if path == '/printer/print/start':
        assert req.method == 'POST'
        state['plain_starts'].append(re.search(r'filename=([^&]*)', query).group(1))
        return ok('ok')
    # Includes /server/files/start_local_print: Moonraker has it on the websocket only
    return route.fulfill(status=404, body='')

def wait_reply(page, rid):
    page.wait_for_function(f'replies.some(r => r.id === {json.dumps(rid)})')
    return page.evaluate(f'replies.find(r => r.id === {json.dumps(rid)})')

def pressed(page, tool):
    return page.eval_on_selector_all(f'.pm-row[data-tool="{tool}"] .pm-choice',
                                     'bs => bs.filter(b => b.getAttribute("aria-pressed") === "true").map(b => +b.dataset.extruder)')

def choices(page, tool):
    return page.eval_on_selector_all(f'.pm-row[data-tool="{tool}"] .pm-choice', 'bs => bs.map(b => +b.dataset.extruder)')

with sync_playwright() as pw:
    b = pw.chromium.launch()
    page = b.new_page(viewport={'width': 1000, 'height': 800})
    errors = []
    page.on('pageerror', lambda e: errors.append(str(e)))
    # The 404 for missing.gcode's metadata is expected
    page.on('console', lambda m: m.type == 'error' and 'missing.gcode' not in m.location.get('url', '')
            and errors.append(f"{m.text} {m.location.get('url', '')}"))
    page.route_web_socket(re.compile(r'.*/websocket'), ws_handler)
    page.route('**/*', route)
    page.goto('http://printer.test/')
    page.wait_for_function('typeof start === "function"')

    # 1. Dialog: used filaments only, same material only, closest colors by default
    page.evaluate('start("local", "orca.gcode", 1)')
    page.wait_for_selector('#print-map')
    assert page.eval_on_selector_all('.pm-row', 'rs => rs.map(r => +r.dataset.tool)') == [0, 1, 2]
    assert choices(page, 0) == [0, 2] and choices(page, 1) == [0, 2] and choices(page, 2) == [1]
    assert pressed(page, 0) == [2] and pressed(page, 1) == [0] and pressed(page, 2) == [1], \
        [pressed(page, t) for t in range(3)]
    assert page.is_checked('#print-map-bed-level')
    assert not page.is_checked('#print-map-flow-calibrate') and not page.is_checked('#print-map-timelapse')
    page.screenshot(path=f'{OUT}/1-dialog.png')

    # 2. Change filament 2 to extruder 3, turn flow calibration on, print
    page.click('.pm-row[data-tool="1"] .pm-choice[data-extruder="2"]')
    assert pressed(page, 1) == [2]
    page.check('#print-map-flow-calibrate')
    page.click('#print-map-print')
    assert wait_reply(page, 1) == {'jsonrpc': '2.0', 'id': 1, 'result': 'ok'}
    page.wait_for_selector('#print-map', state='detached')
    start = state['starts'][-1]
    print('start_local_print:', json.dumps(start))
    assert start == {'path': 'orca.gcode', 'print_plate': 1, 'options': {
        'map_table': [[0, 2], [1, 2], [2, 1]], 'bed_level': 1, 'flow_calibrate': 1,
        'flow_calibrate_extruders': [1, 2], 'time_lapse_camera': 0}}, start
    assert not state['frames'], state['frames']

    # 3. Start error keeps the dialog open; Cancel answers the UI with an error
    state['start_result'] = {'state': 'error', 'message': 'nozzle diameter mismatch'}
    page.evaluate('start("local", "orca.gcode", 2)')
    page.click('#print-map-print')
    page.wait_for_selector('#print-map-error:has-text("nozzle diameter mismatch")')
    page.screenshot(path=f'{OUT}/3-error.png')
    page.click('#print-map-cancel')
    assert wait_reply(page, 2)['error']['message'] == 'Print cancelled'
    assert not page.query_selector('#print-map') and not state['frames']
    state['start_result'] = None

    # 4. Escape cancels too
    page.evaluate('start("local", "orca.gcode", 3)')
    page.wait_for_selector('#print-map')
    page.keyboard.press('Escape')
    assert wait_reply(page, 3)['error']['message'] == 'Print cancelled'

    # 5. No matching material loaded: Print stays disabled
    page.evaluate('start("local", "abs.gcode", 4)')
    page.wait_for_selector('.pm-none:has-text("No ABS loaded")')
    assert page.is_disabled('#print-map-print')
    page.screenshot(path=f'{OUT}/5-no-match.png')
    page.click('#print-map-cancel')
    wait_reply(page, 4)

    # 6. Passed through unchanged: no filament metadata, no metadata, busy printer
    starts = len(state['starts'])
    for rid, name in ((5, 'plain.gcode'), (6, 'missing.gcode')):
        page.evaluate(f'start("local", "{name}", {rid})')
        assert wait_reply(page, rid)['result'] == 'native'
    state['print_state'] = 'printing'
    page.evaluate('start("local", "orca.gcode", 7)')
    assert wait_reply(page, 7)['result'] == 'native'
    state['print_state'] = 'standby'
    assert [f['params']['filename'] for f in state['frames']] == ['plain.gcode', 'missing.gcode', 'orca.gcode']
    assert all(f['method'] == 'printer.print.start' for f in state['frames'])

    # 7. Another printer's socket (Fluidd's printer switcher) is not touched
    queries = state['queries']
    page.evaluate('start("ws://other.test/websocket", "orca.gcode", 8)')
    assert wait_reply(page, 8)['result'] == 'native'
    assert state['queries'] == queries and len(state['starts']) == starts
    assert not page.query_selector('#print-map')

    # 8. Fluidd's AFC dialog is swapped for this one
    page.evaluate('store.commit("afc/setDialogState", {show: true, filename: "orca.gcode"})')
    page.wait_for_selector('#print-map')
    assert page.evaluate('store.state.dialogs["afc/setDialogState"].show') is False
    page.click('#print-map-print')
    page.wait_for_selector('#print-map', state='detached')
    page.wait_for_function('routes.length === 1')
    assert page.evaluate('routes') == ['home']
    assert state['starts'][-1]['path'] == 'orca.gcode' and len(state['starts']) == starts + 1

    # 9. Spoolman's spool dialog too; a file without filament info just starts
    page.evaluate('store.commit("spoolman/setDialogState", {show: true, filename: "plain.gcode"})')
    page.wait_for_function('routes.length === 2')
    assert state['plain_starts'] == ['plain.gcode'], state['plain_starts']
    assert page.evaluate('store.state.dialogs["spoolman/setDialogState"].show') is False
    assert not page.query_selector('#print-map')

    # 10. Cancel leaves Fluidd's dialog closed and starts nothing
    page.evaluate('store.commit("spoolman/setDialogState", {show: true, filename: "orca.gcode"})')
    page.wait_for_selector('#print-map')
    page.click('#print-map-cancel')
    page.wait_for_selector('#print-map', state='detached')
    assert len(state['starts']) == starts + 1 and page.evaluate('routes.length') == 2

    # 11. Fluidd's printer switcher on another printer: its dialog stays
    page.evaluate('store.state.config.apiUrl = "http://other.test:7125"')
    page.evaluate('store.commit("afc/setDialogState", {show: true, filename: "orca.gcode"})')
    page.wait_for_timeout(300)
    assert page.evaluate('store.state.dialogs["afc/setDialogState"].show') is True
    assert not page.query_selector('#print-map')

    assert not errors, errors
    b.close()
print('OK')
