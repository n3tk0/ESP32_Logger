// How /api/ingest splits a batch, tested where the web server is not needed.
//
// The endpoint itself cannot be compiled on a desktop — ESPAsyncWebServer —
// so the rules with no I/O in them live in src/web/IngestBatch.h and are
// checked here. All three of them fail SILENTLY on a device:
//
//   • the newest reading of a metric sent to the history queue instead of the
//     mailbox leaves a node that is posting perfectly reading as offline,
//   • a backlog sent to the mailbox instead of the queue is overwritten to its
//     last sample microseconds after arriving,
//   • an age anchored to a clock that is not set files a reading in 1970.
//
// None of those produce an error anywhere. They produce a dashboard that is
// quietly wrong, days later, on a device in a garden.
#include "src/web/IngestBatch.h"
#include "check.h"

#include <vector>

using namespace IngestBatch;

// A batch as it arrives: metric names, oldest first.
struct Batch {
    std::vector<const char*> names;
    const char* operator()(int i) const { return names[(size_t)i]; }
};

static int newest(const Batch& b, int* out, int maxOut) {
    return findNewestPerMetric((int)b.names.size(), b, out, maxOut);
}

// ---------------------------------------------------------------------------
static void test_the_last_of_each_metric_is_the_current_value() {
    // Five minutes of a three-metric node, oldest first — the shape the
    // reference node's ring hands over.
    Batch b{{"temperature", "humidity", "pressure",
             "temperature", "humidity", "pressure",
             "temperature", "humidity", "pressure"}};
    int live[16];
    const int n = newest(b, live, 16);
    CHECK_EQ(n, 3);

    // Indices 6, 7, 8 — the last of each — and nothing earlier.
    CHECK(isNewest(live, n, 6));
    CHECK(isNewest(live, n, 7));
    CHECK(isNewest(live, n, 8));
    for (int i = 0; i < 6; i++) CHECK(!isNewest(live, n, i));
}

// ---------------------------------------------------------------------------
static void test_a_metric_reported_once_is_still_current() {
    // A node that starts reporting humidity mid-outage: one reading, and it is
    // the newest humidity there is. Missing this is how a metric that has just
    // appeared spends an hour invisible on the dashboard.
    Batch b{{"temperature", "temperature", "humidity", "temperature"}};
    int live[16];
    const int n = newest(b, live, 16);
    CHECK_EQ(n, 2);
    CHECK(isNewest(live, n, 3));   // last temperature
    CHECK(isNewest(live, n, 2));   // the only humidity
    CHECK(!isNewest(live, n, 0));
    CHECK(!isNewest(live, n, 1));
}

// ---------------------------------------------------------------------------
static void test_a_single_live_reading_is_the_whole_answer() {
    Batch b{{"temperature"}};
    int live[16];
    CHECK_EQ(newest(b, live, 16), 1);
    CHECK(isNewest(live, 1, 0));

    // An empty batch claims nothing, and asking about an index in one is safe.
    Batch none{{}};
    CHECK_EQ(newest(none, live, 16), 0);
    CHECK(!isNewest(live, 0, 0));
}

// ---------------------------------------------------------------------------
static void test_a_nameless_reading_is_not_anybodys_newest() {
    // A malformed entry must not become the "current value" of the empty
    // metric, and must not stop the real newest behind it from being found.
    Batch b{{"temperature", "", "temperature", ""}};
    int live[16];
    const int n = newest(b, live, 16);
    CHECK_EQ(n, 1);
    CHECK(isNewest(live, n, 2));
    CHECK(!isNewest(live, n, 1));
    CHECK(!isNewest(live, n, 3));

    // Null is the same answer, not a crash: the accessor returns whatever the
    // JSON had, and absent keys are a real case.
    Batch nulls{{nullptr, "humidity", nullptr}};
    CHECK_EQ(newest(nulls, live, 16), 1);
    CHECK(isNewest(live, 1, 1));
}

// ---------------------------------------------------------------------------
static void test_more_metrics_than_slots_keeps_the_newest_ones() {
    // Past the cap the excess goes to the history queue, which is where a
    // reading that is not its metric's newest belongs anyway. What must not
    // happen is writing past the end of the caller's array.
    static const char* kNames[20] = {
        "m0","m1","m2","m3","m4","m5","m6","m7","m8","m9",
        "n0","n1","n2","n3","n4","n5","n6","n7","n8","n9"};
    Batch b{{kNames, kNames + 20}};

    int live[24];
    for (int i = 0; i < 24; i++) live[i] = -999;
    const int n = findNewestPerMetric(20, b, live, 4);
    CHECK_EQ(n, 4);
    for (int i = 4; i < 24; i++) CHECK_EQ(live[i], -999);   // nothing past maxOut

    // Walking backwards means the four it did claim are the four newest.
    CHECK(isNewest(live, n, 19));
    CHECK(isNewest(live, n, 16));
    CHECK(!isNewest(live, n, 15));

    // A zero-length or null destination is answered, not written to.
    CHECK_EQ(findNewestPerMetric(20, b, live, 0), 0);
    CHECK_EQ(findNewestPerMetric(20, b, (int*)nullptr, 8), 0);
}

// ---------------------------------------------------------------------------
static void test_an_age_is_clamped_rather_than_refused() {
    CHECK_EQ((long)clampAge(0), 0L);
    CHECK_EQ((long)clampAge(3600), 3600L);
    CHECK_EQ((long)clampAge(MAX_AGE_S), (long)MAX_AGE_S);

    // A node whose millis() wrapped can offer an age of weeks. It is believed
    // as far as the trend window and no further — the reading still arrives.
    CHECK_EQ((long)clampAge(MAX_AGE_S + 1), (long)MAX_AGE_S);
    CHECK_EQ((long)clampAge(0xFFFFFFFFu), (long)MAX_AGE_S);
}

// ---------------------------------------------------------------------------
// Newest of its metric is not the same as current, and the difference is nine
// deleted readings
// ---------------------------------------------------------------------------
// A node handing over an hour-long outage sends it oldest-first as four
// batches of forty-eight. "Newest in this batch" is true of three readings in
// EVERY one of those batches, not only the last — so a rule that sent the
// newest to the mailbox sent batch 1's three, then let batch 2's overwrite
// them, then batch 3's overwrite those. Nine of sixty-four buffered samples
// deleted on arrival, by the rule whose whole purpose is to stop the mailbox
// eating a backlog.
static void test_newest_in_a_batch_is_not_the_same_as_current() {
    // The last reading of a live post: newest, and taken just now.
    CHECK(isLive(true, 0));
    CHECK(isLive(true, 5));
    CHECK(isLive(true, LIVE_AGE_S));

    // Newest of its metric in a catch-up batch, and an hour old. NOT current —
    // this is the one that was costing readings.
    CHECK(!isLive(true, LIVE_AGE_S + 1));
    CHECK(!isLive(true, 3600));
    CHECK(!isLive(true, MAX_AGE_S));

    // And something with a newer reading behind it is never current, however
    // recent it is.
    CHECK(!isLive(false, 0));
    CHECK(!isLive(false, 3600));

    // The window is the pipeline's own: readingIsBackfilled() calls anything
    // older than this history, so a reading the mailbox called current and the
    // pipeline called backfill would be drawn on the dashboard and skipped by
    // the alert engine.
    CHECK_EQ((long)LIVE_AGE_S, 120L);
}

// ---------------------------------------------------------------------------
static void test_what_counts_as_backfill() {
    const uint32_t NOW = 1750000000u;

    // The ordinary case: an old reading, a collector that knows what time it
    // is, and something newer behind it in the batch.
    CHECK(isBackfill(/*live=*/false, 3600, /*canBackfill=*/true, NOW));

    // The current value is never history — but "current" is isLive()'s answer,
    // not "newest in this batch". An hour-old newest is backfill.
    CHECK(!isBackfill(isLive(true, 5), 5, true, NOW));
    CHECK(isBackfill(isLive(true, 3600), 3600, true, NOW));
    CHECK(isBackfill(isLive(true, MAX_AGE_S), MAX_AGE_S, true, NOW));

    // Taken now.
    CHECK(!isBackfill(false, 0, true, NOW));

    // NO CLOCK, NO BACKFILL. base - age against an unset clock is a 1970 date,
    // which storage keeps and the chart files under an hour that has not
    // happened. Taken as live instead and dated on arrival: a gap beats a
    // wrong date.
    CHECK(!isBackfill(false, 3600, false, NOW));
    CHECK(!isBackfill(false, 3600, false, 0));

    // The same rule at the arithmetic level: an age that reaches back past the
    // start of the epoch has no timestamp to become. Both sides of the edge.
    CHECK(!isBackfill(false, 5000, true, 5000));
    CHECK(!isBackfill(false, 5000, true, 4999));
    CHECK(isBackfill(false, 5000, true, 5001));
}

// ---------------------------------------------------------------------------
// The two rules together, over a batch shaped like a real outage recovery.
static void test_an_hours_backlog_splits_into_one_mailbox_slot_per_metric() {
    const uint32_t NOW = 1750000000u;
    std::vector<const char*> names;
    std::vector<uint32_t>    ages;
    for (int minute = 60; minute >= 0; minute--) {          // oldest first
        names.push_back("temperature"); ages.push_back((uint32_t)minute * 60u);
        names.push_back("humidity");    ages.push_back((uint32_t)minute * 60u);
    }
    Batch b{names};
    int live[16];
    const int n = newest(b, live, 16);
    CHECK_EQ(n, 2);

    int toMailbox = 0, toQueue = 0;
    for (int i = 0; i < (int)names.size(); i++) {
        const uint32_t age = clampAge(ages[(size_t)i]);
        const bool     lv  = isLive(isNewest(live, n, i), age);
        if (isBackfill(lv, age, true, NOW)) toQueue++;
        else                                toMailbox++;
    }

    // 122 readings in, two slots out and 120 distinct measurements queued.
    // The failure this is guarding is 122 in and 2 out.
    CHECK_EQ((long)names.size(), 122L);
    CHECK_EQ(toMailbox, 2);
    CHECK_EQ(toQueue, 120);

    // The two that reached the mailbox are the last two — age zero, this
    // minute — so "last seen" is honest the moment the batch lands.
    CHECK(isNewest(live, n, 120));
    CHECK(isNewest(live, n, 121));
}

// ---------------------------------------------------------------------------
// A batch from the middle of a catch-up puts NOTHING in the mailbox
// ---------------------------------------------------------------------------
// The node holds 192 readings after an hour off the network and hands them
// over in four batches. Only the last one carries anything that is actually
// current; the first three are pure history, and every reading in them has to
// reach the queue. This is the arithmetic that was losing nine of them.
static void test_a_middle_catch_up_batch_is_all_history() {
    const uint32_t NOW = 1750000000u;

    // Batch 2 of 4: sixteen samples of three metrics, all between 30 and 46
    // minutes old, oldest first.
    std::vector<const char*> names;
    std::vector<uint32_t>    ages;
    for (int m = 46; m >= 31; m--) {
        names.push_back("temperature"); ages.push_back((uint32_t)m * 60u);
        names.push_back("humidity");    ages.push_back((uint32_t)m * 60u);
        names.push_back("pressure");    ages.push_back((uint32_t)m * 60u);
    }
    Batch b{names};
    int live[16];
    const int n = newest(b, live, 16);
    CHECK_EQ(n, 3);                      // three metrics have a newest here

    int toMailbox = 0, toQueue = 0;
    for (int i = 0; i < (int)names.size(); i++) {
        const uint32_t age = clampAge(ages[(size_t)i]);
        const bool     lv  = isLive(isNewest(live, n, i), age);
        if (isBackfill(lv, age, true, NOW)) toQueue++;
        else                                toMailbox++;
    }
    CHECK_EQ((long)names.size(), 48L);
    CHECK_EQ(toQueue, 48);               // every one of them is history
    CHECK_EQ(toMailbox, 0);              // and the mailbox is not touched

    // The three that WOULD have gone to the mailbox are the newest of each
    // metric — thirty-one minutes old, and about to be overwritten by the
    // next batch's three. That is the deletion this test exists to catch.
    CHECK(isNewest(live, n, 45));
    CHECK(isNewest(live, n, 46));
    CHECK(isNewest(live, n, 47));
}

// ---------------------------------------------------------------------------
// A collector with no clock must HOLD an aged reading, not swallow it
// ---------------------------------------------------------------------------
// canBackfill is false until NTP lands, and isBackfill() then answers false
// for everything — so an aged reading fell through to the one-slot-per-metric
// mailbox and the batch came back fully accepted. A node handing over an
// hour-long outage to a collector that had just rebooted was told to drop all
// forty-eight readings after forty-seven of them had overwritten each other.
//
// That is precisely the window the node's ring exists for. The endpoint holds
// them now; this is the arithmetic it holds on.
static void test_no_clock_holds_the_backlog() {
    // Nothing is backfill without a clock — that much was already true, and is
    // what made the readings fall through.
    CHECK(!isBackfill(/*live=*/false, 3600, /*canBackfill=*/false, 0));
    CHECK(!isBackfill(false, 60, false, 0));

    // So the endpoint cannot decide from isBackfill() alone. What it tests is
    // "this wanted to be history and there is no clock to date it with":
    for (uint32_t age : {1u, 60u, 121u, 3600u, MAX_AGE_S}) {
        const bool live = isLive(/*newest=*/false, age);
        CHECK(!live);
        CHECK(!isBackfill(live, age, false, 0));   // nowhere to go
        CHECK(age > 0);                            // and so it must be held
    }

    // A reading taken NOW is still deliverable with no clock: the collector
    // dates it on arrival, which is what a zero age means.
    CHECK(isLive(true, 0));
    CHECK(!isBackfill(isLive(true, 0), 0, false, 0));

    // And once the clock lands, the same readings are history again.
    const uint32_t NOW = 1750000000u;
    CHECK(isBackfill(isLive(false, 3600), 3600, true, NOW));
}

int main() {
    RUN(test_the_last_of_each_metric_is_the_current_value);
    RUN(test_a_metric_reported_once_is_still_current);
    RUN(test_a_single_live_reading_is_the_whole_answer);
    RUN(test_a_nameless_reading_is_not_anybodys_newest);
    RUN(test_more_metrics_than_slots_keeps_the_newest_ones);
    RUN(test_an_age_is_clamped_rather_than_refused);
    RUN(test_newest_in_a_batch_is_not_the_same_as_current);
    RUN(test_what_counts_as_backfill);
    RUN(test_a_middle_catch_up_batch_is_all_history);
    RUN(test_no_clock_holds_the_backlog);
    RUN(test_an_hours_backlog_splits_into_one_mailbox_slot_per_metric);
    return SUMMARY();
}
