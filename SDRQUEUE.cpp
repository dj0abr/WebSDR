#include "SDRQUEUE.h"

#include <algorithm>
#include <cstring>
#include <cstdio>

// ------------------------------------------------------------
// Statische Member (Speicher)
// ------------------------------------------------------------
std::array<SDRQUEUE::Slot, SDRQUEUE::Config::SLOTS> SDRQUEUE::ring_ {};
std::atomic<std::uint64_t> SDRQUEUE::write_seq_ {0};
std::array<SDRQUEUE::PaddedU64, SDRQUEUE::Config::CONSUMER_COUNT> SDRQUEUE::read_seq_ {};
std::atomic<std::uint64_t> SDRQUEUE::drops_ {0};
std::atomic<std::uint64_t> SDRQUEUE::max_lag_ {0};

void SDRQUEUE::reset()
{
    // Annahme: Keine Threads greifen gerade zu (Producer/Consumer gestoppt).
    write_seq_.store(0, std::memory_order_relaxed);
    for (auto& r : read_seq_) {
        r.v.store(0, std::memory_order_relaxed);
    }
    drops_.store(0, std::memory_order_relaxed);
    max_lag_.store(0, std::memory_order_relaxed);

    // Slot-Inhalte müssen nicht gelöscht werden:
    // Sie werden sowieso überschrieben, sobald geschrieben wird.
}

bool SDRQUEUE::pushData(const Sample* in, std::uint32_t n, const SDRStreamInfo& info)
{
    if (!in || n == 0) {
        return true; // nichts zu tun
    }

    // Truncate, falls jemand mehr als MAX liefert
    if (n > Config::MAX_SAMPLES_PER_SLOT) {
        n = static_cast<std::uint32_t>(Config::MAX_SAMPLES_PER_SLOT);
        // Optional: hier könnte man einen Counter für "truncated blocks" führen
    }

    // Aktuelle Write-Sequence (nur Producer schreibt -> relaxed ok)
    const std::uint64_t w = write_seq_.load(std::memory_order_relaxed);

    // min(read_seq[i]) bestimmen (slowest consumer)
    std::uint64_t min_r = read_seq_[0].v.load(std::memory_order_acquire);
    for (std::size_t i = 1; i < Config::CONSUMER_COUNT; ++i) {
        const std::uint64_t r = read_seq_[i].v.load(std::memory_order_acquire);
        min_r = std::min(min_r, r);
    }

    const std::uint64_t lag = w - min_r; // wie viele Slots der langsamste hinterher ist
    // Statistik: max_lag aktualisieren (best effort)
    {
        std::uint64_t cur = max_lag_.load(std::memory_order_relaxed);
        while (lag > cur && !max_lag_.compare_exchange_weak(cur, lag, std::memory_order_relaxed)) {
            // cur wird aktualisiert, loop
        }
    }

    // Ring voll? (würden wir unread data überschreiben)
    if (lag >= Config::SLOTS) {
        const auto d = drops_.fetch_add(1, std::memory_order_relaxed) + 1;

        // Debug-Print: rate-limited (sonst ruiniert Logging die Echtzeit)
        // -> hier: jede 100. Drop-Meldung
        if ((d % 100) == 1) {
            std::fprintf(stderr,
                         "[SDRQUEUE] Ring full -> drop block (drops=%llu, lag=%llu, slots=%zu)\n",
                         static_cast<unsigned long long>(d),
                         static_cast<unsigned long long>(lag),
                         Config::SLOTS);
        }
        return false;
    }

    // In Slot schreiben
    Slot& slot = ring_[w & (Config::SLOTS - 1)];
    slot.count = n;
    slot.info = info;
    slot.seq   = w;

    std::memcpy(slot.samples.data(), in, static_cast<std::size_t>(n) * sizeof(Sample));

    // Publish: Slot-Inhalt sichtbar machen, dann write_seq erhöhen
    write_seq_.store(w + 1, std::memory_order_release);
    return true;
}

bool SDRQUEUE::tryPop(std::size_t consumerIdx, View& out)
{
    // Für den Test (und später) gehen wir davon aus:
    // consumerIdx ist korrekt. Optional könnte man hier assert() machen.

    const std::uint64_t r = read_seq_[consumerIdx].v.load(std::memory_order_relaxed);
    const std::uint64_t w = write_seq_.load(std::memory_order_acquire);

    if (r >= w) {
        return false; // nichts neues
    }

    const Slot& slot = ring_[r & (Config::SLOTS - 1)];

    // Paranoia-Check:
    // Wenn der Producer jemals Slots überschreibt, bevor der Consumer sie gelesen hat,
    // dann stimmt slot.seq nicht mehr mit dem erwarteten r überein.
    // In unserem Design sollte das NICHT passieren (Producer droppt vorher).
    if (slot.seq != r) {
        std::fprintf(stderr,
                     "[SDRQUEUE] PARANOIA: slot.seq mismatch (expected=%llu, got=%llu). "
                     "Consumer too slow or reset/race?\n",
                     static_cast<unsigned long long>(r),
                     static_cast<unsigned long long>(slot.seq));
        // Wir liefern in dem Fall lieber keinen View, um keinen Müll zu verarbeiten.
        return false;
    }

    out.data  = slot.samples.data();
    out.count = slot.count;
    out.info = slot.info;
    out.seq   = slot.seq;
    return true;
}

void SDRQUEUE::consume(std::size_t consumerIdx)
{
    const std::uint64_t r = read_seq_[consumerIdx].v.load(std::memory_order_relaxed);
    read_seq_[consumerIdx].v.store(r + 1, std::memory_order_release);
}
