// Resolve the transmitting station, then its CTY/DXCC entity. No online lookups.
(function (root) {
    'use strict';
    const prefixes = new Map(), exact = new Map();
    for (const [country, aliases] of root.FT8CountryData?.entities || []) {
        for (const alias of aliases.split(',')) {
            const fullCall = alias.startsWith('=');
            const key = fullCall ? alias.slice(1) : alias;
            const table = fullCall ? exact : prefixes;
            if (!table.has(key)) table.set(key, country);
        }
    }

    function normalizeCallsign(value) {
        let call = String(value || '').trim().toUpperCase();
        if (call.startsWith('<') && call.endsWith('>')) call = call.slice(1, -1);
        if (!/^[A-Z0-9]+(?:\/[A-Z0-9]+)*$/.test(call)) return '';
        // Grid locators and reports are not callsigns. Keep known special calls.
        return exact.has(call) || call.split('/').some(part => /\d[A-Z]+$/.test(part)) ? call : '';
    }

    function matchPrefix(call) {
        // KG4 with a one- or three-letter suffix belongs to the mainland USA.
        if (call.startsWith('KG4') && call.length !== 5 && call.length !== 3) {
            return prefixes.get('K') || '';
        }
        for (let length = call.length; length > 0; --length) {
            const country = prefixes.get(call.slice(0, length));
            if (country) return country;
        }
        return '';
    }

    function countryForCallsign(value) {
        let call = normalizeCallsign(value);
        // Maritime/aeronautical mobile operation has no terrestrial DXCC entity.
        if (!call || /\/(?:MM|AM)(?:\/|$)/.test(call)) return '';
        if (exact.has(call)) return exact.get(call);
        call = call.replace(/(?:\/(?:P|M|R|QRP|QRPP))+$/, '');
        if (exact.has(call)) return exact.get(call);
        const parts = call.split('/');
        if (parts.length === 1) return matchPrefix(call);
        if (parts.length !== 2) return '';
        const [left, right] = parts;
        const leftIsCall = /\d[A-Z]+$/.test(left), rightIsCall = /\d[A-Z]+$/.test(right);
        if (leftIsCall && /^\d$/.test(right)) {
            return matchPrefix(left.replace(/\d+(?=[A-Z]+$)/, right));
        }
        // Prefer the operating prefix in F/DL1ABC and DL1ABC/EA8.
        if (!leftIsCall && rightIsCall) return matchPrefix(left);
        if (leftIsCall && !rightIsCall) return matchPrefix(right);
        return '';
    }

    function senderCallsign(message) {
        const words = String(message || '').trim().toUpperCase().split(/\s+/);
        // DXpedition: recipient RR73; next-recipient <transmitter> report.
        if (words.length === 5 && words[1] === 'RR73;') {
            return normalizeCallsign(words[3]);
        }
        if (words[0] === 'TU;') words.shift();
        if (words[0] === 'CQ') {
            let index = 1;
            // Directed CQ: CQ DX/JA/TEST/123 CALL [locator].
            if (/^(?:[A-Z]{1,4}|\d{3})$/.test(words[1] || '')) ++index;
            return normalizeCallsign(words[index]);
        }
        if (words[0] === 'DE' || words[0] === 'QRZ') return normalizeCallsign(words[1]);
        // The second callsign sends a standard QSO message. Never fall back to
        // the recipient when the sender is an unresolved <...> hash.
        if (!normalizeCallsign(words[0]) && words[0] !== '<...>') return '';
        return normalizeCallsign(words[1]);
    }

    root.FT8Countries = Object.freeze({countryForCallsign, senderCallsign});
})(globalThis);
