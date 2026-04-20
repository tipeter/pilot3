'use strict';

/* ═══════════════════════════════════════════════════════════════════════════
 * SSOT: token store
 * All token access goes through these two functions.
 * ═══════════════════════════════════════════════════════════════════════════ */
const Token = (() => {
    let _token = localStorage.getItem('pilot_token') || '';
    return {
        get()        { return _token; },
        set(t)       { _token = t; localStorage.setItem('pilot_token', t); },
        authHeader() { return { 'Authorization': 'Bearer ' + _token }; },
    };
})();

/* ═══════════════════════════════════════════════════════════════════════════
 * SSOT: format helpers
 * Single place for all display-format conversions.
 * ═══════════════════════════════════════════════════════════════════════════ */
const Fmt = {
    bytes(b) {
        if (b == null) return '—';
        if (b < 1024)       return b + ' B';
        if (b < 1048576)    return (b / 1024).toFixed(1) + ' KB';
        return                     (b / 1048576).toFixed(2) + ' MB';
    },
    uptime(s) {
        if (s == null) return '—';
        const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
        return `${h}h ${m}m ${sec}s`;
    },
};

/* ═══════════════════════════════════════════════════════════════════════════
 * SSOT: API client
 * All HTTP calls go through apiFetch(). Auth header injected once here.
 * ═══════════════════════════════════════════════════════════════════════════ */
const Api = {
    async fetch(path, opts = {}) {
        opts.headers = Object.assign(Token.authHeader(), opts.headers || {});
        const r = await fetch(path, opts);
        if (r.status === 401) { UI.setDot('err'); throw new Error('Unauthorized'); }
        return r;
    },
    async getJSON(path)       { return (await this.fetch(path)).json(); },
    async postJSON(path, body) {
        return this.fetch(path, {
            method:  'POST',
            headers: { 'Content-Type': 'application/json' },
            body:    JSON.stringify(body),
        });
    },
};

/* ═══════════════════════════════════════════════════════════════════════════
 * SSOT: UI state helpers
 * Centralised DOM update functions – no direct getElementById outside here.
 * ═══════════════════════════════════════════════════════════════════════════ */
const UI = {
    $: (id) => document.getElementById(id),
    setDot(state) {
        const d = this.$('conn-dot');
        d.className = 'status-dot ' + (state || '');
    },
    setOtaStatus(msg, type) {
        const el = this.$('ota-status');
        el.textContent = msg;
        el.className   = type;
    },
    setProgress(rx, total) {
        const pct = total > 0 ? Math.round(rx / total * 100) : 0;
        this.$('prog-fill').style.width  = pct + '%';
        this.$('prog-bytes').textContent = Fmt.bytes(rx) + ' / ' + Fmt.bytes(total);
        this.$('prog-pct').textContent   = pct + '%';
        this.$('prog-wrap').classList.add('visible');
    },
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: SystemPanel
 * Single responsibility: fetch and render /api/v1/system.
 * ═══════════════════════════════════════════════════════════════════════════ */
const SystemPanel = {
    async refresh() {
        try {
            const d = await Api.getJSON('/api/v1/system');
            UI.$('s-fw').textContent     = d.fw_version   || '—';
            UI.$('s-idf').textContent    = d.idf_version  || '—';
            UI.$('s-ip').textContent     = d.ip           || '—';
            UI.$('s-rssi').textContent   = d.rssi_dbm != null ? d.rssi_dbm + ' dBm' : '—';
            UI.$('s-uptime').textContent = Fmt.uptime(d.uptime_s);
            UI.$('s-heap').textContent   = Fmt.bytes(d.free_heap);
            UI.$('s-mheap').textContent  = Fmt.bytes(d.min_heap);
            UI.$('s-psram').textContent  = Fmt.bytes(d.free_psram);
            UI.$('s-lfs').textContent    = d.lfs_mounted
                ? Fmt.bytes(d.lfs_used) + ' / ' + Fmt.bytes(d.lfs_total)
                : 'Not mounted';
            UI.setDot('ok');
        } catch (_) { UI.setDot('err'); }
    },
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: OtaPanel
 * Single responsibility: OTA upload, rollback, and status display.
 * ═══════════════════════════════════════════════════════════════════════════ */
const OtaPanel = {
    _file: null,

    init() {
        const input = UI.$('fw-file');
        const dz    = UI.$('drop-zone');

        input.addEventListener('change', () => this._setFile(input.files[0]));

        dz.addEventListener('dragover',  (e) => { e.preventDefault(); dz.classList.add('drag'); });
        dz.addEventListener('dragleave', ()  => dz.classList.remove('drag'));
        dz.addEventListener('drop', (e) => {
            e.preventDefault(); dz.classList.remove('drag');
            const f = e.dataTransfer.files[0];
            if (f && f.name.endsWith('.bin')) {
                const dt = new DataTransfer(); dt.items.add(f);
                input.files = dt.files;
                this._setFile(f);
            }
        });
    },

    _setFile(f) {
        if (!f) return;
        this._file = f;
        UI.$('file-name').textContent = f.name + ' (' + Fmt.bytes(f.size) + ')';
        UI.$('drop-zone').classList.add('has-file');
        UI.$('btn-upload').disabled = false;
    },

    async loadStatus() {
        try {
            const d      = await Api.getJSON('/api/v1/ota/status');
            const states = ['undefined','pending_verify','valid','invalid','aborted','new'];
            UI.$('ota-meta').innerHTML =
                `<span class="chip">v<span>${d.version || '?'}</span></span>` +
                `<span class="chip">partition: <span>${d.partition || '?'}</span></span>` +
                `<span class="chip">state: <span>${states[d.ota_state] || d.ota_state}</span></span>` +
                `<span class="chip">built: <span>${d.date || '?'}</span></span>`;
        } catch (_) {}
    },

    async upload() {
        if (!this._file) return;
        const btn = UI.$('btn-upload');
        btn.disabled = true;
        UI.setProgress(0, this._file.size);
        UI.setOtaStatus('Uploading firmware…', 'info');
        try {
            const r = await Api.fetch('/api/v1/ota', {
                method:  'POST',
                headers: { 'Content-Type': 'application/octet-stream',
                           'Content-Length': this._file.size },
                body:    this._file,
                duplex:  'half',
            });
            if (r.ok) {
                UI.setOtaStatus('Upload accepted. Device rebooting…', 'ok');
                UI.setProgress(this._file.size, this._file.size);
            } else {
                UI.setOtaStatus('Error: ' + await r.text(), 'err');
                btn.disabled = false;
            }
        } catch (e) {
            UI.setOtaStatus('Network error: ' + e.message, 'err');
            btn.disabled = false;
        }
    },

    async rollback() {
        if (!confirm('Roll back to the previous firmware and reboot?')) return;
        try {
            const r = await Api.fetch('/api/v1/ota/rollback', { method: 'POST' });
            const d = await r.json();
            UI.setOtaStatus(d.message || 'Rollback initiated.', r.ok ? 'ok' : 'err');
        } catch (e) { UI.setOtaStatus('Rollback error: ' + e.message, 'err'); }
    },
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: ConfigPanel
 * Single responsibility: NVS config display and editing.
 * ═══════════════════════════════════════════════════════════════════════════ */
const ConfigPanel = {
    async refresh() {
        UI.$('config-empty').textContent = 'Loading…';
        try {
            const data = await Api.getJSON('/api/v1/config');
            this._render(data);
        } catch (_) { UI.$('config-empty').textContent = 'Load failed.'; }
    },

    _render(data) {
        const tbody = UI.$('config-body');
        tbody.innerHTML = '';
        const keys = Object.keys(data);
        UI.$('config-empty').style.display = keys.length ? 'none' : 'block';
        keys.forEach(k => {
            const tr = document.createElement('tr');
            tr.innerHTML =
                `<td>${k}</td>` +
                `<td><input type="text" value="${data[k]}" data-key="${k}" onchange="ConfigPanel._markDirty(this)"></td>` +
                `<td><button class="config-save-btn" data-key="${k}" onclick="ConfigPanel._save(this)">Save</button></td>`;
            tbody.appendChild(tr);
        });
    },

    _markDirty(input) {
        input.closest('tr').querySelector('.config-save-btn').style.display = 'inline-block';
    },

    async _save(btn) {
        const key   = btn.dataset.key;
        const raw   = btn.closest('tr').querySelector('input').value;
        const value = isNaN(raw) ? raw : Number(raw);
        const r     = await Api.postJSON('/api/v1/config', { key, value });
        if (r.ok) btn.style.display = 'none';
    },

    async addEntry() {
        const key = UI.$('new-key').value.trim();
        const raw = UI.$('new-val').value.trim();
        if (!key || !raw) return;
        await Api.postJSON('/api/v1/config', { key, value: isNaN(raw) ? raw : Number(raw) });
        UI.$('new-key').value = '';
        UI.$('new-val').value = '';
        this.refresh();
    },
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: LogPanel
 * Single responsibility: WebSocket log stream display.
 * ═══════════════════════════════════════════════════════════════════════════ */
const LogPanel = {
    _ws:       null,
    _retry:    null,
    _lines:    0,
    MAX_LINES: 500,

    connect() {
        if (this._ws && this._ws.readyState < 2) this._ws.close();
        if (this._retry) { clearTimeout(this._retry); this._retry = null; }

        const proto = location.protocol === 'https:' ? 'wss' : 'ws';
        const url   = `${proto}://${location.host}/ws/log?token=${encodeURIComponent(Token.get())}`;
        this._ws    = new WebSocket(url);

        const statusEl = UI.$('ws-status');
        this._ws.onopen  = () => { statusEl.textContent = 'Connected';    statusEl.className = 'connected'; };
        this._ws.onclose = () => { statusEl.textContent = 'Disconnected'; statusEl.className = ''; this._retry = setTimeout(() => this.connect(), 5000); };
        this._ws.onerror = () => {};
        this._ws.onmessage = (ev) => this._onMessage(ev.data);
    },

    _onMessage(raw) {
        try {
            const msg = JSON.parse(raw);
            if      (msg.type === 'log')          this._appendLine(msg.msg);
            else if (msg.type === 'ota_progress')  UI.setProgress(msg.bytes, msg.total);
            else if (msg.type === 'ota_done')      UI.setOtaStatus('OTA complete – rebooting…', 'ok');
            else if (msg.type === 'ota_rollback')  UI.setOtaStatus('Rollback – rebooting…', 'info');
        } catch (_) { this._appendLine(raw); }
    },

    _appendLine(line) {
        const con  = UI.$('log-console');
        const span = document.createElement('span');
        const lvl  = line.match(/\b([IWED])\s*\(/)?.[1] || 'I';
        span.className   = 'l-' + lvl;
        span.textContent = line + '\n';
        con.appendChild(span);
        if (++this._lines > this.MAX_LINES) { con.firstChild && con.removeChild(con.firstChild); this._lines--; }
        if (UI.$('chk-scroll').checked) con.scrollTop = con.scrollHeight;
    },

    clear() { UI.$('log-console').innerHTML = ''; this._lines = 0; },
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Module: App
 * Single responsibility: bootstrap and periodic refresh orchestration.
 * ═══════════════════════════════════════════════════════════════════════════ */
const App = {
    init() {
        /* Restore token into the input field. */
        if (Token.get()) UI.$('token-input').value = Token.get();

        OtaPanel.init();

        if (Token.get()) this._start();
    },

    connect() {
        Token.set(UI.$('token-input').value.trim());
        UI.setDot('ok');
        this._start();
    },

    _start() {
        /* Stagger API calls by 300 ms each to avoid opening 4+ parallel TLS
         * sessions simultaneously, which exhausts the ESP32 mbedTLS heap.
         * Each call reuses the same TLS session if keep-alive is active. */
        SystemPanel.refresh();
        setTimeout(() => OtaPanel.loadStatus(),  300);
        setTimeout(() => ConfigPanel.refresh(),   600);
        setTimeout(() => LogPanel.connect(),      900);
        /* Stagger periodic refresh the same way. */
        setInterval(() => SystemPanel.refresh(), 30000);
    },
};

/* Expose callbacks used by inline HTML onclick attributes. */
window.App          = App;
window.OtaPanel     = OtaPanel;
window.ConfigPanel  = ConfigPanel;
window.LogPanel     = LogPanel;

document.addEventListener('DOMContentLoaded', () => App.init());
