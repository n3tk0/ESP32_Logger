// Host tests for LazySlots (src/core/LazySlots.h): nothing is allocated until
// a record is claimed, a record that fell out of use is handed out again
// before a new one is made, iteration visits only allocated records, and the
// table stops at N.
#include "src/core/LazySlots.h"
#include "check.h"

struct Rec { bool used; int v; };

static void test_starts_empty() {
    LazySlots<Rec, 4> s;
    CHECK_EQ(s.allocated(), 0u);
    int seen = 0;
    for (Rec& r : s) { (void)r; seen++; }
    CHECK_EQ(seen, 0);
}

static void test_claim_reuses_before_allocating() {
    LazySlots<Rec, 4> s;
    auto isFree = [](const Rec& r) { return !r.used; };
    Rec* a = s.claim(isFree);
    CHECK(a && !a->used && a->v == 0);      // value-initialised
    a->used = true; a->v = 7;
    Rec* b = s.claim(isFree);
    CHECK(b && b != a);
    b->used = true;
    CHECK_EQ(s.allocated(), 2u);
    a->used = false;                         // fell out of use
    Rec* c = s.claim(isFree);
    CHECK(c == a);                           // handed back, not a new one
    CHECK_EQ(s.allocated(), 2u);
    CHECK_EQ(c->v, 7);                       // as it was left: the caller resets it
}

static void test_full_table() {
    LazySlots<Rec, 3> s;
    auto isFree = [](const Rec& r) { return !r.used; };
    for (int i = 0; i < 3; i++) { Rec* r = s.claim(isFree); CHECK(r); r->used = true; r->v = i; }
    CHECK(s.claim(isFree) == nullptr);
    int sum = 0, n = 0;
    for (const Rec& r : s) { sum += r.v; n++; }
    CHECK_EQ(n, 3);
    CHECK_EQ(sum, 0 + 1 + 2);
}

int main() {
    RUN(test_starts_empty);
    RUN(test_claim_reuses_before_allocating);
    RUN(test_full_table);
    return SUMMARY();
}
