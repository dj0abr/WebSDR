// Exercise real CTY data, message parsing, and both production FT8 tables.
// No network, npm modules or SDR hardware are required.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const htmlDirectory = path.join(__dirname, '../../html');
const dataScript = fs.readFileSync(path.join(htmlDirectory, 'ft8-country-data.js'), 'utf8');
const lookupScript = fs.readFileSync(path.join(htmlDirectory, 'ft8-countries.js'), 'utf8');
const lookupContext = {};
vm.runInNewContext(dataScript + '\n' + lookupScript, lookupContext);
const {countryForCallsign, senderCallsign} = lookupContext.FT8Countries;
assert.ok(lookupContext.FT8CountryData.entities.length >= 300);

const countries = [
    ['DJ0ABR', 'Deutschland'], ['F5NNN', 'Frankreich'], ['YO4DWD', 'Rumänien'],
    ['ON4KBU', 'Belgien'], ['JA4FEN', 'Japan'], ['LZ2VQ', 'Bulgarien'],
    ['K1ABC', 'USA'], ['KH6ABC', 'Hawaii'], ['KL7ABC', 'Alaska'],
    ['KG4AA', 'Guantanamo Bay'], ['KG4ABC', 'USA'], ['KG4A', 'USA'],
    ['EA8ABC', 'Kanaren'], ['EA6ABC', 'Balearen'], ['EA1ABC', 'Spanien'],
    ['RA6ABO', 'Russland (EU)'], ['UA0ABC', 'Russland (AS)'],
    ['4U1UN', 'United Nations HQ'], ['4U1ITU', 'ITU HQ'],
    ['SV2RSG/A', 'Athos'], ['SV2RSG', 'Griechenland'],
    ['DL1ABC/P', 'Deutschland'], ['DL1ABC/M', 'Deutschland'],
    ['DL1ABC/QRP', 'Deutschland'], ['DL1ABC/P/QRP', 'Deutschland'],
    ['F/DL1ABC', 'Frankreich'], ['F/DL1ABC/P', 'Frankreich'],
    ['DL1ABC/F', 'Frankreich'], ['DL1ABC/EA8', 'Kanaren'],
    ['EA8/DL1ABC', 'Kanaren'], ['W4/G0XYZ', 'USA'],
    ['K1ABC/VE6', 'Kanada'], ['EA8ABC/1', 'Spanien'],
    ['<yo4dwd>', 'Rumänien'], ['<...>', ''], ['', ''],
    ['JO42', ''], ['R-10', ''], ['73', ''], ['HELLO', ''],
    ['DL1ABC/MM', ''], ['DL1ABC/AM', ''], ['XX/DL1ABC', ''],
    ['DL1ABC/XX', ''], ['DL1ABC/ON4KBU', ''],
];
for (const [call, expected] of countries) assert.equal(countryForCallsign(call), expected, call);

const messages = [
    ['CQ F5NNN IN94', 'F5NNN', 'Frankreich'],
    ['CQ DX DJ0ABR JN68', 'DJ0ABR', 'Deutschland'],
    ['CQ JA YO4DWD KN34', 'YO4DWD', 'Rumänien'],
    ['CQ TEST K1ABC FN42', 'K1ABC', 'USA'],
    ['CQ 123 KH6ABC BL11', 'KH6ABC', 'Hawaii'],
    ['CQ EA8/DL1ABC', 'EA8/DL1ABC', 'Kanaren'],
    ['ON4KBU YO4DWD RR73', 'YO4DWD', 'Rumänien'],
    ['YL3QT LZ2VQ 73', 'LZ2VQ', 'Bulgarien'],
    ['M1NIG 5B4AHO RR73', '5B4AHO', 'Zypern'],
    ['<ON4KBU> YO4DWD -11', 'YO4DWD', 'Rumänien'],
    ['ON4KBU <YO4DWD> R-12', 'YO4DWD', 'Rumänien'],
    ['<...> YO4DWD -12', 'YO4DWD', 'Rumänien'],
    ['R2BRU <...> -02', '', ''],
    ['CQ <...> IN94', '', ''],
    ['TU; ON4KBU YO4DWD R 599 001', 'YO4DWD', 'Rumänien'],
    ['K1ABC RR73; W9XYZ <KH1/KH7Z> -11', 'KH1/KH7Z', 'Baker & Howland Islands'],
    ['K1ABC RR73; W9XYZ <...> -11', '', ''],
    ['DE F/DL1ABC -12', 'F/DL1ABC', 'Frankreich'],
    ['QRZ K1ABC FN42', 'K1ABC', 'USA'],
    ['TNX K1ABC 73', '', ''], ['HELLO WORLD', '', ''], ['', '', ''],
];
for (const [message, sender, country] of messages) {
    assert.equal(senderCallsign(message), sender, message);
    assert.equal(countryForCallsign(senderCallsign(message)), country, message);
}
console.log(`${countries.length} country/portable cases and ${messages.length} transmitter cases passed.`);

class Element {
    constructor(tag) { this.tag = tag; this.children = []; this.textContent = ''; }
    appendChild(child) {
        if (child.tag === 'fragment') this.children.push(...child.children);
        else this.children.push(child);
    }
    get rows() { return this.children; }
}

for (const page of ['index.html', 'fftmobile.html']) {
    const html = fs.readFileSync(path.join(htmlDirectory, page), 'utf8');
    const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
    new vm.Script(script);
    const header = html.match(/<table id="ft8Table">\s*<thead>([\s\S]*?)<\/thead>/)[1];
    assert.equal((header.match(/<th\b/g) || []).length, 6);
    assert.ok(header.includes('>Land</th>'));
    assert.ok(!header.includes('NF Hz'));
    assert.ok(html.includes('src="ft8-country-data.js"'));
    assert.ok(html.includes('src="ft8-countries.js"'));
    assert.ok(html.indexOf('src="ft8-country-data.js"') < html.indexOf('src="ft8-countries.js"'));
    assert.ok(html.indexOf('src="ft8-countries.js"') < html.indexOf('<script>'));
    const extract = name => script.match(new RegExp('    function ' + name + '\\([^]*?\\n    }'))[0];
    for (const loaded of [true, false]) {
        const elements = Object.fromEntries(['ft8Status', 'ft8Empty', 'ft8Rows', 'ft8Count'].map(id => [id, new Element(id)]));
        elements.ft8Scroll = {scrollTop: 0, clientHeight: 250, scrollHeight: 250};
        const context = {document: {
            getElementById: id => elements[id],
            createElement: tag => new Element(tag),
            createDocumentFragment: () => new Element('fragment'),
        }};
        vm.createContext(context);
        if (loaded) vm.runInContext(dataScript + '\n' + lookupScript, context);
        vm.runInContext('const FT8_MAX_ROWS = 500; let ft8DialHz = null;\n' +
            extract('setFT8Status') + '\n' + extract('processFT8Frame'), context);
        const frame = text => {
            const values = new Float32Array(1024);
            values.set([...text].map(char => char.charCodeAt(0)));
            context.processFT8Frame(values);
        };
        const utc = Date.UTC(2026, 9, 27, 18, 16, 0) / 1000;
        frame('S\t10136000\n');
        const record = (message, offset) => `D\t${utc}\t10136000\t181600 -10 0.14 ${offset} ~ ${message}\n`;
        frame(record('ON4KBU YO4DWD RR73', '1788.5') + record('CQ F5NNN IN94', '1096.4'));
        frame(record('R2BRU <...> -02', '1265.1'));
        const rows = elements.ft8Rows.rows.map(row => row.children.map(cell => cell.textContent));
        assert.deepEqual(rows[0], ['18:16:00', '10.1377885', '-10', '0.14', loaded ? 'Rumänien' : '—', 'ON4KBU YO4DWD RR73']);
        assert.equal(rows[1][4], loaded ? 'Frankreich' : '—');
        assert.equal(rows[2][4], '—');
        assert.equal(rows[2][5], 'R2BRU <...> -02');
        assert.equal(elements.ft8Status.textContent, '10.136000 MHz · USB');
        assert.equal(elements.ft8Count.textContent, '3 Meldungen');
        assert.ok(rows.every(row => row.length === 6));
    }
    console.log(`${page}: country column, correct transmitter, unresolved hash, frequency precision and missing-asset fallback passed.`);
}

const noDataContext = {};
vm.runInNewContext(lookupScript, noDataContext);
assert.equal(noDataContext.FT8Countries.countryForCallsign('DL1ABC'), '');
console.log('Missing-country-data fallback passed.');
