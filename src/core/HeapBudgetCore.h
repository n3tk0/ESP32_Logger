#pragma once
// src/core/HeapBudgetCore.h — the arithmetic of HeapBudget.h, without the
// lock, so tests/host/test_heap_budget.cpp can drive every case.
//
// `total` units of heap. A web reply holds one while it exists; a heavy job
// (a TLS export or forecast fetch) wants `heavy` of them. While a heavy job
// waits, no new web reply is admitted, so the replies in flight drain and it
// gets its units instead of waiting behind a steady stream of page loads.

struct HeapBudgetCore {
    int  total;
    int  heavy;
    int  used = 0;
    int  heavyWaiting = 0;    // heavy jobs waiting for their units
    int  heavyActive  = 0;    // heavy jobs holding them

    HeapBudgetCore(int totalUnits, int heavyUnits) : total(totalUnits), heavy(heavyUnits) {}

    bool webAdmissible() const { return !heavyWaiting && used < total; }
    bool tryWeb() {
        if (!webAdmissible()) return false;
        used++;
        return true;
    }
    void giveWeb() { if (used > 0) used--; }

    // The first call registers the job as waiting; a later one succeeds once
    // enough units are free.
    bool tryHeavy(bool alreadyWaiting) {
        if (used + heavy <= total) {
            if (alreadyWaiting) heavyWaiting--;
            used += heavy;
            heavyActive++;
            return true;
        }
        if (!alreadyWaiting) heavyWaiting++;
        return false;
    }
    // Waited long enough: take the units anyway (used may exceed total), so
    // an export is late but never starved; web replies stay refused until
    // the in-flight ones and this job are done.
    void forceHeavy(bool alreadyWaiting) {
        if (alreadyWaiting) heavyWaiting--;
        used += heavy;
        heavyActive++;
    }
    void giveHeavy() {
        used -= heavy;
        if (used < 0) used = 0;
        if (heavyActive > 0) heavyActive--;
    }
};
