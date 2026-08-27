// Host unit tests for src/sensors/RemoteIngest.{h,cpp}
//
// The mailbox now holds two different things and the difference is the point:
//
//   put()            one slot per (node, metric), overwritten. The live value.
//   putHistorical()  a queue. Readings a node buffered through an outage, each
//                    a distinct measurement with its own timestamp.
//
// Feeding the second kind through the first is what this file exists to stop
// happening again: fifteen buffered temperatures went into one slot and
// fourteen of them were overwritten microseconds after arriving, so a node's
// entire outage buffer delivered exactly one reading.
//
// The ordering in drain() is the subtle part and is tested hardest. Latest
// values come first and history fills what is left, because history drains a
// few readings per tick — ahead of the current value it would leave the live
// dashboard blank for minutes while a backlog cleared.
#include <stdint.h>
#include <string.h>

// The implementation is guarded by this, and the CI harness compiles every
// host test with the same bare command line — no -D flags. Defined here so the
// test carries its own configuration rather than depending on how it is
// invoked, which is the same reason test_refresh_cadence.cpp sets its knobs
// before the include.
#define FEATURE_REMOTE_NODES 1

#include "src/sensors/RemoteIngest.cpp"
#include "check.h"

static const uint32_t T0 = 1750000000u;   // a real epoch

static RemoteIngest& fresh() {
    static RemoteIngest r;
    r = RemoteIngest();
    hostSetMillis(100000);
    return r;
}

// ---------------------------------------------------------------------------
// The live slot still behaves like a mailbox
// ---------------------------------------------------------------------------
static void test_put_overwrites() {
    RemoteIngest& ri = fresh();
    CHECK(ri.put("out", "temperature", 20.0f, "C", T0));
    CHECK(ri.put("out", "temperature", 21.0f, "C", T0 + 60));

    SensorReading out[8];
    const int n = ri.drain("out", out, 8, 0);
    CHECK_EQ(n, 1);                       // one slot, not two
    CHECK(out[0].value > 20.99f && out[0].value < 21.01f);
}

// ---------------------------------------------------------------------------
// History is a queue, and that is the whole difference
// ---------------------------------------------------------------------------
static void test_history_keeps_every_reading() {
    RemoteIngest& ri = fresh();
    for (int i = 0; i < 5; i++)
        CHECK(ri.putHistorical("out", "temperature", 10.0f + i, "C", T0 + i * 60));
    CHECK_EQ(ri.historyPending(), 5);

    SensorReading out[8];
    const int n = ri.drain("out", out, 8, 0);
    CHECK_EQ(n, 5);
    CHECK_EQ(ri.historyPending(), 0);

    // Oldest first, each with its own timestamp — the record is only useful in
    // order and only truthful with the times the node actually measured at.
    for (int i = 0; i < 5; i++) {
        CHECK(out[i].value > 9.99f + i && out[i].value < 10.01f + i);
        CHECK_EQ((long long)out[i].timestamp, (long long)(T0 + i * 60));
    }
}

// The ordering that keeps the live dashboard current while a backlog drains.
static void test_latest_comes_before_history() {
    RemoteIngest& ri = fresh();
    for (int i = 0; i < 6; i++)
        ri.putHistorical("out", "temperature", 10.0f + i, "C", T0 + i * 60);
    ri.put("out", "temperature", 30.0f, "C", T0 + 600);
    ri.put("out", "humidity",    55.0f, "%", T0 + 600);

    SensorReading out[4];
    const int n = ri.drain("out", out, 4, 0);
    CHECK_EQ(n, 4);

    // The two live values first...
    CHECK(out[0].value > 29.99f && out[0].value < 30.01f);
    CHECK_STREQ(out[1].metric, "humidity");
    // ...then history, oldest first, in the two slots that were left.
    CHECK(out[2].value > 9.99f  && out[2].value < 10.01f);
    CHECK(out[3].value > 10.99f && out[3].value < 11.01f);

    // And the rest is still queued rather than dropped.
    CHECK_EQ(ri.historyPending(), 4);
}

// A backlog wider than one tick has to survive being drained in pieces, in
// order, with nothing repeated and nothing skipped.
static void test_history_drains_across_ticks() {
    RemoteIngest& ri = fresh();
    const int TOTAL = 20;
    for (int i = 0; i < TOTAL; i++)
        ri.putHistorical("out", "temperature", (float)i, "C", T0 + i * 60);

    int seen = 0;
    for (int tick = 0; tick < 10 && seen < TOTAL; tick++) {
        SensorReading out[3];
        const int n = ri.drain("out", out, 3, 0);
        for (int i = 0; i < n; i++) {
            CHECK(out[i].value > (float)seen - 0.01f);
            CHECK(out[i].value < (float)seen + 0.01f);
            CHECK_EQ((long long)out[i].timestamp, (long long)(T0 + seen * 60));
            seen++;
        }
    }
    CHECK_EQ(seen, TOTAL);
    CHECK_EQ(ri.historyPending(), 0);
}

// Two nodes recovering at once must not eat each other's backlog: each
// plugin drains its own node, and the queue is shared.
static void test_history_is_per_node() {
    RemoteIngest& ri = fresh();
    ri.putHistorical("a", "temperature", 1.0f, "C", T0);
    ri.putHistorical("b", "temperature", 2.0f, "C", T0 + 1);
    ri.putHistorical("a", "temperature", 3.0f, "C", T0 + 2);
    ri.putHistorical("b", "temperature", 4.0f, "C", T0 + 3);

    SensorReading out[8];
    int n = ri.drain("a", out, 8, 0);
    CHECK_EQ(n, 2);
    CHECK(out[0].value > 0.99f && out[0].value < 1.01f);
    CHECK(out[1].value > 2.99f && out[1].value < 3.01f);

    // b's entries were stepped over, not consumed.
    CHECK_EQ(ri.historyPending(), 2);
    n = ri.drain("b", out, 8, 0);
    CHECK_EQ(n, 2);
    CHECK(out[0].value > 1.99f && out[0].value < 2.01f);
    CHECK(out[1].value > 3.99f && out[1].value < 4.01f);
    CHECK_EQ(ri.historyPending(), 0);
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

// A backdated reading whose date is wrong is worse than a gap: it gets filed
// under an hour it did not happen in and corrupts the record it was meant to
// complete. A node with no clock sends 0, and 0 must not become 1970.
static void test_history_refuses_a_reading_with_no_clock() {
    RemoteIngest& ri = fresh();
    CHECK(!ri.putHistorical("out", "temperature", 20.0f, "C", 0));
    CHECK(!ri.putHistorical("out", "temperature", 20.0f, "C", 12345));
    CHECK_EQ(ri.historyPending(), 0);

    // The live path still accepts one, because there the collector stamps on
    // arrival and the value is current by definition.
    CHECK(ri.put("out", "temperature", 20.0f, "C", 0));
}

static void test_history_refuses_nonsense() {
    RemoteIngest& ri = fresh();
    CHECK(!ri.putHistorical(nullptr, "temperature", 1.0f, "C", T0));
    CHECK(!ri.putHistorical("out", nullptr, 1.0f, "C", T0));
    CHECK(!ri.putHistorical("out", "", 1.0f, "C", T0));
    CHECK(!ri.putHistorical("out", "temperature", NAN, "C", T0));
    CHECK(!ri.putHistorical("out", "temperature", INFINITY, "C", T0));
    CHECK_EQ(ri.historyPending(), 0);
}

// Full queue sheds the OLDEST, and says so by returning false — the same rule
// the node applies to its own buffer, for the same reason: the recent readings
// are the ones that say what is happening now.
static void test_full_queue_drops_the_oldest() {
    RemoteIngest& ri = fresh();
    const int CAP = 64;                    // REMOTE_HISTORY_SLOTS
    for (int i = 0; i < CAP; i++)
        CHECK(ri.putHistorical("out", "temperature", (float)i, "C", T0 + i));
    CHECK_EQ(ri.historyPending(), CAP);

    // One too many: accepted, but it cost the oldest and the caller is told.
    CHECK(!ri.putHistorical("out", "temperature", 999.0f, "C", T0 + CAP));
    CHECK_EQ(ri.historyPending(), CAP);

    SensorReading out[1];
    CHECK_EQ(ri.drain("out", out, 1, 0), 1);
    CHECK(out[0].value > 0.99f && out[0].value < 1.01f);   // 0 was shed, not 1
}

// The queue must survive being wrapped around its ring many times over.
static void test_ring_wraps_cleanly() {
    RemoteIngest& ri = fresh();
    int expect = 0, produced = 0;
    for (int round = 0; round < 40; round++) {
        for (int i = 0; i < 5; i++) {
            // Sequenced deliberately: `produced` was read twice in one call
            // here, which is undefined behaviour and which -Wsequence-point
            // caught. A test that is itself undefined proves nothing.
            const int v = produced++;
            ri.putHistorical("out", "temperature", (float)v, "C",
                             T0 + (uint32_t)v);
        }
        SensorReading out[5];
        const int n = ri.drain("out", out, 5, 0);
        CHECK_EQ(n, 5);
        for (int i = 0; i < n; i++) {
            CHECK(out[i].value > (float)expect - 0.01f);
            CHECK(out[i].value < (float)expect + 0.01f);
            expect++;
        }
    }
    CHECK_EQ(ri.historyPending(), 0);
    CHECK_EQ(expect, produced);
}

// ---------------------------------------------------------------------------
// The live path's own guarantees, unchanged
// ---------------------------------------------------------------------------
static void test_staleness_still_marks_the_live_value() {
    RemoteIngest& ri = fresh();
    ri.put("out", "temperature", 20.0f, "C", T0);
    hostAdvanceMillis(700000);             // 11.7 minutes

    SensorReading out[4];
    CHECK_EQ(ri.drain("out", out, 4, 600000), 1);
    CHECK_EQ((int)out[0].quality, (int)QUALITY_ERROR);
}

// History is never marked stale by the same rule. It is old BY DEFINITION —
// that is what makes it history — and flagging it as an error would throw away
// exactly the readings the queue exists to deliver.
static void test_history_is_not_stale_merely_for_being_old() {
    RemoteIngest& ri = fresh();
    ri.putHistorical("out", "temperature", 20.0f, "C", T0);
    hostAdvanceMillis(700000);

    SensorReading out[4];
    CHECK_EQ(ri.drain("out", out, 4, 600000), 1);
    CHECK_EQ((int)out[0].quality, (int)QUALITY_GOOD);
}

int main() {
    RUN(test_put_overwrites);
    RUN(test_history_keeps_every_reading);
    RUN(test_latest_comes_before_history);
    RUN(test_history_drains_across_ticks);
    RUN(test_history_is_per_node);
    RUN(test_history_refuses_a_reading_with_no_clock);
    RUN(test_history_refuses_nonsense);
    RUN(test_full_queue_drops_the_oldest);
    RUN(test_ring_wraps_cleanly);
    RUN(test_staleness_still_marks_the_live_value);
    RUN(test_history_is_not_stale_merely_for_being_old);
    return SUMMARY();
}
