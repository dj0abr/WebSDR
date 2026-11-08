// Run the actual browser connection setup in a small DOM/WebSocket fixture.
// No network, npm modules or SDR hardware are required.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const cases = [
    ['sdr.db0sl.de', 'https:', 'wss://ws.db0sl.de'],
    ['sdr.db0sl.de', 'http:', 'wss://ws.db0sl.de'],
    ['ws.db0sl.de', 'https:', 'wss://ws.db0sl.de'],
    ['db0sl.de', 'https:', 'wss://db0sl.de'],
    ['www.radio.example.co.uk', 'https:', 'wss://ws.example.co.uk'],
    ['example.co.uk', 'https:', 'wss://example.co.uk'],
    ['sdr.example.edu.au', 'https:', 'wss://ws.example.edu.au'],
    ['sdr.example.gov.uk', 'https:', 'wss://ws.example.gov.uk'],
    ['sdr.example.ac.uk', 'https:', 'wss://ws.example.ac.uk'],
    ['sdr.example.org.au', 'https:', 'wss://ws.example.org.au'],
    ['sdr.example.com.au', 'https:', 'wss://ws.example.com.au'],
    ['localhost', 'http:', 'ws://localhost:9001'],
    ['127.0.0.1', 'http:', 'ws://127.0.0.1:9001'],
    ['192.168.1.12', 'http:', 'ws://192.168.1.12:9001'],
    ['10.1.2.3', 'http:', 'ws://10.1.2.3:9001'],
    ['172.16.0.2', 'http:', 'ws://172.16.0.2:9001'],
    ['172.20.0.2', 'http:', 'ws://172.20.0.2:9001'],
    ['172.31.255.2', 'http:', 'ws://172.31.255.2:9001'],
];
function fixture(page, savedUsername = '', hostname = 'localhost', protocol = 'http:') {
    const html = fs.readFileSync(path.join(__dirname, '../../html', page), 'utf8');
    const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
    new vm.Script(script);
    assert.match(html, /id="bandBox" disabled/);
    const extract = name => {
        const match = script.match(new RegExp('    function ' + name + '\\([^]*?\\n    }'));
        assert.ok(match, name);
        return match[0];
    };
    const elements = Object.fromEntries(['username', 'password', 'ft8Status', 'bandBox', 'modeBox'].map(id =>
        [id, {value: '', textContent: '', disabled: id === 'bandBox', listeners: {},
            addEventListener(event, callback) {this.listeners[event] = callback;}}]));
    const storage = new Map([['username', savedUsername], ['password', 'obsolete-password']]);
    const timers = new Map(), connections = [], loads = [];
    let timerId = 0, received;
    class WebSocket {
        static CONNECTING = 0; static OPEN = 1; static CLOSED = 3;
        constructor(url) {this.url = url; this.readyState = 0; this.sent = []; connections.push(this);}
        send(buffer) {assert.equal(this.readyState, 1); this.sent.push(new Float32Array(buffer));}
        open() {this.readyState = 1; this.onopen?.();}
        close() {this.readyState = 3; this.onclose?.();}
    }
    const context = {
        window: {location: {hostname, protocol, host: `${hostname}:8080`},
            addEventListener(event, callback) {assert.equal(event, 'load'); loads.push(callback);}},
        document: {getElementById: id => {assert.ok(elements[id], id); return elements[id];}},
        localStorage: {getItem: key => storage.get(key), setItem: (key, value) => storage.set(key, value)},
        console: {log() {}, error() {}}, WebSocket, TextEncoder,
        socket: undefined, reconnectTimer: null, reconnectInterval: 2000, audioQueue: [],
        bandBox: elements.bandBox,
        setTimeout: callback => {timers.set(++timerId, callback); return timerId;},
        clearTimeout: id => timers.delete(id),
        updateFFT: data => {received = data;},
        updateInfoVisibility() {}, drawTuningLine() {}, draw_connlines() {},
        makeLabels480() {}, makeLabels48() {}, printQRG() {}, getHamRadioBand: () => 20,
    };
    const names = ['getCallsign', 'isValidCallsign', 'syncLogin', 'setupWebSocket', 'retryConnection',
        'sendUserPW', 'sendBandToServer', 'sendModeToServer', 'sendFreqOffsetToServer', 'handle_config'];
    const start = script.indexOf('    function saveToLocalStorage()');
    const end = script.indexOf(page === 'index.html' ? '    function toggleMenu()' : '    window.onload', start);
    vm.runInNewContext(names.map(extract).join('\n') + '\n' + script.slice(start, end), context);
    return {context, elements, connections, timers, storage, received: () => received,
        load: () => loads.forEach(callback => callback()),
        input: value => {elements.username.value = value; elements.username.listeners.input();},
        runTimers: () => {for (const [id, callback] of [...timers]) {timers.delete(id); callback();}}};
}
for (const page of ['index.html', 'fftmobile.html']) {
    for (const [hostname, protocol, expected] of cases) {
        const f = fixture(page, '  dj0abr/p  ', hostname, protocol);
        f.load();
        assert.equal(f.connections.length, 1);
        const socket = f.connections[0];
        assert.equal(socket.url, expected, hostname);
        assert.equal(socket.binaryType, 'arraybuffer');
        socket.open();
        assert.match(f.elements.ft8Status.textContent, /Warte auf Empfänger/);
        assert.equal(socket.sent[0][0], 4);
        assert.equal(String.fromCharCode(...socket.sent[0].slice(1)), 'DJ0ABR/P:DJ0ABR/P');
        const frame = new Float32Array(1025); frame[0] = 7;
        socket.onmessage({data: frame.buffer});
        assert.equal(f.received(), frame.buffer, 'FT8 frames must reach the browser dispatcher');
        f.context.handle_config([14000000, 0, 1, 14000000, 14350000, 1]);
        assert.equal(f.elements.bandBox.disabled, false);
        f.context.handle_config([14000000, 0, 1, 14000000, 14350000, 2]);
        assert.equal(f.elements.bandBox.disabled, true);
        socket.close();
        assert.equal(f.timers.size, 1);
        assert.equal(f.elements.ft8Status.textContent, 'Verbindung unterbrochen');
        f.runTimers();
        assert.equal(f.connections.length, 2);
        f.connections[1].open();
        assert.equal(f.connections[1].sent[0][0], 4, 'Reconnect must send login immediately');
    }
    const f = fixture(page);
    f.load();
    for (const value of ['', '   ', '\t\n ', 'D1', '///', '1234', 'DL 1ABC', 'DL1ABC,DL2ABC', 'DL1ABC:PW', 'A'.repeat(33)]) {
        f.input(value);
        f.context.sendUserPW();
        f.context.sendBandToServer(20);
        f.context.sendModeToServer(1);
        f.context.sendFreqOffsetToServer(240000);
        assert.equal(f.connections.length, 0, JSON.stringify(value));
        assert.equal(f.timers.size, 0);
        assert.equal(f.elements.bandBox.disabled, true);
        const saved = fixture(page, value); saved.load();
        assert.equal(saved.connections.length, 0, 'Invalid stored callsign must not connect');
    }
    f.input('dl1abc');
    const first = f.connections[0];
    f.input('dl1abc/p');
    assert.equal(f.connections.length, 1, 'Typing must not duplicate connecting sockets');
    first.open();
    assert.equal(String.fromCharCode(...first.sent[0].slice(1)), 'DL1ABC/P:DL1ABC/P');
    f.input('DL2ABC');
    assert.equal(f.connections.length, 1, 'Callsign update must reuse open socket');
    assert.equal(String.fromCharCode(...first.sent.at(-1).slice(1)), 'DL2ABC:DL2ABC');
    f.input('  ');
    assert.equal(first.readyState, 3);
    f.runTimers();
    assert.equal(f.connections.length, 1, 'Clearing callsign must close without reconnect');
    f.input('SWL');
    f.connections[1].open();
    f.connections[1].close();
    const staleRetry = [...f.timers.values()][0];
    f.input('');
    assert.equal(f.timers.size, 0, 'Clearing callsign must cancel pending reconnect');
    staleRetry();
    assert.equal(f.connections.length, 2);
    f.input('DL3ABC');
    const connecting = f.connections[2];
    const staleOpen = connecting.onopen;
    const staleMessage = connecting.onmessage;
    f.input('');
    staleOpen(); staleMessage({data: new ArrayBuffer(4)});
    assert.equal(connecting.sent.length, 0, 'Cancelled connection must not send a late login');
    assert.equal(f.received(), undefined);
    f.context.handle_config([14000000, 0, 1, 14000000, 14350000, 1]);
    assert.equal(f.elements.bandBox.disabled, true);
    console.log(`${page}: ${cases.length} gateway cases, login validation, login on open/reconnect, logout, stale callbacks and band lock passed.`);
}
