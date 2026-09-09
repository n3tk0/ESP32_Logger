// The node's store-and-forward queue.
//
// WHY THIS FILE EXISTS
// --------------------
// The ESP8266 node used to read its sensor, fail to POST, print a line and
// throw the values away. A router reboot at 2 am was a hole in the record that
// nothing could fill afterwards. It keeps them now — and "keeps them" is a
// ring buffer, an index that wraps, and an age computed across a counter that
// rolls over every 49.7 days on a device meant to run for months.
//
// Every one of those is wrong QUIETLY. A ring that drops the newest instead of
// the oldest still reports a plausible count; an age that goes backwards at
// the wrap files an hour-old reading as current and draws a chart with a kink
// in it, three weeks from now, on a node in a garden. None of it produces a
// symptom anyone can act on, which is why it is checked here rather than
// discovered there.
#include "node/src/Backlog.h"
#include "check.h"

using namespace NodeBacklog;

static Entry mk(uint32_t ms, float v, const char* metric = "temperature") {
    return Entry{ms, v, metric, "C"};
}

// ---------------------------------------------------------------------------
static void test_it_hands_them_back_oldest_first() {
    Ring r;
    CHECK_EQ(r.count(), 0);
    CHECK(!r.full());

    for (int i = 0; i < 5; i++) r.push(mk(1000u * (uint32_t)i, (float)i));
    CHECK_EQ(r.count(), 5);

    // at(0) is the one that has been waiting longest — the order the collector
    // has to receive them in for the history to be a history.
    for (int i = 0; i < 5; i++) {
        CHECK_EQ((long)r.at(i).ms, (long)(1000u * (uint32_t)i));
        CHECK(r.at(i).value == (float)i);
    }
    CHECK_STREQ(r.at(0).metric, "temperature");
    CHECK_STREQ(r.at(0).unit, "C");
}

// ---------------------------------------------------------------------------
static void test_a_full_ring_drops_the_start_of_the_outage() {
    Ring r;
    for (int i = 0; i < CAPACITY; i++) r.push(mk((uint32_t)i, (float)i));
    CHECK(r.full());
    CHECK_EQ((long)r.dropped(), 0L);
    CHECK(r.at(0).value == 0.0f);

    // One more than it can hold. The OLDEST goes: the recent hours are the
    // ones the dashboard draws and the ones somebody is looking at.
    r.push(mk(9999u, 999.0f));
    CHECK_EQ(r.count(), CAPACITY);
    CHECK_EQ((long)r.dropped(), 1L);
    CHECK(r.at(0).value == 1.0f);                    // 0 is gone
    CHECK(r.at(CAPACITY - 1).value == 999.0f);       // the newest is kept

    // And it keeps being the oldest that goes, however long the outage runs.
    for (int i = 0; i < CAPACITY; i++) r.push(mk(20000u + (uint32_t)i, -1.0f));
    CHECK_EQ(r.count(), CAPACITY);
    CHECK_EQ((long)r.dropped(), (long)(CAPACITY + 1));
    for (int i = 0; i < CAPACITY; i++) CHECK(r.at(i).value == -1.0f);
}

// ---------------------------------------------------------------------------
static void test_dropping_a_sent_batch_leaves_the_rest_in_order() {
    Ring r;
    for (int i = 0; i < 100; i++) r.push(mk((uint32_t)i, (float)i));

    r.drop(48);                       // one POST's worth was accepted
    CHECK_EQ(r.count(), 52);
    CHECK(r.at(0).value == 48.0f);
    CHECK(r.at(51).value == 99.0f);

    // Dropping more than is there empties it rather than going negative — the
    // collector could answer for a batch the queue has since been trimmed of.
    r.drop(1000);
    CHECK_EQ(r.count(), 0);
    r.drop(5);
    CHECK_EQ(r.count(), 0);
    r.drop(-1);
    CHECK_EQ(r.count(), 0);

    // Nothing is dropped for a zero, which is what a failed POST asks for.
    r.push(mk(1u, 7.0f));
    r.drop(0);
    CHECK_EQ(r.count(), 1);
    CHECK(r.at(0).value == 7.0f);
}

// ---------------------------------------------------------------------------
static void test_the_indexes_wrap_without_reordering_anything() {
    Ring r;
    // Push and drop past the end of the array several times over, so head and
    // tail are on opposite sides of the wrap for most of this.
    uint32_t stamp = 0;
    for (int round = 0; round < 7; round++) {
        for (int i = 0; i < CAPACITY - 10; i++) { r.push(mk(stamp, (float)stamp)); stamp++; }
        r.drop(CAPACITY - 20);
    }
    // Whatever is left must still be ascending: the ring is a queue, and a
    // reading that overtakes another is a measurement filed under the wrong
    // minute.
    for (int i = 1; i < r.count(); i++) {
        CHECK(r.at(i).ms > r.at(i - 1).ms);
    }
    CHECK(r.count() > 0);
    CHECK(r.count() <= CAPACITY);
}

// ---------------------------------------------------------------------------
static void test_age_survives_the_49_day_rollover() {
    // Ordinary case first.
    CHECK_EQ((long)ageSeconds(10000u, 4000u), 6L);
    CHECK_EQ((long)ageSeconds(1000u, 1000u), 0L);
    CHECK_EQ((long)ageSeconds(1999u, 1000u), 0L);   // under a second is now

    // THE ONE THIS IS HERE FOR. millis() wraps at 2^32 ms — 49.7 days — and a
    // node in a garden reaches that four times in its first year. A reading
    // taken ten seconds before the wrap, sent ten seconds after it:
    const uint32_t before = 0xFFFFFFFFu - 10000u + 1u;   // 10 s to go
    const uint32_t after  = 10000u;                      // 10 s past
    CHECK_EQ((long)ageSeconds(after, before), 20L);

    // …and the version that reads as caution and is wrong: `now > then` is
    // false here, so a guarded subtraction would answer 0 and file twenty
    // seconds of history as this instant.
    CHECK(!(after > before));

    // An hour across the wrap is still an hour.
    CHECK_EQ((long)ageSeconds(1800000u, 0xFFFFFFFFu - 1800000u + 1u), 3600L);
}

// ---------------------------------------------------------------------------
static void test_what_a_batch_looks_like_on_the_wire() {
    // The shape postBatch() builds: up to 48 readings, oldest first, each with
    // the age it had when the batch was assembled. Ages must DESCEND — the
    // oldest reading is the one that has been waiting longest — or the
    // collector reconstructs the series backwards.
    Ring r;
    const uint32_t t0 = 500000u;
    for (int i = 0; i < 10; i++) r.push(mk(t0 + 60000u * (uint32_t)i, (float)i));

    const uint32_t nowMs = t0 + 60000u * 10u;
    uint32_t prev = 0xFFFFFFFFu;
    for (int i = 0; i < r.count(); i++) {
        const uint32_t age = ageSeconds(nowMs, r.at(i).ms);
        CHECK(age < prev);
        prev = age;
    }
    CHECK_EQ((long)ageSeconds(nowMs, r.at(0).ms), 600L);            // ten minutes
    CHECK_EQ((long)ageSeconds(nowMs, r.at(9).ms), 60L);             // one minute

    // The newest reading of a node whose link is up has an age of zero, which
    // is what tells the collector to treat it as live rather than as backfill.
    r.push(mk(nowMs, 42.0f));
    CHECK_EQ((long)ageSeconds(nowMs, r.at(r.count() - 1).ms), 0L);
}

// ---------------------------------------------------------------------------
static void test_an_hour_of_outage_fits_and_says_what_it_lost() {
    // A three-metric node at the default one-minute interval, off the network.
    Ring r;
    uint32_t ms = 0;
    int minutes = 0;
    while (!r.full()) {
        for (int m = 0; m < 3; m++) r.push(mk(ms, 20.0f + (float)m));
        ms += 60000u;
        minutes++;
    }
    CHECK_EQ(minutes, CAPACITY / 3);
    CHECK(minutes >= 60);            // at least an hour before anything is lost
    CHECK_EQ((long)r.dropped(), 0L);

    // Past that it keeps the most recent hour and counts what it could not
    // keep, so the loss is a number in the log rather than a silence.
    for (int m = 0; m < 3 * 30; m++) r.push(mk(ms + (uint32_t)m, 1.0f));
    CHECK_EQ((long)r.dropped(), 90L);
    CHECK_EQ(r.count(), CAPACITY);
}

int main() {
    RUN(test_it_hands_them_back_oldest_first);
    RUN(test_a_full_ring_drops_the_start_of_the_outage);
    RUN(test_dropping_a_sent_batch_leaves_the_rest_in_order);
    RUN(test_the_indexes_wrap_without_reordering_anything);
    RUN(test_age_survives_the_49_day_rollover);
    RUN(test_what_a_batch_looks_like_on_the_wire);
    RUN(test_an_hour_of_outage_fits_and_says_what_it_lost);
    return SUMMARY();
}
