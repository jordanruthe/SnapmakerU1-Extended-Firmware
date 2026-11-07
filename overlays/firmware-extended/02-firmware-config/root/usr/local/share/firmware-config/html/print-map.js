// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
// SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

// Loaded by Fluidd's and Mainsail's index.html (see inject_print_map.sh). Asks
// which loaded filament each slicer tool should use before a print starts, like
// the touchscreen does, along with its bed leveling, flow calibration and
// timelapse switches.
// Both UIs start prints with JSON-RPC "printer.print.start" on Moonraker's
// websocket, which runs a plain SDCARD_PRINT_FILE. This holds that request,
// shows the dialog, and starts the print through the touchscreen's
// "server.files.start_local_print" (SDCARD_PRINT_FILE_WITH_PARAMETERS
// MAP_TABLE=...) instead, then answers the held request so the UI carries on.
// Files without per-tool filament metadata, and sockets to another printer,
// are left alone.
// Fluidd opens its own AFC (AFC-Lite) or Spoolman spool dialog first when
// those are present; those are swapped for this one so only one dialog shows,
// by watching Fluidd's Vuex store (nav.js reaches it the same way).
(function () {
    'use strict';

    const MARKER = 'data-print-map';
    const nativeSend = WebSocket.prototype.send;
    let uiSocket = null;
    let rpcId = 0;

    function getJWT() {
        const instanceKey = `user-token-${window.location.host.replace(/[^a-zA-Z0-9]/g, '_')}`;
        let token = localStorage.getItem(instanceKey);
        if (token) return token;
        for (let i = 0; i < localStorage.length; i++) {
            const key = localStorage.key(i);
            if (key && key.startsWith('user-token-')) {
                token = localStorage.getItem(key);
                if (token) return token;
            }
        }
        return null;
    }

    async function api(method, path, body) {
        const headers = { 'Accept': 'application/json' };
        const jwt = getJWT();
        if (jwt) headers['Authorization'] = `Bearer ${jwt}`;
        if (body) headers['Content-Type'] = 'application/json';
        const resp = await fetch(path, { method, headers, body: body && JSON.stringify(body) });
        const data = await resp.json().catch(() => ({}));
        if (!resp.ok) throw new Error((data.error && data.error.message) || `HTTP ${resp.status}`);
        return data.result;
    }

    // Moonraker only offers some methods (start_local_print) to websocket
    // clients; over HTTP they are a 404. These go over the UI's own socket, with
    // an id of ours that Fluidd and Mainsail ignore in the reply.
    function rpc(method, params) {
        const ws = uiSocket;
        if (!ws || ws.readyState !== WebSocket.OPEN) return Promise.reject(new Error('Not connected to Moonraker'));
        const id = `print-map-${++rpcId}`;
        return new Promise((resolve, reject) => {
            const done = (settle, value) => {
                ws.removeEventListener('message', onMessage);
                ws.removeEventListener('close', onClose);
                settle(value);
            };
            const onMessage = e => {
                const msg = JSON.parse(e.data);
                if (msg.id !== id) return;
                if (msg.error) done(reject, new Error(msg.error.message));
                else done(resolve, msg.result);
            };
            const onClose = () => done(reject, new Error('Lost connection to Moonraker'));
            ws.addEventListener('message', onMessage);
            ws.addEventListener('close', onClose);
            nativeSend.call(ws, JSON.stringify({ jsonrpc: '2.0', method, params, id }));
        });
    }

    // This host's Moonraker, not another printer's (Fluidd's printer switcher)
    function isLocalSocket(ws) {
        try {
            const url = new URL(ws.url);
            return url.host === window.location.host && url.pathname.endsWith('/websocket');
        } catch (e) {
            return false;
        }
    }

    // The held request, if it is a print start on this host's Moonraker
    function printStartRequest(ws, data) {
        if (typeof data !== 'string' || !data.includes('printer.print.start')) return null;
        if (!isLocalSocket(ws)) return null;
        let msg;
        try {
            msg = JSON.parse(data);
        } catch (e) {
            return null;
        }
        if (!msg || msg.method !== 'printer.print.start' || msg.id === undefined) return null;
        const filename = msg.params && msg.params.filename;
        return typeof filename === 'string' && filename ? { id: msg.id, filename } : null;
    }

    function reply(ws, id, payload) {
        const data = JSON.stringify(Object.assign({ jsonrpc: '2.0', id }, payload));
        ws.dispatchEvent(new MessageEvent('message', { data }));
    }

    function list(value) {
        if (Array.isArray(value)) return value;
        if (typeof value === 'string' && value) return value.split(';');
        return [];
    }

    function hex(color) {
        const m = /^#?([0-9a-f]{6})/i.exec(String(color || '').trim());
        return m ? m[1].toUpperCase() : null;
    }

    // Same weighting as print_task_config._weighted_rgb_distance (auto replenish)
    function distance(a, b) {
        if (!a || !b) return Infinity;
        const weights = [0.299, 0.587, 0.114];
        let d = 0;
        for (let i = 0; i < 3; i++) {
            d += (weights[i] * (parseInt(a.substr(i * 2, 2), 16) - parseInt(b.substr(i * 2, 2), 16))) ** 2;
        }
        return d;
    }

    function same(a, b) {
        return String(a || '').trim().toUpperCase() === String(b || '').trim().toUpperCase();
    }

    // Slicer tools the file uses: [{tool, type, color}]
    function usedTools(meta) {
        const types = list(meta.filament_type);
        const colors = list(meta.filament_colors).length ? list(meta.filament_colors) : list(meta.filament_colour);
        const weights = list(meta.filament_weights).length ? list(meta.filament_weights) : list(meta.filament_weight);
        const tools = [];
        for (let i = 0; i < types.length; i++) {
            if (weights.length && !(Number(weights[i]) > 0)) continue;
            tools.push({ tool: i, type: types[i].trim(), color: hex(colors[i]) });
        }
        return tools;
    }

    // Loaded extruders: [{index, vendor, type, subType, color}]
    function loadedExtruders(ptc) {
        const out = [];
        const types = ptc.filament_type || [];
        for (let i = 0; i < types.length; i++) {
            if (!(ptc.filament_exist || [])[i] || !types[i] || same(types[i], 'NONE')) continue;
            const sub = (ptc.filament_sub_type || [])[i];
            out.push({
                index: i,
                vendor: (ptc.filament_vendor || [])[i],
                type: types[i],
                subType: same(sub, 'NONE') ? '' : sub,
                color: hex((ptc.filament_color_rgba || [])[i]),
            });
        }
        return out;
    }

    // Closest color pairs first, each extruder used once while any are free
    // (ties go to the same extruder number); leftover tools share the closest
    function defaultMap(tools, extruders) {
        const map = {};
        const taken = new Set();
        const choices = t => extruders.filter(e => same(e.type, t.type));
        const better = (a, b) => !b || a.d < b.d || (a.d === b.d && a.moved < b.moved);
        for (;;) {
            let best = null;
            for (const t of tools) {
                if (t.tool in map) continue;
                for (const e of choices(t)) {
                    if (taken.has(e.index)) continue;
                    const pair = { t, e, d: distance(t.color, e.color), moved: e.index === t.tool ? 0 : 1 };
                    if (better(pair, best)) best = pair;
                }
            }
            if (!best) break;
            map[best.t.tool] = best.e.index;
            taken.add(best.e.index);
        }
        for (const t of tools) {
            if (t.tool in map) continue;
            let best = null;
            for (const e of choices(t)) {
                const pair = { e, d: distance(t.color, e.color), moved: e.index === t.tool ? 0 : 1 };
                if (better(pair, best)) best = pair;
            }
            if (best) map[t.tool] = best.e.index;
        }
        return map;
    }

    const STYLE = `
[${MARKER}] { position: fixed; inset: 0; z-index: 100000; display: flex; align-items: center;
  justify-content: center; background: rgba(0,0,0,.6); font-family: Roboto, system-ui, sans-serif; }
[${MARKER}] .pm-card { background: #1e1e1e; color: #eee; border-radius: 8px; width: min(560px, calc(100vw - 32px));
  max-height: calc(100vh - 32px); overflow: auto; padding: 20px; box-shadow: 0 8px 32px rgba(0,0,0,.5); }
[${MARKER}] h2 { margin: 0 0 4px; font-size: 18px; font-weight: 500; }
[${MARKER}] .pm-file { margin: 0 0 16px; font-size: 13px; color: #aaa; word-break: break-all; }
[${MARKER}] .pm-row { display: flex; gap: 12px; align-items: flex-start; padding: 10px 0; border-top: 1px solid #333; }
[${MARKER}] .pm-tool { display: flex; gap: 8px; align-items: center; min-width: 120px; font-size: 14px; }
[${MARKER}] .pm-swatch { width: 22px; height: 22px; border-radius: 50%; border: 1px solid #666; flex: none; }
[${MARKER}] .pm-choices { display: flex; flex-wrap: wrap; gap: 6px; flex: 1; }
[${MARKER}] .pm-choice { display: flex; gap: 6px; align-items: center; padding: 6px 10px; border-radius: 6px;
  border: 2px solid #444; background: #2a2a2a; color: #eee; cursor: pointer; font: inherit; font-size: 13px; }
[${MARKER}] .pm-choice[aria-pressed="true"] { border-color: #2196f3; background: #1d3448; }
[${MARKER}] .pm-none { font-size: 13px; color: #ff8a80; }
[${MARKER}] .pm-options { border-top: 1px solid #333; padding-top: 12px; display: grid; gap: 8px; }
[${MARKER}] .pm-options label { display: flex; gap: 8px; align-items: center; font-size: 14px; cursor: pointer; }
[${MARKER}] .pm-error { color: #ff8a80; font-size: 13px; margin-top: 12px; white-space: pre-wrap; }
[${MARKER}] .pm-actions { display: flex; justify-content: flex-end; gap: 8px; margin-top: 16px; }
[${MARKER}] .pm-actions button { padding: 8px 16px; border-radius: 4px; border: 0; font: inherit; font-size: 14px;
  cursor: pointer; text-transform: uppercase; letter-spacing: .05em; }
[${MARKER}] #print-map-cancel { background: transparent; color: #eee; }
[${MARKER}] #print-map-print { background: #2196f3; color: #fff; }
[${MARKER}] #print-map-print:disabled { background: #444; color: #888; cursor: default; }`;

    function el(tag, className, text) {
        const node = document.createElement(tag);
        if (className) node.className = className;
        if (text) node.textContent = text;
        return node;
    }

    function swatch(color) {
        const s = el('span', 'pm-swatch');
        s.style.background = color ? `#${color}` : 'transparent';
        return s;
    }

    function toggle(id, label, checked) {
        const row = el('label');
        const box = el('input');
        box.type = 'checkbox';
        box.id = id;
        box.checked = !!checked;
        row.append(box, document.createTextNode(label));
        return row;
    }

    // Resolves with {map, bedLevel, flowCalibrate, timelapse}, or null if cancelled;
    // start() is called with that and returns an error message or null
    function showDialog(filename, tools, extruders, ptc, start) {
        return new Promise(resolve => {
            const map = defaultMap(tools, extruders);
            let busy = false;
            const root = el('div');
            root.setAttribute(MARKER, '');
            root.id = 'print-map';
            const style = el('style');
            style.textContent = STYLE;
            const card = el('div', 'pm-card');
            card.setAttribute('role', 'dialog');
            card.setAttribute('aria-modal', 'true');
            card.append(el('h2', '', 'Filament for each tool'), el('p', 'pm-file', filename));

            const printBtn = el('button', '', 'Print');
            printBtn.id = 'print-map-print';
            const update = () => {
                printBtn.disabled = busy || tools.some(t => !(t.tool in map));
                for (const btn of card.querySelectorAll('.pm-choice')) {
                    btn.setAttribute('aria-pressed', String(map[btn.dataset.tool] === Number(btn.dataset.extruder)));
                }
            };

            for (const t of tools) {
                const row = el('div', 'pm-row');
                row.dataset.tool = t.tool;
                const tool = el('div', 'pm-tool');
                tool.append(swatch(t.color), el('span', '', `Filament ${t.tool + 1} \u00B7 ${t.type}`));
                const choices = el('div', 'pm-choices');
                const matching = extruders.filter(e => same(e.type, t.type));
                for (const e of matching) {
                    const btn = el('button', 'pm-choice');
                    btn.type = 'button';
                    btn.dataset.tool = t.tool;
                    btn.dataset.extruder = e.index;
                    const name = [e.vendor && !same(e.vendor, 'NONE') ? e.vendor : '', e.type, e.subType]
                        .filter(Boolean).join(' ');
                    btn.append(swatch(e.color), el('span', '', `Extruder ${e.index + 1} \u00B7 ${name}`));
                    btn.addEventListener('click', () => { map[t.tool] = e.index; update(); });
                    choices.appendChild(btn);
                }
                if (!matching.length) {
                    choices.appendChild(el('span', 'pm-none', `No ${t.type} loaded`));
                }
                row.append(tool, choices);
                card.appendChild(row);
            }

            const options = el('div', 'pm-options');
            options.append(
                toggle('print-map-bed-level', 'Auto bed leveling', ptc.auto_bed_leveling),
                toggle('print-map-flow-calibrate', 'Flow calibration', ptc.flow_calibrate),
                toggle('print-map-timelapse', 'Timelapse', ptc.time_lapse_camera));
            card.appendChild(options);

            const error = el('div', 'pm-error');
            error.id = 'print-map-error';
            const cancelBtn = el('button', '', 'Cancel');
            cancelBtn.id = 'print-map-cancel';
            const actions = el('div', 'pm-actions');
            actions.append(cancelBtn, printBtn);
            card.append(error, actions);
            root.append(style, card);

            const close = result => {
                document.removeEventListener('keydown', onKey, true);
                root.remove();
                resolve(result);
            };
            const onKey = e => {
                if (e.key === 'Escape' && !busy) { e.stopPropagation(); close(null); }
            };
            cancelBtn.addEventListener('click', () => { if (!busy) close(null); });
            printBtn.addEventListener('click', async () => {
                const choice = {
                    map: Object.assign({}, map),
                    bedLevel: card.querySelector('#print-map-bed-level').checked,
                    flowCalibrate: card.querySelector('#print-map-flow-calibrate').checked,
                    timelapse: card.querySelector('#print-map-timelapse').checked,
                };
                busy = true;
                error.textContent = '';
                update();
                const message = await start(choice);
                busy = false;
                if (message === null) return close(choice);
                error.textContent = message;
                update();
            });
            document.addEventListener('keydown', onKey, true);
            update();
            document.body.appendChild(root);
        });
    }

    async function startPrint(filename, choice) {
        const used = [...new Set(Object.values(choice.map))].sort((a, b) => a - b);
        const options = {
            map_table: Object.entries(choice.map).map(([tool, extruder]) => [Number(tool), extruder]),
            bed_level: choice.bedLevel ? 1 : 0,
            flow_calibrate: choice.flowCalibrate ? 1 : 0,
            flow_calibrate_extruders: used,
            time_lapse_camera: choice.timelapse ? 1 : 0,
        };
        try {
            const result = await rpc('server.files.start_local_print',
                { path: filename, print_plate: 1, options });
            if (result && result.state === 'error') return result.message || 'Print failed to start';
            return null;
        } catch (e) {
            return e.message || String(e);
        }
    }

    // 'skip' (not a multi-filament file, or printer busy), 'started' or 'cancelled'
    async function askAndPrint(filename) {
        let meta, status;
        try {
            [meta, status] = await Promise.all([
                api('GET', `/server/files/metadata?filename=${encodeURIComponent(filename)}`),
                api('GET', '/printer/objects/query?print_task_config&print_stats'),
            ]);
        } catch (e) {
            return 'skip';
        }
        const ptc = status && status.status && status.status.print_task_config;
        const state = status && status.status && status.status.print_stats && status.status.print_stats.state;
        const tools = meta ? usedTools(meta) : [];
        if (!ptc || !tools.length || state === 'printing' || state === 'paused') return 'skip';

        const choice = await showDialog(filename, tools, loadedExtruders(ptc), ptc,
            c => startPrint(filename, c));
        return choice ? 'started' : 'cancelled';
    }

    async function handlePrintStart(ws, data, req) {
        const outcome = await askAndPrint(req.filename);
        if (outcome === 'skip') return nativeSend.call(ws, data);
        if (ws.readyState !== WebSocket.OPEN) return;
        if (outcome === 'started') {
            reply(ws, req.id, { result: 'ok' });
        } else {
            reply(ws, req.id, { error: { code: 400, message: 'Print cancelled' } });
        }
    }

    WebSocket.prototype.send = function (data) {
        if (isLocalSocket(this)) uiSocket = this;
        const req = printStartRequest(this, data);
        if (!req || document.querySelector(`[${MARKER}]`)) return nativeSend.call(this, data);
        handlePrintStart(this, data, req).catch(e => {
            console.error('print-map:', e);
            if (this.readyState === WebSocket.OPEN) {
                reply(this, req.id, { error: { code: 500, message: String(e && e.message || e) } });
            }
        });
    };

    // host[:port] of a URL that may lack a scheme; default ports dropped
    function hostOf(url) {
        const m = /^(?:[a-z][a-z0-9+.-]*:\/\/)?([^/?#]+)/i.exec(String(url || '').trim());
        return m ? m[1].toLowerCase().replace(/:(80|443)$/, '') : '';
    }

    // Fluidd dialogs that open on Print instead of starting it
    const FLUIDD_DIALOGS = ['afc/setDialogState', 'spoolman/setDialogState'];

    async function replaceFluiddDialog(vm, filename) {
        let outcome = await askAndPrint(filename);
        if (outcome === 'skip') {
            try {
                await api('POST', `/printer/print/start?filename=${encodeURIComponent(filename)}`);
                outcome = 'started';
            } catch (e) {
                window.alert(`Print failed to start: ${e.message || e}`);
            }
        }
        // Fluidd goes to the dashboard once a print starts
        if (outcome === 'started' && vm.$route && vm.$route.name !== 'home') {
            vm.$router.push({ name: 'home' }).catch(() => {});
        }
    }

    function hookFluidd() {
        const app = document.getElementById('app');
        const vm = app && app.__vue__;
        const store = vm && vm.$store;
        if (!store || !store.state || !store.state.config) return false;
        store.subscribe(mutation => {
            const p = mutation.payload;
            if (!FLUIDD_DIALOGS.includes(mutation.type) || !p || !p.show || !p.filename) return;
            // Printer switcher on another printer: leave Fluidd alone
            const apiUrl = store.state.config.apiUrl;
            if (hostOf(apiUrl) && hostOf(apiUrl) !== hostOf(window.location.host)) return;
            if (document.querySelector(`[${MARKER}]`)) return;
            store.commit(mutation.type, Object.assign({}, p, { show: false }));
            replaceFluiddDialog(vm, p.filename).catch(e => console.error('print-map:', e));
        });
        return true;
    }

    // Fluidd mounts after this deferred script runs; Mainsail has no such store
    let tries = 0;
    const timer = setInterval(() => {
        if (hookFluidd() || ++tries > 120) clearInterval(timer);
    }, 500);
})();
