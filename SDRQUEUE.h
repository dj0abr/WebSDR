#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include "liquid.h"
#include "SDRStreamInfo.h"

/**
 * SDRQUEUE
 * --------
 * Broadcast-Ringbuffer-Design (Disruptor-Style):
 *  - genau 1 Producer (SDR RX Callback)
 *  - N Consumer (mehrere DSP-Pipelines: SSB/FM, FFT-1, FFT-2, Digital, ...)
 *
 * Ziel:
 *  - Producer kopiert Samples genau 1x in den Ring
 *  - jeder Consumer liest dieselben Blöcke über eigenen Lesezeiger
 *  - alte Slots werden beim Umrunden überschrieben, sobald alle Consumer vorbei sind
 *  - wenn Ring "voll" (ein Consumer ist zu langsam), Producer droppt Block + Debugmeldung
 *
 */
class SDRQUEUE final {
public:
    // ------------------------------------------------------------
    // Konfiguration
    // ------------------------------------------------------------
    struct Config {
        /// Sample-Typ, der in den Ring geschrieben wird.
        using SampleType = liquid_float_complex;

        /**
         * Anzahl Slots (Blöcke) im Ring.
         * MUSS Power-of-two sein (256, 512, 1024, ...).
         */
        static constexpr std::size_t SLOTS = 256;

        /**
         * Maximale Samples pro Slot (Blockgröße).
         * RX liefert 1344, wir wählen 2048 als fixen Slot-Speicher.
         */
        static constexpr std::size_t MAX_SAMPLES_PER_SLOT = 2048;

        /**
         * Anzahl Consumer.
         * Wenn du später mehr brauchst: hier erhöhen.
         * (In späteren Steps sorgen wir dafür, dass die API das sauber nutzt.)
         */
        static constexpr std::size_t CONSUMER_COUNT = 3;
    };

    // ------------------------------------------------------------
    // Öffentliche Typen
    // ------------------------------------------------------------
    using Sample = typename Config::SampleType;

    /**
     * View: das ist das, was ein Consumer aus einem Slot bekommt.
     * Step 1: nur Pointer+Count+Seq (noch kein std::span).
     *
     * - data zeigt direkt in den Ring-Slot (keine Kopie)
     * - gültig bis der Slot überschrieben wird (also bis Consumer zu langsam wäre)
     */
    struct View {
        const Sample* data = nullptr;  ///< Pointer auf Samples im Slot
        std::uint32_t count = 0;       ///< Anzahl gültiger Samples
        SDRStreamInfo info;
        std::uint64_t seq = 0;         ///< Sequence-Nummer (fortlaufend), Debug/Tracing
    };

private:
    // ------------------------------------------------------------
    // Interne Datenstrukturen
    // ------------------------------------------------------------

    /**
     * Cacheline Padding für Atomics, um False Sharing zu minimieren.
     *
     * read_seq[i] wird von Consumer i geschrieben, vom Producer gelesen.
     * Wenn mehrere read_seq dicht beieinander liegen, invalidiert jeder Consumer
     * die Cacheline der anderen => unnötige CPU-Last.
     */
    struct alignas(64) PaddedU64 {
        std::atomic<std::uint64_t> v {0};
    };

    /**
     * Ein Slot im Ring:
     * - Metadaten: count, seq
     * - Sample-Storage: fixes Array (MAX_SAMPLES_PER_SLOT)
     *
     * Producer schreibt Samples+Metadaten, dann "publish" über write_seq.
     * Consumer lesen Slot nachdem sie write_seq gesehen haben.
     */
    struct Slot {
        std::uint32_t count = 0;  ///< gültige Samples im Slot
        SDRStreamInfo info;
        std::uint64_t seq = 0;    ///< Sequence-Nummer (entspricht Slot-Index in seq space)
        std::array<Sample, Config::MAX_SAMPLES_PER_SLOT> samples {};
    };

    // ------------------------------------------------------------
    // Statische Member (globale Queue-Instanz)
    // ------------------------------------------------------------

    /// Ring-Storage (SLOTS Slots).
    static std::array<Slot, Config::SLOTS> ring_;

    /**
     * Write-Sequence (Producer):
     * - Producer schreibt Slot [write_seq & (SLOTS-1)]
     * - danach write_seq++
     */
    static std::atomic<std::uint64_t> write_seq_;

    /**
     * Read-Sequences pro Consumer:
     * - Consumer i liest Slot [read_seq[i] & (SLOTS-1)]
     * - danach read_seq[i]++
     */
    static std::array<PaddedU64, Config::CONSUMER_COUNT> read_seq_;

    // ------------------------------------------------------------
    // Debug/Stats
    // ------------------------------------------------------------

    /// Anzahl gedroppter Blöcke (Ring voll).
    static std::atomic<std::uint64_t> drops_;

    /// Maximales beobachtetes Lag (write_seq - min(read_seq)).
    static std::atomic<std::uint64_t> max_lag_;

public:
    // ------------------------------------------------------------
    // Konstruktion verhindern (statische Klasse)
    // ------------------------------------------------------------
    SDRQUEUE() = delete;
    SDRQUEUE(const SDRQUEUE&) = delete;
    SDRQUEUE& operator=(const SDRQUEUE&) = delete;
    ~SDRQUEUE() = delete;

    /**
     * Reset der Queue-Statistiken und Sequenzen.
     *
     * WICHTIG: Nur aufrufen, wenn Producer/Consumer-Threads gestoppt sind.
     * (Also: kein gleichzeitiger Zugriff auf Queue-Daten.)
     */
    static void reset();

    /**
     * Producer-API: schreibt einen Block Samples in den Ring.
     *
     * - Kopiert höchstens MAX_SAMPLES_PER_SLOT Samples (bei Bedarf truncation).
     * - Wenn der Ring voll ist: drop + Debugmeldung, Rückgabewert false.
     *
     * @param in Pointer auf Eingangssamples (vom RX Callback)
     * @param n  Anzahl Samples (bei dir typ. 1344)
     * @return true wenn geschrieben, false wenn gedroppt
     */
    static bool pushData(const Sample* in, std::uint32_t n, const SDRStreamInfo& info = {});

    // ------------------------------------------------------------
    // Consumer API (getrennt: erst holen, dann bestätigen)
    // ------------------------------------------------------------

    /**
     * Versucht, den nächsten verfügbaren Block für consumerIdx zu holen.
     *
     * - Gibt true zurück und füllt `out`, wenn ein Block verfügbar ist.
     * - Gibt false zurück, wenn noch kein neuer Block da ist.
     *
     * WICHTIG:
     *  - tryPop() bewegt den Lesezeiger NICHT!
     *  - Nach erfolgreichem Verarbeiten MUSS consume() aufgerufen werden,
     *    sonst läuft der Ring voll und der Producer droppt.
     *
     * Paranoia-Check:
     *  - Wenn slot.seq != erwartete read_seq, wird eine Fehlermeldung gedruckt.
     */
    static bool tryPop(std::size_t consumerIdx, View& out);

    /**
     * Bestätigt genau EINEN Block als "verbraucht" für consumerIdx.
     * Muss nach einem erfolgreichen tryPop() aufgerufen werden,
     * sobald der Consumer fertig verarbeitet hat.
     */
    static void consume(std::size_t consumerIdx);
};
