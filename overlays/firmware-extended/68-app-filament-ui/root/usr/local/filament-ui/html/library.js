// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-PackageHomePage: https://github.com/paxx12-snapmaker-u1/SnapmakerU1-Extended-Firmware
// SPDX-FileCopyrightText: Copyright (c) 2026 @paxx12

'use strict';

// ── Filament library ──────────────────────────────────────────────────────

// Saved filaments, applied to an extruder in one step so the touchscreen shows
// them and loads/unloads at their temperatures. Stored in the printer config
// as `extended/filament_library.json`, which the patched
// `filament_parameters` Klipper module reads for load, unload and print
// temperatures. Depends on the helpers in script.js.

const LIBRARY_DIR = 'extended';
const LIBRARY_FILE = 'filament_library.json';
const LIBRARY_VERSION = 1;
// Entries per vendor the touchscreen can list.
const LIBRARY_VENDOR_LIMIT = 23;
// The touchscreen's own brands; they always come first, in this order.
const STOCK_BRANDS = ['Snapmaker', 'Polymaker', 'Generic'];

// Types the stock filament tables have parameters for, offered as the base
// type a custom type borrows flow and speed values from.
const BASE_TYPES = ['PLA', 'PLA-CF', 'PETG', 'PETG-CF', 'ABS', 'ASA', 'TPU',
                    'PA', 'PA-CF', 'PA-GF', 'PC', 'PC-ABS', 'PVA', 'PEBA'];

let libraryFilaments = [];
let libraryLoaded = false;
// Extruder the library was opened from; null when opened to manage entries.
let libraryChannel = null;
let libraryView = 'library';
let librarySpoolmanFilaments = [];
// Spoolman filament ids that have a spool (not archived, not empty); null
// when the spools could not be read.
let librarySpooledIds = null;
let libraryEditColors = [];
// Touchscreen order of the added brands; empty means alphabetical. Brands not
// listed follow the listed ones alphabetically, as in libgui-vendors.
let libraryBrandOrder = [];
let brandOrderDraft = [];

function moonrakerHeaders() {
    const jwt = getJWT();
    return jwt ? { Authorization: `Bearer ${jwt}` } : {};
}

async function loadLibrary() {
    const res = await fetch(`/server/files/config/${LIBRARY_DIR}/${LIBRARY_FILE}`,
                            { headers: moonrakerHeaders(), cache: 'no-store' });
    if (res.status === 404) {
        libraryFilaments = [];
        libraryBrandOrder = [];
    } else if (!res.ok) {
        throw new Error(`Failed to read ${LIBRARY_FILE} (HTTP ${res.status})`);
    } else {
        const data = await res.json();
        libraryFilaments = Array.isArray(data?.filaments) ? data.filaments.filter(f => f && f.type) : [];
        libraryBrandOrder = Array.isArray(data?.brand_order)
            ? data.brand_order.filter(b => typeof b === 'string').map(cleanText).filter(Boolean) : [];
    }
    libraryLoaded = true;
}

async function saveLibrary() {
    const data = { version: LIBRARY_VERSION, filaments: libraryFilaments };
    if (libraryBrandOrder.length) data.brand_order = libraryBrandOrder;
    const body = JSON.stringify(data, null, 2) + '\n';
    const form = new FormData();
    form.append('root', 'config');
    form.append('path', LIBRARY_DIR);
    form.append('file', new Blob([body], { type: 'application/json' }), LIBRARY_FILE);
    const res = await fetch('/server/files/upload', { method: 'POST', body: form, headers: moonrakerHeaders() });
    if (!res.ok) throw new Error(`Failed to save ${LIBRARY_FILE} (HTTP ${res.status})`);
}

function newLibraryId() {
    return Date.now().toString(36) + Math.random().toString(36).slice(2, 6);
}

// `PLA+` -> `PLA`, `PETG-HF` -> `PETG`; the longest base type the name starts with.
function guessBaseType(type) {
    const wanted = cleanText(type).toUpperCase();
    if (BASE_TYPES.includes(wanted)) return '';
    return BASE_TYPES.filter(base => wanted.startsWith(base))
                     .sort((a, b) => b.length - a.length)[0] || '';
}

function libraryColors(entry) {
    const colors = (entry.colors || []).map(normalizeColor).filter(Boolean);
    return colors.length ? colors.slice(0, FILAMENT_MAX_COLORS) : ['FFFFFF'];
}

function libraryDisplayName(entry) {
    return cleanText(entry.name) ||
        [entry.vendor, entry.type, entry.sub_type].map(cleanText).filter(Boolean).join(' ');
}

function libraryTempsLabel(entry) {
    const fmt = value => finiteNumber(value) != null ? `${value}°` : 'default';
    return `load ${fmt(entry.load_temp)} · unload ${fmt(entry.unload_temp)} · print ${fmt(entry.print_temp)}`;
}

// The touchscreen passes brand and material to Klipper unquoted, so whitespace
// and # * ; ' " make it skip the entry (subtype tolerates spaces). Entries it
// skips still apply from here, which quotes them.
function hiddenOnTouchscreen(vendor, type, subType) {
    return /[\s#*;'"]/.test(vendor || '') || /[\s#*;'"]/.test(type || '') || /[#*;'"]/.test(subType || '');
}

// Gcode string parameters cannot carry a double quote, and Klipper runs each
// line of a script as its own command, so a newline (e.g. in a Spoolman name)
// would inject gcode: drop control characters too.
function gcodeString(value) {
    return `"${cleanText(String(value ?? '').replace(/[\x00-\x1f\x7f]+/g, ' ')).replace(/"/g, '')}"`;
}

// ── Library dialog ────────────────────────────────────────────────────────

async function openLibrary(channel = null) {
    libraryChannel = channel;
    document.getElementById('library-modal-title').textContent =
        channel == null ? 'Filament Library' : `Filament Library — Extruder ${channel + 1}`;
    document.getElementById('library-intro').textContent = channel == null
        ? 'Saved filaments. Apply one from an extruder card so the touchscreen shows it and loads at its temperatures.'
        : 'Pick a filament to apply to this extruder. The touchscreen shows it and loads/unloads at its temperatures.';
    document.getElementById('library-search').value = '';
    document.getElementById('library-import').style.display = spoolmanActive ? '' : 'none';
    showLibraryView('library');
    openModal('library-modal');

    if (!libraryLoaded) {
        const list = document.getElementById('library-list');
        list.innerHTML = '<div class="spoolman-empty"><span class="spinner"></span> Loading…</div>';
        try {
            await loadLibrary();
        } catch (err) {
            list.innerHTML = `<div class="spoolman-empty">${escHtml(err.message)}</div>`;
            return;
        }
    }
    renderLibraryList();
}

function showLibraryView(view) {
    libraryView = view;
    const importing = view === 'spoolman';
    document.getElementById('library-import').textContent = importing ? '← Library' : 'Import from Spoolman';
    document.getElementById('library-add').style.display = importing ? 'none' : '';
    document.getElementById('library-order').style.display = importing ? 'none' : '';
    document.getElementById('library-import-all').style.display = importing ? '' : 'none';
    document.getElementById('library-spools-only-row').style.display = importing ? '' : 'none';
    if (!importing) document.getElementById('library-stale').style.display = 'none';
    document.getElementById('library-search').placeholder = importing
        ? 'Filter Spoolman filaments…' : 'Filter by name, brand, or material…';
}

function libraryItem(entry, { meta, note = '', right, onClick, actions = [] }) {
    const item = document.createElement('div');
    item.className = 'spoolman-item';
    item.innerHTML =
        `<div class="spoolman-swatches">${libraryColors(entry)
            .map(c => `<div class="spoolman-swatch" style="background:#${escHtml(c)}"></div>`).join('')}</div>` +
        `<div class="spoolman-info">` +
            `<div class="spoolman-name">${escHtml(libraryDisplayName(entry))}</div>` +
            `<div class="spoolman-meta">${escHtml(meta)}` +
                (note ? ` <span class="library-note">${escHtml(note)}</span>` : '') + `</div>` +
        `</div>` +
        `<div class="spoolman-right"><div class="spoolman-id">${escHtml(right)}</div></div>`;
    for (const [label, title, handler] of actions) {
        const btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'channel-action-btn library-item-btn';
        btn.textContent = label;
        btn.title = title;
        btn.addEventListener('click', e => { e.stopPropagation(); handler(); });
        item.appendChild(btn);
    }
    item.addEventListener('click', onClick);
    return item;
}

function matchesFilter(fields, filter) {
    const q = normalizeText(filter);
    return !q || fields.some(field => normalizeText(field).includes(q));
}

function renderLibraryList() {
    if (libraryView === 'spoolman') {
        renderSpoolmanImportList();
        return;
    }
    const filter = document.getElementById('library-search').value;
    const list = document.getElementById('library-list');
    list.innerHTML = '';
    const entries = libraryFilaments.filter(e =>
        matchesFilter([e.name, e.vendor, e.type, e.sub_type], filter));
    if (!entries.length) {
        list.innerHTML = `<div class="spoolman-empty">${libraryFilaments.length
            ? 'No filaments match' : 'No saved filaments yet. Add one, or import from Spoolman.'}</div>`;
        return;
    }
    for (const entry of entries) {
        const type = [entry.vendor, entry.type, entry.sub_type].map(cleanText).filter(Boolean).join(' · ');
        list.appendChild(libraryItem(entry, {
            meta: `${type} — ${libraryTempsLabel(entry)}`,
            note: hiddenOnTouchscreen(entry.vendor, entry.type, entry.sub_type) ? 'not shown on touchscreen' : '',
            right: entry.spoolman_filament_id ? `SM #${entry.spoolman_filament_id}` : '',
            onClick: () => libraryChannel == null ? openLibraryEdit(entry) : applyLibraryFilament(libraryChannel, entry),
            actions: libraryChannel == null ? [] : [['✎', 'Edit', () => openLibraryEdit(entry)]],
        }));
    }
}

// ── Apply ─────────────────────────────────────────────────────────────────

async function applyLibraryFilament(channel, entry) {
    const ch = channelsData.find(c => c.channel === channel);
    const sources = [];
    if (ch?.official) sources.push('an RFID tag');
    if (ch?.spool_id != null) sources.push(`Spoolman spool #${ch.spool_id}`);
    if (sources.length) {
        const accepted = await requestConfirmation(`Apply to Extruder ${channel + 1}?`, [
            { label: 'Filament', value: libraryDisplayName(entry) },
            { label: 'Replaces', value: `Filament set from ${sources.join(' and ')}` },
        ]);
        if (!accepted) return;
    }

    const colors = libraryColors(entry);
    const alpha = Number.isInteger(entry.alpha) ? Math.min(255, Math.max(0, entry.alpha)) : 255;
    const gcode = [
        'SET_PRINT_FILAMENT_CONFIG',
        `CONFIG_EXTRUDER=${channel}`,
        `VENDOR=${gcodeString(entry.vendor || 'Generic')}`,
        `FILAMENT_TYPE=${gcodeString(entry.type)}`,
        `FILAMENT_SUBTYPE=${gcodeString(entry.sub_type || '')}`,
        `COLOR_NUMS=${colors.length}`,
        `COLORS=${colors.join(',')}`,
        'MULTI_MODE=0',
        `ALPHA=${alpha}`,
        'FORCE=1',
    ].join(' ');

    try {
        showStatus(`Applying ${libraryDisplayName(entry)}…`, 'info');
        await sendGcode(gcode);
        closeModal('library-modal');
        showStatus(`Extruder ${channel + 1}: ${libraryDisplayName(entry)}`, 'success');
    } catch (err) {
        showStatus(`Failed: ${err.message}`, 'error');
    }
}

// ── Restart screen ────────────────────────────────────────────────────────

// The touchscreen reads the library when it starts, so changes show up there
// after the GUI restarts. firmware-config runs the restart as root.
function showRestartNotice() {
    document.getElementById('library-restart').style.display = '';
}

async function restartScreen() {
    const accepted = await requestConfirmation('Restart the screen?', [
        { label: 'Effect', value: 'The touchscreen reloads and is unavailable for up to a minute. Prints are not affected.' },
    ]);
    if (!accepted) return;
    try {
        showStatus('Restarting screen…', 'info');
        const res = await fetch('/firmware-config/api/action/restart-screen',
                                { method: 'POST', headers: moonrakerHeaders() });
        if (res.status === 404) throw new Error('Firmware config is not available, restart the screen from the printer');
        const text = await res.text();
        if (!res.ok || /^ERROR:/m.test(text)) throw new Error(`Restart failed (HTTP ${res.status})`);
        document.getElementById('library-restart').style.display = 'none';
        showStatus('Screen restarting, it is back in about a minute', 'success');
    } catch (err) {
        showStatus(err.message, 'error');
    }
}

// ── Spoolman import ───────────────────────────────────────────────────────

function spoolmanFilamentEntry(filament) {
    const multi = cleanText(filament.multi_color_hexes).split(',').map(normalizeColor).filter(Boolean);
    const single = normalizeColor(filament.color_hex);
    const type = cleanText(filament.material).toUpperCase();
    const entry = {
        name: cleanText(filament.name),
        vendor: cleanText(filament.vendor?.name) || 'Generic',
        type,
        sub_type: decodeExtraString(filament.extra?.variant || ''),
        base_type: guessBaseType(type),
        colors: multi.length ? multi.slice(0, FILAMENT_MAX_COLORS) : (single ? [single] : []),
        spoolman_filament_id: Number(filament.id),
    };
    const printTemp = finiteNumber(filament.settings_extruder_temp);
    if (printTemp != null && printTemp > 0) entry.print_temp = Math.round(printTemp);
    return entry;
}

// Updates the entry already imported from this Spoolman filament, keeping
// the load/unload temperatures and anything else set by hand.
function importSpoolmanFilament(filament) {
    const imported = spoolmanFilamentEntry(filament);
    if (!imported.type) return false;
    const existing = libraryFilaments.find(e => e.spoolman_filament_id === imported.spoolman_filament_id);
    if (existing) {
        if (!imported.base_type) delete imported.base_type;
        Object.assign(existing, imported);
    } else {
        libraryFilaments.push({ id: newLibraryId(), ...imported });
    }
    return true;
}

async function showSpoolmanImport() {
    showLibraryView('spoolman');
    document.getElementById('library-search').value = '';
    const list = document.getElementById('library-list');
    list.innerHTML = '<div class="spoolman-empty"><span class="spinner"></span> Loading Spoolman filaments…</div>';
    try {
        const [filaments, spools] = await Promise.all([
            fetchSpoolmanList('/v1/filament'),
            fetchSpoolmanList('/v1/spool').catch(() => null),
        ]);
        librarySpoolmanFilaments = filaments;
        librarySpooledIds = spools && new Set(spools
            .filter(sp => !sp.archived && (finiteNumber(sp.remaining_weight) ?? 1) > 0)
            .map(sp => Number(sp.filament?.id)).filter(Number.isFinite));
    } catch (err) {
        list.innerHTML = `<div class="spoolman-empty">${escHtml(`Failed to load Spoolman: ${err.message}`)}</div>`;
        return;
    }
    renderSpoolmanImportList();
}

function spoolsOnly() {
    return librarySpooledIds != null && document.getElementById('library-spools-only').checked;
}

// Spoolman filaments the import list shows: filtered by the search text and,
// unless unticked, to those with a spool.
function shownSpoolmanFilaments() {
    const filter = document.getElementById('library-search').value;
    return librarySpoolmanFilaments.filter(f =>
        (!spoolsOnly() || librarySpooledIds.has(Number(f.id))) &&
        matchesFilter([f.name, f.vendor?.name, f.material], filter));
}

// Library entries imported from a Spoolman filament that no longer has a spool.
function staleSpoolmanEntries() {
    if (librarySpooledIds == null) return [];
    const known = new Set(librarySpoolmanFilaments.map(f => Number(f.id)));
    return libraryFilaments.filter(e => e.spoolman_filament_id &&
        (!known.has(e.spoolman_filament_id) || !librarySpooledIds.has(e.spoolman_filament_id)));
}

function renderStaleNotice() {
    const stale = spoolsOnly() ? staleSpoolmanEntries() : [];
    document.getElementById('library-stale').style.display = stale.length ? '' : 'none';
    document.getElementById('library-stale-text').textContent =
        `${stale.length} library filament${stale.length === 1 ? ' was' : 's were'} imported from ` +
        `Spoolman and ${stale.length === 1 ? 'has' : 'have'} no spool left.`;
}

async function removeStaleSpoolmanEntries() {
    const stale = staleSpoolmanEntries();
    if (!stale.length) return;
    const accepted = await requestConfirmation('Remove from library?', [
        { label: 'Filaments', value: stale.map(libraryDisplayName).join(', ') },
        { label: 'Reason', value: 'Imported from Spoolman, no spool left (archived or empty)' },
    ]);
    if (!accepted) return;
    const previous = libraryFilaments;
    libraryFilaments = libraryFilaments.filter(e => !stale.includes(e));
    try {
        await saveLibrary();
        showRestartNotice();
        showStatus(`Removed ${stale.length} filament${stale.length === 1 ? '' : 's'}`, 'success');
    } catch (err) {
        libraryFilaments = previous;
        showStatus(err.message, 'error');
    }
    renderSpoolmanImportList();
}

function renderSpoolmanImportList() {
    const list = document.getElementById('library-list');
    list.innerHTML = '';
    renderStaleNotice();
    document.getElementById('library-spools-only').disabled = librarySpooledIds == null;
    const filaments = shownSpoolmanFilaments();
    if (!filaments.length) {
        list.innerHTML = `<div class="spoolman-empty">${spoolsOnly()
            ? 'No Spoolman filaments with a spool found' : 'No Spoolman filaments found'}</div>`;
        return;
    }
    const importedIds = new Set(libraryFilaments.map(e => e.spoolman_filament_id).filter(Boolean));
    for (const filament of filaments) {
        const entry = spoolmanFilamentEntry(filament);
        const imported = importedIds.has(entry.spoolman_filament_id);
        list.appendChild(libraryItem(entry, {
            meta: [entry.vendor, entry.type, entry.sub_type,
                   entry.print_temp ? `${entry.print_temp}°` : ''].filter(Boolean).join(' · '),
            note: hiddenOnTouchscreen(entry.vendor, entry.type, entry.sub_type) ? 'not shown on touchscreen' : '',
            right: imported ? `#${filament.id} ✓` : `#${filament.id}`,
            onClick: () => importSpoolmanFilaments([filament]),
        }));
    }
}

async function importSpoolmanFilaments(filaments) {
    // importSpoolmanFilament() updates entries in place: keep a copy so a
    // failed save does not leave unsaved changes for the next save to write.
    const previous = libraryFilaments.map(entry => ({ ...entry }));
    const count = filaments.filter(importSpoolmanFilament).length;
    if (!count) {
        libraryFilaments = previous;
        showStatus('Nothing to import: Spoolman filaments need a material', 'error');
        return;
    }
    try {
        await saveLibrary();
        showRestartNotice();
        showStatus(`Imported ${count} filament${count === 1 ? '' : 's'} from Spoolman`, 'success');
    } catch (err) {
        libraryFilaments = previous;
        showStatus(err.message, 'error');
    }
    renderSpoolmanImportList();
}

// ── Touchscreen brand order ───────────────────────────────────────────────

// Brands the library adds to the touchscreen (one spelling per brand,
// case-insensitive), in the order it shows them for `order`: listed brands
// first, the rest alphabetically. Same rules as libgui-vendors.
function addedBrands(order) {
    const stock = new Set(STOCK_BRANDS.map(normalizeText));
    const brands = new Map();
    for (const e of libraryFilaments) {
        const vendor = cleanText(e.vendor) || 'Generic';
        const key = normalizeText(vendor);
        if (stock.has(key) || brands.has(key) || hiddenOnTouchscreen(vendor, e.type, e.sub_type)) continue;
        brands.set(key, vendor);
    }
    const rank = brand => {
        const i = order.findIndex(b => normalizeText(b) === normalizeText(brand));
        return i < 0 ? order.length : i;
    };
    return [...brands.values()].sort((a, b) => rank(a) - rank(b) ||
        a.localeCompare(b, undefined, { sensitivity: 'base' }));
}

function openBrandOrder() {
    brandOrderDraft = libraryBrandOrder.length ? addedBrands(libraryBrandOrder) : [];
    document.querySelector(`input[name=brand-order-mode][value=${brandOrderDraft.length ? 'custom' : 'alpha'}]`)
        .checked = true;
    renderBrandOrder();
    openModal('brand-order-modal');
}

function renderBrandOrder() {
    const custom = document.querySelector('input[name=brand-order-mode][value=custom]').checked;
    const brands = custom ? brandOrderDraft : addedBrands([]);
    const list = document.getElementById('brand-order-list');
    list.innerHTML = '';
    for (const brand of STOCK_BRANDS) {
        const li = document.createElement('li');
        li.className = 'fixed';
        li.innerHTML = `<span>${escHtml(brand)}</span>`;
        list.appendChild(li);
    }
    if (!brands.length) {
        const li = document.createElement('li');
        li.className = 'fixed';
        li.innerHTML = '<span>No added brands yet</span>';
        list.appendChild(li);
    }
    brands.forEach((brand, i) => {
        const li = document.createElement('li');
        li.innerHTML = `<span>${escHtml(brand)}</span>`;
        if (custom) {
            for (const [label, title, to] of [['↑', 'Move up', i - 1], ['↓', 'Move down', i + 1]]) {
                const btn = document.createElement('button');
                btn.type = 'button';
                btn.className = 'channel-action-btn';
                btn.textContent = label;
                btn.title = title;
                btn.setAttribute('aria-label', `${title}: ${brand}`);
                btn.disabled = to < 0 || to >= brands.length;
                btn.addEventListener('click', () => {
                    [brandOrderDraft[i], brandOrderDraft[to]] = [brandOrderDraft[to], brandOrderDraft[i]];
                    renderBrandOrder();
                });
                li.appendChild(btn);
            }
        }
        list.appendChild(li);
    });
}

async function saveBrandOrder() {
    const custom = document.querySelector('input[name=brand-order-mode][value=custom]').checked;
    const previous = libraryBrandOrder;
    libraryBrandOrder = custom ? [...brandOrderDraft] : [];
    try {
        await saveLibrary();
        showRestartNotice();
        closeModal('brand-order-modal');
        showStatus(custom ? 'Saved brand order' : 'Brands sorted alphabetically', 'success');
    } catch (err) {
        libraryBrandOrder = previous;
        showStatus(err.message, 'error');
    }
}

// ── Edit dialog ───────────────────────────────────────────────────────────

function fillDatalist(id, values) {
    document.getElementById(id).innerHTML = values
        .map(value => `<option value="${escHtml(value)}"></option>`).join('');
}

function renderLibraryEditColors() {
    const list = document.getElementById('le-colors');
    list.innerHTML = '';
    libraryEditColors.forEach((color, i) => {
        const group = document.createElement('div');
        group.className = 'color-input-group';
        const picker = document.createElement('input');
        picker.type = 'color';
        picker.value = `#${color}`;
        picker.setAttribute('aria-label', `Colour ${i + 1}`);
        picker.addEventListener('input', e => { libraryEditColors[i] = normalizeColor(e.target.value); });
        group.appendChild(picker);
        if (libraryEditColors.length > 1) {
            const remove = document.createElement('button');
            remove.type = 'button';
            remove.className = 'channel-action-btn';
            remove.textContent = '×';
            remove.title = 'Remove colour';
            remove.addEventListener('click', () => { libraryEditColors.splice(i, 1); renderLibraryEditColors(); });
            group.appendChild(remove);
        }
        list.appendChild(group);
    });
    document.getElementById('le-color-add').disabled = libraryEditColors.length >= FILAMENT_MAX_COLORS;
}

function openLibraryEdit(entry = null) {
    const form = document.getElementById('library-edit-form');
    form.reset();
    document.getElementById('library-edit-title').textContent = entry ? 'Edit Filament' : 'Add Filament';
    document.getElementById('library-delete').style.display = entry ? '' : 'none';
    form.elements.id.value = entry?.id || '';
    form.elements.name.value = entry?.name || '';
    form.elements.vendor.value = entry?.vendor || 'Generic';
    form.elements.type.value = entry?.type || 'PLA';
    form.elements.sub_type.value = entry?.sub_type || '';
    form.elements.base_type.value = entry?.base_type || '';
    for (const key of ['load_temp', 'unload_temp', 'print_temp']) {
        form.elements[key].value = finiteNumber(entry?.[key]) ?? '';
    }
    form.elements.is_soft.value = entry?.is_soft == null ? '' : (entry.is_soft ? '1' : '0');
    libraryEditColors = entry ? libraryColors(entry) : ['FFFFFF'];
    renderLibraryEditColors();
    syncLibraryBaseType();
    openModal('library-edit-modal');
}

// Shows how the touchscreen will list the filament and warns when the vendor
// is already at the touchscreen's per-vendor limit.
function updateLibraryPreview() {
    const form = document.getElementById('library-edit-form');
    const vendor = cleanText(form.elements.vendor.value) || 'Generic';
    const type = cleanText(form.elements.type.value).toUpperCase();
    const sub = cleanText(form.elements.sub_type.value);

    const preview = document.getElementById('le-preview');
    const hidden = hiddenOnTouchscreen(vendor, type, sub);
    preview.classList.toggle('library-warning', hidden);
    preview.textContent = hidden
        ? 'Not shown on the touchscreen: brand and material can\'t contain spaces or # * ; \' ", ' +
          'and subtype can\'t contain # * ; \' ". It still applies from here.'
        : `On the touchscreen: ${vendor} → ${sub ? `${type}#${sub}` : type}`;

    const invalid = /"/.test(`${vendor}${type}${sub}`);
    const error = document.getElementById('le-error');
    error.style.display = invalid ? '' : 'none';
    error.textContent = 'Brand, material and subtype can\'t contain double quotes.';
    form.querySelector('button[type=submit]').disabled = invalid;

    const others = libraryFilaments.filter(f =>
        f.id !== form.elements.id.value && normalizeText(f.vendor) === normalizeText(vendor)).length;
    const warning = document.getElementById('le-warning');
    warning.style.display = others >= LIBRARY_VENDOR_LIMIT ? '' : 'none';
    warning.textContent = `${vendor} already has ${others} filaments. The touchscreen shows the first ` +
        `${LIBRARY_VENDOR_LIMIT} materials per brand (some firmware versions allow 26).`;
}

// Base type only matters for types the stock tables don't know.
function syncLibraryBaseType() {
    const form = document.getElementById('library-edit-form');
    const known = BASE_TYPES.includes(cleanText(form.elements.type.value).toUpperCase());
    document.getElementById('le-base-type-row').style.display = known ? 'none' : '';
    if (!known && !form.elements.base_type.value) {
        form.elements.base_type.value = guessBaseType(form.elements.type.value);
    }
    updateLibraryPreview();
}

async function handleLibrarySave(e) {
    e.preventDefault();
    const form = e.target;
    const id = form.elements.id.value;
    const type = cleanText(form.elements.type.value).toUpperCase();
    const entry = {
        id: id || newLibraryId(),
        name: cleanText(form.elements.name.value),
        vendor: cleanText(form.elements.vendor.value) || 'Generic',
        type,
        sub_type: cleanText(form.elements.sub_type.value),
        colors: libraryEditColors.filter(Boolean),
    };
    if (!BASE_TYPES.includes(type) && form.elements.base_type.value) {
        entry.base_type = form.elements.base_type.value;
    }
    for (const key of ['load_temp', 'unload_temp', 'print_temp']) {
        const value = finiteNumber(form.elements[key].value);
        if (value != null && value > 0) entry[key] = Math.round(value);
    }
    if (form.elements.is_soft.value !== '') entry.is_soft = form.elements.is_soft.value === '1';

    const existing = libraryFilaments.find(f => f.id === id);
    if (existing?.spoolman_filament_id) entry.spoolman_filament_id = existing.spoolman_filament_id;
    const previous = libraryFilaments;
    libraryFilaments = existing
        ? libraryFilaments.map(f => (f === existing ? entry : f))
        : [...libraryFilaments, entry];

    try {
        await saveLibrary();
        showRestartNotice();
        closeModal('library-edit-modal');
        showStatus(`Saved ${libraryDisplayName(entry)}`, 'success');
    } catch (err) {
        libraryFilaments = previous;
        showStatus(err.message, 'error');
    }
    renderLibraryList();
}

async function handleLibraryDelete() {
    const id = document.getElementById('library-edit-form').elements.id.value;
    const entry = libraryFilaments.find(f => f.id === id);
    if (!entry) return;
    const accepted = await requestConfirmation('Delete filament?', [
        { label: 'Filament', value: libraryDisplayName(entry) },
    ]);
    if (!accepted) return;
    const previous = libraryFilaments;
    libraryFilaments = libraryFilaments.filter(f => f !== entry);
    try {
        await saveLibrary();
        showRestartNotice();
        closeModal('library-edit-modal');
        showStatus(`Deleted ${libraryDisplayName(entry)}`, 'success');
    } catch (err) {
        libraryFilaments = previous;
        showStatus(err.message, 'error');
    }
    renderLibraryList();
}

function initializeLibrary() {
    fillDatalist('le-vendor-list', BRAND_ITEMS);
    fillDatalist('le-type-list', MATERIALS.map(([name]) => name));
    fillDatalist('le-subtype-list', SUBTYPE_ITEMS.map(itemValue).filter(Boolean));
    document.getElementById('le-base-type').innerHTML =
        '<option value="">None</option>' +
        BASE_TYPES.map(t => `<option value="${t}">${t}</option>`).join('');

    document.getElementById('open-library').addEventListener('click', () => openLibrary());
    document.getElementById('library-close').addEventListener('click', () => closeModal('library-modal'));
    document.getElementById('library-modal').addEventListener('click', e => {
        if (e.target.id === 'library-modal') closeModal('library-modal');
    });
    document.getElementById('library-restart-btn').addEventListener('click', restartScreen);
    document.getElementById('library-search').addEventListener('input', renderLibraryList);
    document.getElementById('library-add').addEventListener('click', () => openLibraryEdit());
    document.getElementById('library-import').addEventListener('click', () => {
        if (libraryView === 'spoolman') {
            showLibraryView('library');
            document.getElementById('library-search').value = '';
            renderLibraryList();
        } else {
            showSpoolmanImport();
        }
    });
    document.getElementById('library-import-all').addEventListener('click', () =>
        importSpoolmanFilaments(shownSpoolmanFilaments()));
    document.getElementById('library-spools-only').addEventListener('change', renderSpoolmanImportList);
    document.getElementById('library-stale-btn').addEventListener('click', removeStaleSpoolmanEntries);

    document.getElementById('library-order').addEventListener('click', openBrandOrder);
    document.querySelectorAll('.brand-order-close').forEach(btn =>
        btn.addEventListener('click', () => closeModal('brand-order-modal')));
    document.getElementById('brand-order-modal').addEventListener('click', e => {
        if (e.target.id === 'brand-order-modal') closeModal('brand-order-modal');
    });
    document.querySelectorAll('input[name=brand-order-mode]').forEach(radio =>
        radio.addEventListener('change', () => {
            brandOrderDraft = radio.value === 'custom' ? addedBrands([]) : [];
            renderBrandOrder();
        }));
    document.getElementById('brand-order-save').addEventListener('click', saveBrandOrder);

    document.querySelectorAll('.library-edit-close').forEach(btn =>
        btn.addEventListener('click', () => closeModal('library-edit-modal')));
    document.getElementById('library-edit-modal').addEventListener('click', e => {
        if (e.target.id === 'library-edit-modal') closeModal('library-edit-modal');
    });
    document.getElementById('library-edit-form').addEventListener('submit', handleLibrarySave);
    document.getElementById('library-delete').addEventListener('click', handleLibraryDelete);
    document.getElementById('le-type').addEventListener('input', syncLibraryBaseType);
    document.getElementById('library-edit-form').addEventListener('input', updateLibraryPreview);
    document.getElementById('le-color-add').addEventListener('click', () => {
        if (libraryEditColors.length < FILAMENT_MAX_COLORS) {
            libraryEditColors.push('FFFFFF');
            renderLibraryEditColors();
        }
    });
}

document.addEventListener('DOMContentLoaded', initializeLibrary);
