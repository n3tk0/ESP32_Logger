// Host tests for HeapBudgetCore (src/core/HeapBudgetCore.h): web replies up
// to the total, a heavy job waiting closes the door to new replies and gets
// its units once the ones in flight are done, and a forced one is counted.
#include "src/core/HeapBudgetCore.h"
#include "check.h"

static void test_web_only() {
    HeapBudgetCore b(3, 3);
    CHECK(b.tryWeb()); CHECK(b.tryWeb()); CHECK(b.tryWeb());
    CHECK(!b.tryWeb());
    CHECK(!b.webAdmissible());
    b.giveWeb();
    CHECK(b.webAdmissible());
    CHECK(b.tryWeb());
    b.giveWeb(); b.giveWeb(); b.giveWeb();
    b.giveWeb();                       // one too many: stays at zero
    CHECK_EQ(b.used, 0);
}

static void test_heavy_waits_for_replies() {
    HeapBudgetCore b(3, 3);
    CHECK(b.tryWeb());
    CHECK(!b.tryHeavy(false));         // a reply in flight
    CHECK_EQ(b.heavyWaiting, 1);
    CHECK(!b.webAdmissible());         // no new reply while it waits
    CHECK(!b.tryWeb());
    b.giveWeb();
    CHECK(b.tryHeavy(true));
    CHECK_EQ(b.heavyWaiting, 0);
    CHECK_EQ(b.heavyActive, 1);
    CHECK(!b.tryWeb());                // the job holds every unit
    b.giveHeavy();
    CHECK(b.tryWeb());
    CHECK_EQ(b.used, 1);
}

static void test_heavy_forced() {
    HeapBudgetCore b(3, 3);
    CHECK(b.tryWeb()); CHECK(b.tryWeb());
    CHECK(!b.tryHeavy(false));
    b.forceHeavy(true);                // waited long enough
    CHECK_EQ(b.heavyWaiting, 0);
    CHECK_EQ(b.used, 5);               // over the total until all are done
    CHECK(!b.tryWeb());
    b.giveWeb(); b.giveWeb();
    CHECK(!b.tryWeb());
    b.giveHeavy();
    CHECK_EQ(b.used, 0);
    CHECK(b.tryWeb());
}

static void test_light_heavy() {
    HeapBudgetCore b(3, 2);            // a job that leaves room for one reply
    CHECK(b.tryHeavy(false));
    CHECK(b.tryWeb());
    CHECK(!b.tryWeb());
    b.giveHeavy();
    CHECK(b.tryWeb()); CHECK(b.tryWeb());
}

int main() {
    RUN(test_web_only);
    RUN(test_heavy_waits_for_replies);
    RUN(test_heavy_forced);
    RUN(test_light_heavy);
    return SUMMARY();
}
