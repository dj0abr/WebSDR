// Shared desktop/mobile controls. Command 5 sets 0..100 or queries with -1.
// Type-8 replies carry [8, version=1, configured percent, effective percent].
// Every new listener starts at 0; switching off preserves the slider strength.
globalThis.NoiseReductionControl = (() => {
    const enabled = document.getElementById('nrEnabled');
    const strength = document.getElementById('nrStrength');
    const value = document.getElementById('nrStrengthValue');
    const status = document.getElementById('nrStatus');
    let connection = null, available = false, wanted = null, confirmationTimer;

    function send(percent) {
        if (!connection || connection.readyState !== WebSocket.OPEN) return;
        connection.send(new Float32Array([5, percent]).buffer);
    }
    function update() {
        value.textContent = `${strength.value} %`;
        if (!available) return;
        wanted = enabled.checked ? Number(strength.value) : 0;
        status.textContent = 'Warte auf Bestätigung …';
        send(wanted);
    }
    enabled.addEventListener('change', update);
    strength.addEventListener('input', () => {
        value.textContent = `${strength.value} %`;
        if (enabled.checked) update();
    });
    return {
        connect(socket) {
            connection = socket;
            available = false;
            wanted = null;
            enabled.checked = false;
            enabled.disabled = strength.disabled = true;
            status.textContent = 'Warte auf NR-Status …';
            clearTimeout(confirmationTimer);
            confirmationTimer = setTimeout(() => {
                if (!available) status.textContent = 'NR vom Empfänger nicht bestätigt';
            }, 3000);
            send(-1);
        },
        disconnect() {
            connection = null;
            available = false;
            wanted = null;
            clearTimeout(confirmationTimer);
            enabled.checked = false;
            enabled.disabled = strength.disabled = true;
            status.textContent = 'Nicht verbunden';
        },
        handleState(frame) {
            if (!connection || frame.length < 4 || frame[0] !== 8 || frame[1] !== 1) return;
            const percent = frame[2], effective = frame[3];
            if (!Number.isInteger(percent) || percent < 0 || percent > 100 ||
                !Number.isInteger(effective) || (effective !== 0 && effective !== percent)) return;
            available = true;
            clearTimeout(confirmationTimer);
            enabled.disabled = strength.disabled = false;
            // A reply to an earlier slider event must not undo a later choice.
            if (wanted !== null && percent !== wanted) return;
            wanted = null;
            enabled.checked = percent > 0;
            if (percent) strength.value = String(percent);
            value.textContent = `${strength.value} %`;
            status.textContent = !percent ? 'Aus' :
                effective ? `Aktiv: ${effective} %` : `FM: pausiert (${percent} % gewählt)`;
        }
    };
})();
