// Browser command 6 sets the level. Type 9 acknowledges the level and reports the gate.
globalThis.FMSquelchControl = (() => {
    const row = document.getElementById("fmSquelch");
    const slider = document.getElementById("fmSquelchLevel");
    const value = document.getElementById("fmSquelchValue");
    const status = document.getElementById("fmSquelchStatus");
    const storageKey = "kwWebRXpp.fmSquelch";
    let socket = null;
    let sendTimer, confirmationTimer;

    try {
        const stored = Number(localStorage.getItem(storageKey));
        if (Number.isInteger(stored) && stored >= 0 && stored <= 100) slider.value = stored;
    } catch {
        // The control remains usable when browser storage is unavailable.
    }
    value.textContent = slider.value;

    function setStatus(text) {
        if (status.textContent !== text) status.textContent = text;
    }

    function clearTimers() {
        clearTimeout(sendTimer);
        clearTimeout(confirmationTimer);
    }

    function sendLevel() {
        clearTimers();
        if (row.hidden || socket?.readyState !== WebSocket.OPEN) return;
        setStatus("Warte auf Empfänger …");
        socket.send(new Float32Array([6, Number(slider.value)]).buffer);
        confirmationTimer = setTimeout(() => setStatus("Keine Rückmeldung vom Empfänger"), 3000);
    }

    slider.addEventListener("input", () => {
        value.textContent = slider.value;
        try {
            localStorage.setItem(storageKey, slider.value);
        } catch {
            // Keep the current value for this page.
        }
        clearTimers();
        setStatus("Warte auf Empfänger …");
        sendTimer = setTimeout(sendLevel, 100);
    });

    return {
        connect(connection) {
            socket = connection;
        },
        disconnect() {
            socket = null;
            row.hidden = true;
            clearTimers();
        },
        onMode(mode) {
            const show = mode === 2 && socket?.readyState === WebSocket.OPEN;
            if (show === !row.hidden) return;
            row.hidden = !show;
            if (show) sendLevel();
            else clearTimers();
        },
        handleState(frame) {
            if (row.hidden || !socket || frame.length !== 1025 ||
                frame[0] !== 9 || frame[1] !== 1 || frame[2] !== Number(slider.value)) return;
            const gate = frame[3];
            if (gate === 0 && frame[2] === 0) setStatus("Aus");
            else if (gate === 1) setStatus("Geschlossen");
            else if (gate === 2) setStatus("Offen");
            else return;
            clearTimeout(confirmationTimer);
        },
    };
})();
