(() => {
    const row = document.getElementById("receiverPresets");
    const status = document.getElementById("receiverPresetStatus");
    const bandBox = document.getElementById("bandBox");
    const modeBox = document.getElementById("modeBox");
    const filterBox = document.getElementById("nffilter");
    const storagePrefix = "kwWebRXpp.receiverPreset.";
    const defaultFilter = filterBox.value;
    let receiver = null;
    let pending = null;

    function getOption(select, value) {
        return Array.from(select.options).find(option => option.value === String(value));
    }

    function readPreset(number) {
        try {
            const preset = JSON.parse(localStorage.getItem(storagePrefix + number));
            if (preset && Number.isSafeInteger(preset.frequency) && preset.frequency > 0 &&
                Number.isInteger(preset.band) &&
                Number.isInteger(preset.mode) && getOption(modeBox, preset.mode) &&
                Number.isInteger(preset.filter) && getOption(filterBox, preset.filter)) return preset;
        } catch {
            // A missing, malformed or unavailable storage entry is an empty slot.
        }
        return null;
    }

    function describe(preset) {
        const frequency = (preset.frequency / 1e6).toFixed(6).replace(".", ",");
        const band = getOption(bandBox, preset.band)?.textContent ?? preset.band;
        return `${frequency} MHz · ${band} · ${getOption(modeBox, preset.mode).textContent} · ${getOption(filterBox, preset.filter).textContent}`;
    }

    function createButton(text, number, click) {
        const button = document.createElement("button");
        button.type = "button";
        button.textContent = text;
        button.setAttribute("aria-label", `Speicher ${number} ${text.toLowerCase()}`);
        button.addEventListener("click", click);
        return button;
    }

    const slots = Array.from({ length: 5 }, (_, index) => {
        const number = index + 1;
        const group = document.createElement("div");
        group.className = "receiver-preset";
        const label = document.createElement("span");
        label.textContent = number;
        const save = createButton("Speichern", number, () => savePreset(number));
        const load = createButton("Laden", number, () => loadPreset(number));
        group.append(label, save, load);
        row.insertBefore(group, status);
        return { number, save, load };
    });

    function render() {
        for (const { number, save, load } of slots) {
            const preset = readPreset(number);
            save.disabled = !receiver || !getOption(bandBox, receiver.band) || Boolean(pending);
            load.disabled = !receiver || !preset || Boolean(pending);
            load.title = preset ? describe(preset) : "Speicher leer";
        }
    }

    function savePreset(number) {
        if (!receiver || pending) return;
        const preset = {
            frequency: receiver.frequency,
            band: receiver.band,
            mode: Number(modeBox.value),
            filter: Number(filterBox.value),
        };
        try {
            localStorage.setItem(storagePrefix + number, JSON.stringify(preset));
            status.textContent = `Speicher ${number} gespeichert.`;
        } catch {
            status.textContent = "Der Browser erlaubt das Speichern derzeit nicht.";
        }
        render();
    }

    function finish(message) {
        clearTimeout(pending?.timer);
        pending = null;
        status.textContent = message;
        render();
    }

    function matches(preset) {
        return receiver.frequency === preset.frequency && receiver.mode === preset.mode;
    }

    function tuneToPreset() {
        const { number, preset } = pending;
        // The tuning command uses an oscillator offset relative to center minus 240 kHz.
        const offset = preset.frequency - receiver.center + 240000;
        if (offset < 0 || offset > 480000) {
            finish("Die gespeicherte Frequenz liegt außerhalb des Empfangsbereichs.");
            return;
        }
        pending.tuning = true;
        modeBox.value = preset.mode;
        updateMode();
        filterBox.value = preset.filter;
        updateFilter();
        freqOffset = clampFrequency(preset.frequency - receiver.start);
        sendFreqOffsetToServer(offset);
        drawTuningLine();
        if (matches(preset)) finish(`Speicher ${number} geladen.`);
    }

    function loadPreset(number) {
        const preset = readPreset(number);
        if (!receiver || !preset || pending) return;
        if (!getOption(bandBox, preset.band)) {
            status.textContent = "Dieses Band ist in dieser Ansicht nicht verfügbar.";
            return;
        }
        const changeBand = receiver.band !== preset.band;
        if (changeBand && bandBox.disabled) {
            status.textContent = "Bandwechsel gesperrt, solange mehrere Hörer verbunden sind.";
            return;
        }
        status.textContent = `Lade Speicher ${number} …`;
        pending = {
            number, preset, tuning: false,
            timer: setTimeout(() => finish("Der Empfänger hat die Einstellung nicht bestätigt."), 5000),
        };
        render();
        if (changeBand) sendBandToServer(preset.band);
        else tuneToPreset();
    }

    function onConfig(data, band) {
        if (!(data[0] > 0 && data[4] > data[3]) || !Number.isFinite(data[1])) return;
        receiver = {
            band,
            center: data[0],
            frequency: Math.round(data[0] + data[1]),
            mode: data[2],
            start: data[3],
        };
        filterBox.disabled = false;
        if (pending) {
            const { number, preset, tuning } = pending;
            if (band === preset.band) {
                if (!tuning) tuneToPreset();
                else if (matches(preset)) finish(`Speicher ${number} geladen.`);
            } else if (bandBox.disabled && !tuning) {
                finish("Bandwechsel gesperrt, solange mehrere Hörer verbunden sind.");
            }
        }
        render();
    }

    function reset() {
        receiver = null;
        filterBox.value = defaultFilter;
        filterBox.disabled = true;
        finish("");
    }

    window.addEventListener("storage", event => {
        if (event.key === null || event.key.startsWith(storagePrefix)) render();
    });
    globalThis.ReceiverPresets = {
        onConfig,
        reset,
        cancel() {
            if (pending) finish("Speicherabruf abgebrochen.");
        },
    };
    reset();
})();
