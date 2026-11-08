const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.join(__dirname, '..');
const script = fs.readFileSync(path.join(root, 'html/noise-reduction.js'), 'utf8');
function fixture() {
    const elements = Object.fromEntries(['nrEnabled', 'nrStrength', 'nrStrengthValue', 'nrStatus'].map(id =>
        [id, {disabled: true, checked: false, value: id === 'nrStrength' ? '60' : '',
            addEventListener(name, callback) { this[name] = callback; }}]));
    const timers = new Map(); let timer = 0;
    const context = {document: {getElementById: id => elements[id]}, WebSocket: {OPEN: 1},
        setTimeout: callback => {timers.set(++timer, callback); return timer;},
        clearTimeout: id => timers.delete(id)};
    vm.runInNewContext(script, context);
    const socket = {readyState: 1, sent: [], send(data) { this.sent.push(Array.from(new Float32Array(data))); }};
    const control = context.NoiseReductionControl;
    return {elements, socket, control, timers, reply(percent, effective = percent) {
        control.handleState(new Float32Array([8, 1, percent, effective]));
    }};
}
const f = fixture(), g = fixture();
f.control.connect(f.socket);
assert.deepEqual(f.socket.sent, [[5, -1]]);
assert.equal(f.elements.nrEnabled.disabled, true);
f.reply(0);
assert.equal(f.elements.nrEnabled.checked, false);
assert.equal(f.elements.nrEnabled.disabled, false);
f.elements.nrEnabled.checked = true; f.elements.nrEnabled.change();
assert.deepEqual(f.socket.sent.at(-1), [5, 60]);
f.reply(60);
assert.equal(f.elements.nrStatus.textContent, 'Aktiv: 60 %');
f.elements.nrStrength.value = '80'; f.elements.nrStrength.input();
f.reply(60); // Earlier reply must not overwrite the later slider event.
assert.equal(f.elements.nrStrength.value, '80');
f.reply(80);
f.elements.nrEnabled.checked = false; f.elements.nrEnabled.change(); f.reply(0);
assert.deepEqual(f.socket.sent.at(-1), [5, 0]);
assert.equal(f.elements.nrStrength.value, '80');
f.elements.nrEnabled.checked = true; f.elements.nrEnabled.change(); f.reply(80, 0);
assert.match(f.elements.nrStatus.textContent, /FM: pausiert/);
f.reply(80); assert.equal(f.elements.nrStatus.textContent, 'Aktiv: 80 %');
assert.equal(g.socket.sent.length, 0);
f.control.disconnect();
f.reply(100);
assert.equal(f.elements.nrEnabled.disabled, true);
assert.equal(f.elements.nrStatus.textContent, 'Nicht verbunden');
f.control.connect(f.socket); f.reply(0);
assert.equal(f.elements.nrEnabled.checked, false); // Reconnect starts a fresh listener.
f.control.disconnect(); f.control.connect(f.socket);
for (const callback of f.timers.values()) callback();
assert.match(f.elements.nrStatus.textContent, /nicht bestätigt/);
for (const page of ['index.html', 'fftmobile.html']) {
    const html = fs.readFileSync(path.join(root, 'html', page), 'utf8');
    assert.equal((html.match(/id="nrEnabled"/g) || []).length, 1);
    assert.match(html, /src="noise-reduction.js"/);
    const inline = html.match(/<script>([\s\S]*?)<\/script>/)[1];
    new vm.Script(inline);
    const update = inline.match(/    function updateFFT\([^]*?\n    }/)[0];
    const frames = [];
    const context = {NoiseReductionControl: {handleState: frame => frames.push(frame)}};
    vm.runInNewContext(update, context);
    const data = new Float32Array(1025); data.set([8, 1, 70, 70]);
    context.updateFFT(data.buffer);
    assert.equal(frames.length, 1, page);
    assert.equal(frames[0][2], 70);
}
console.log('Desktop/mobile NR: query, confirmed strength, bypass, stale replies, FM pause, isolation and reconnect passed.');
