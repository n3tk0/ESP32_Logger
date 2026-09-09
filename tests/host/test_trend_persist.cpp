// The 24-hour chart, across a reboot.
//
// WHY THIS FILE EXISTS
// --------------------
// TrendRing is the only twenty-four-hour record the firmware keeps — the web
// ring holds minutes and the FS history reader is stubbed out — so until it
// was persisted, a power cut cost the whole chart and the Kindle woke to a
// blank strip that filled back in one hour at a time over the following day.
//
// The snapshot is written to flash by a device that is losing power, which is
// the whole reason it exists. So the properties worth pinning down are not
// "does it round-trip" alone but "what does it refuse": a torn file whose
// length happens to be right would otherwise restore buckets full of whatever
// was on that page, and a chart of invented temperatures is worse than no
// chart, because nothing downstream can tell.
//
// The other half is that a RESTORED RING NEEDS NO SPECIAL CASE FOR AGE.
// series() already decides what is live by comparing each series' lastHour
// with the hour being asked about, so a four-hour-old snapshot has to come
// back as twenty hours of readings and four gaps with no code saying so.
#define FEATURE_KINDLE_DASHBOARD 1
#include "src/pipeline/TrendRing.cpp"
#include "check.h"

#include <vector>

HostSerial Serial;

static const uint32_t HOUR = 3600u;
// A Tuesday in 2026, on an exact hour boundary, well past MIN_REAL_TS.
static const uint32_t T0   = 1788000000u / HOUR * HOUR;

static SensorReading rd(uint32_t ts, const char* id, const char* metric, float v) {
    SensorReading r;
    r.timestamp = ts;
    snprintf(r.sensorId, sizeof(r.sensorId), "%s", id);
    snprintf(r.metric,   sizeof(r.metric),   "%s", metric);
    r.value   = v;
    r.quality = QUALITY_GOOD;
    return r;
}

/// A ring with `hours` hours of readings ending in the hour containing `now`.
static void fill(TrendRing& t, uint32_t now, int hours) {
    t.track("out", "temperature");
    for (int h = hours - 1; h >= 0; h--) {
        const uint32_t ts = now - (uint32_t)h * HOUR;
        t.add(rd(ts,          "out", "temperature", 10.0f + (float)h));
        t.add(rd(ts + 1800u,  "out", "temperature", 12.0f + (float)h));
    }
}

// ---------------------------------------------------------------------------
static void test_a_snapshot_restores_the_same_chart() {
    TrendRing a;
    fill(a, T0, 24);

    std::vector<uint8_t> buf(TrendRing::snapshotBytes());
    CHECK_EQ((long)a.snapshot(buf.data(), buf.size()), (long)buf.size());

    // TRACK FIRST. restore() fills the series this build tracks; it does not
    // decide which ones exist. See the note on restore() for why.
    TrendRing b;
    b.track("out", "temperature");
    CHECK(b.restore(buf.data(), buf.size()));

    TrendRing::Hour ha[TrendRing::HOURS], hb[TrendRing::HOURS];
    CHECK(a.series("out", "temperature", T0, ha));
    CHECK(b.series("out", "temperature", T0, hb));
    for (int i = 0; i < TrendRing::HOURS; i++) {
        CHECK_EQ((int)ha[i].count, (int)hb[i].count);
        CHECK(ha[i].min == hb[i].min);
        CHECK(ha[i].max == hb[i].max);
        CHECK(ha[i].sum == hb[i].sum);
    }
    // Not merely equal to each other — equal to what was put in. Two rings
    // that agree on nothing would pass the loop above.
    CHECK_EQ((int)hb[TrendRing::HOURS - 1].count, 2);
    CHECK(hb[TrendRing::HOURS - 1].min == 10.0f);
    CHECK(hb[TrendRing::HOURS - 1].max == 12.0f);

    // The series has to be FOUND in the restored ring, not merely present:
    // track() state travels with it, or the first reading after a reboot goes
    // to an untracked series and is dropped.
    CHECK(!b.series("nosuch", "temperature", T0, hb));
}

// ---------------------------------------------------------------------------
static void test_time_passing_needs_no_special_case() {
    TrendRing a;
    fill(a, T0, 24);
    std::vector<uint8_t> buf(TrendRing::snapshotBytes());
    a.snapshot(buf.data(), buf.size());

    // The collector was off for four hours. Twenty hours of readings survive;
    // the four it missed are gaps, because series() judges by lastHour.
    TrendRing b;
    b.track("out", "temperature");
    CHECK(b.restore(buf.data(), buf.size()));
    TrendRing::Hour h[TrendRing::HOURS];
    CHECK(b.series("out", "temperature", T0 + 4 * HOUR, h));

    int live = 0, gaps = 0;
    for (int i = 0; i < TrendRing::HOURS; i++) (h[i].count ? live : gaps)++;
    CHECK_EQ(live, 20);
    CHECK_EQ(gaps, 4);
    // The gaps are the RECENT hours, not scattered: the four it was off for.
    for (int i = TrendRing::HOURS - 4; i < TrendRing::HOURS; i++)
        CHECK_EQ((int)h[i].count, 0);

    // Off for longer than the window: everything is stale, nothing invented.
    TrendRing c;
    c.track("out", "temperature");
    CHECK(c.restore(buf.data(), buf.size()));
    CHECK(c.series("out", "temperature", T0 + 40 * HOUR, h));
    for (int i = 0; i < TrendRing::HOURS; i++) CHECK_EQ((int)h[i].count, 0);
}

// ---------------------------------------------------------------------------
static void test_the_ring_keeps_working_after_a_restore() {
    TrendRing a;
    fill(a, T0, 24);
    std::vector<uint8_t> buf(TrendRing::snapshotBytes());
    a.snapshot(buf.data(), buf.size());

    TrendRing b;
    b.track("out", "temperature");
    CHECK(b.restore(buf.data(), buf.size()));

    // Two hours later the collector is back and reporting again. The new
    // readings land in their own hour, and the two hours it missed are
    // cleared rather than showing yesterday's values in the same slots.
    b.add(rd(T0 + 3 * HOUR, "out", "temperature", 99.0f));

    TrendRing::Hour h[TrendRing::HOURS];
    CHECK(b.series("out", "temperature", T0 + 3 * HOUR, h));
    CHECK_EQ((int)h[TrendRing::HOURS - 1].count, 1);
    CHECK(h[TrendRing::HOURS - 1].min == 99.0f);
    CHECK_EQ((int)h[TrendRing::HOURS - 2].count, 0);   // the missed hours
    CHECK_EQ((int)h[TrendRing::HOURS - 3].count, 0);
    CHECK(h[TrendRing::HOURS - 4].count > 0);          // and the last live one
}

// ---------------------------------------------------------------------------
static void test_a_damaged_snapshot_is_refused_whole() {
    TrendRing a;
    fill(a, T0, 24);
    std::vector<uint8_t> good(TrendRing::snapshotBytes());
    a.snapshot(good.data(), good.size());

    // One flipped bit anywhere in the payload. The length is still right and
    // the magic still matches — this is exactly what a torn write leaves.
    for (size_t at : {(size_t)20, good.size() / 2, good.size() - 1}) {
        std::vector<uint8_t> bad = good;
        bad[at] ^= 0x01;
        TrendRing b;
        b.track("out", "temperature");
        b.add(rd(T0, "out", "temperature", 5.0f));
        CHECK(!b.restore(bad.data(), bad.size()));

        // AND THE RING IS UNTOUCHED. A restore that half-succeeded would put
        // invented temperatures on the chart with nothing able to notice.
        TrendRing::Hour h[TrendRing::HOURS];
        CHECK(b.series("out", "temperature", T0, h));
        CHECK_EQ((int)h[TrendRing::HOURS - 1].count, 1);
        CHECK(h[TrendRing::HOURS - 1].min == 5.0f);
    }
}

// ---------------------------------------------------------------------------
static void test_it_refuses_what_it_does_not_recognise() {
    TrendRing a;
    fill(a, T0, 8);
    std::vector<uint8_t> good(TrendRing::snapshotBytes());
    a.snapshot(good.data(), good.size());

    TrendRing b;
    CHECK(!b.restore(nullptr, good.size()));
    CHECK(!b.restore(good.data(), good.size() - 1));   // truncated
    CHECK(!b.restore(good.data(), good.size() + 1));   // and padded
    CHECK(!b.restore(good.data(), 0));

    { std::vector<uint8_t> v = good; v[0] ^= 0xFF;
      CHECK(!b.restore(v.data(), v.size())); }         // magic
    { std::vector<uint8_t> v = good; v[4] = 99;
      CHECK(!b.restore(v.data(), v.size())); }         // version
    { std::vector<uint8_t> v = good; v[6] = 99;
      CHECK(!b.restore(v.data(), v.size())); }         // slot count
    { std::vector<uint8_t> v = good; v[8] = 99;
      CHECK(!b.restore(v.data(), v.size())); }         // hours per series

    // A buffer smaller than the record gets nothing written into it at all,
    // rather than a header with no body behind it.
    std::vector<uint8_t> small(TrendRing::snapshotBytes() - 1, 0xAB);
    CHECK_EQ((long)a.snapshot(small.data(), small.size()), 0L);
    for (uint8_t byte : small) CHECK_EQ((int)byte, 0xAB);
    CHECK_EQ((long)a.snapshot(nullptr, TrendRing::snapshotBytes()), 0L);
}

// ---------------------------------------------------------------------------
static void test_dirty_marks_finished_hours_and_not_every_reading() {
    TrendRing t;
    t.track("out", "temperature");
    CHECK(!t.dirty());

    // The first reading opens the first bucket — a new hour, so worth saving.
    t.add(rd(T0, "out", "temperature", 1.0f));
    CHECK(t.dirty());
    t.clearDirty();

    // Nine more readings inside the SAME hour change the bucket but do not
    // finish it. A flash write each would be 1,440 a day for an hourly chart.
    for (int i = 1; i <= 9; i++)
        t.add(rd(T0 + (uint32_t)i * 300u, "out", "temperature", 1.0f + (float)i));
    CHECK(!t.dirty());

    // The hour turning over is the moment the bucket stops changing.
    t.add(rd(T0 + HOUR, "out", "temperature", 2.0f));
    CHECK(t.dirty());
    t.clearDirty();

    // A reading for a series nobody tracks changes nothing, so it cannot make
    // the ring look worth writing.
    t.add(rd(T0 + 2 * HOUR, "nosuch", "temperature", 3.0f));
    CHECK(!t.dirty());

    // Neither does one from before the clock was set, which add() drops.
    t.add(rd(1000u, "out", "temperature", 4.0f));
    CHECK(!t.dirty());
}

// ---------------------------------------------------------------------------
static void test_every_tracked_series_travels() {
    TrendRing a;
    a.track("out", "temperature");
    a.track("out", "pressure");
    a.track("in",  "temperature");
    a.add(rd(T0, "out", "temperature", -3.5f));
    a.add(rd(T0, "out", "pressure",  1013.2f));
    a.add(rd(T0, "in",  "temperature", 21.0f));

    std::vector<uint8_t> buf(TrendRing::snapshotBytes());
    a.snapshot(buf.data(), buf.size());
    TrendRing b;
    b.track("out", "temperature");
    b.track("out", "pressure");
    b.track("in",  "temperature");
    CHECK(b.restore(buf.data(), buf.size()));

    TrendRing::Hour h[TrendRing::HOURS];
    CHECK(b.series("out", "temperature", T0, h));
    CHECK(h[TrendRing::HOURS - 1].min == -3.5f);
    CHECK(b.series("out", "pressure", T0, h));
    CHECK(h[TrendRing::HOURS - 1].min == 1013.2f);
    CHECK(b.series("in", "temperature", T0, h));
    CHECK(h[TrendRing::HOURS - 1].min == 21.0f);

    // An empty ring is a legitimate thing to save — a collector that has been
    // up for ten minutes — and restoring one claims nothing and disturbs
    // nothing, rather than blanking a ring that has since collected readings.
    TrendRing empty;
    std::vector<uint8_t> ebuf(TrendRing::snapshotBytes());
    CHECK(empty.snapshot(ebuf.data(), ebuf.size()) == ebuf.size());
    CHECK(b.restore(ebuf.data(), ebuf.size()));
    CHECK(b.series("out", "temperature", T0, h));
    CHECK(h[TrendRing::HOURS - 1].min == -3.5f);
}

// ---------------------------------------------------------------------------
// THE REASON restore() MERGES BY NAME instead of copying the array over.
//
// There are four slots. A snapshot written before the outdoor sensor was
// renamed carries four series nothing feeds any more; copied in wholesale it
// would fill every slot, leave track() no room for the series this build
// actually reports, and give no sign — the chart would simply never fill in
// again, on a device whose only symptom is a blank strip.
static void test_a_snapshot_cannot_squat_on_the_slots() {
    TrendRing old;
    old.track("old_a", "temperature");
    old.track("old_b", "temperature");
    old.track("old_c", "temperature");
    old.track("old_d", "temperature");
    for (const char* id : {"old_a", "old_b", "old_c", "old_d"})
        old.add(rd(T0, id, "temperature", 7.0f));

    std::vector<uint8_t> buf(TrendRing::snapshotBytes());
    old.snapshot(buf.data(), buf.size());

    // The build that reads it tracks one series, under a new name.
    TrendRing now;
    CHECK(now.track("new_out", "temperature"));
    CHECK(now.restore(buf.data(), buf.size()));

    TrendRing::Hour h[TrendRing::HOURS];
    CHECK(now.series("new_out", "temperature", T0, h));
    CHECK_EQ((int)h[TrendRing::HOURS - 1].count, 0);   // no history to inherit
    CHECK(!now.series("old_a", "temperature", T0, h)); // and none was invented

    // The three free slots are still free, so the rest of the configuration
    // can still be tracked after a restore.
    CHECK(now.track("new_in",   "temperature"));
    CHECK(now.track("new_out",  "pressure"));
    CHECK(now.track("new_out",  "humidity"));
    now.add(rd(T0, "new_in", "temperature", 21.5f));
    CHECK(now.series("new_in", "temperature", T0, h));
    CHECK(h[TrendRing::HOURS - 1].min == 21.5f);
}

int main() {
    RUN(test_a_snapshot_restores_the_same_chart);
    RUN(test_time_passing_needs_no_special_case);
    RUN(test_the_ring_keeps_working_after_a_restore);
    RUN(test_a_damaged_snapshot_is_refused_whole);
    RUN(test_it_refuses_what_it_does_not_recognise);
    RUN(test_dirty_marks_finished_hours_and_not_every_reading);
    RUN(test_every_tracked_series_travels);
    RUN(test_a_snapshot_cannot_squat_on_the_slots);
    return SUMMARY();
}
