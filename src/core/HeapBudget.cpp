// src/core/HeapBudget.cpp — see HeapBudget.h.
#include "HeapBudget.h"
#include "HeapBudgetCore.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static HeapBudgetCore s_core(HEAP_BUDGET_UNITS, HEAP_BUDGET_HEAVY_UNITS);
static bool           s_enabled = true;
static uint32_t       s_webRefused = 0, s_heavyRuns = 0, s_heavyForced = 0, s_heavyWaitMax = 0;

void heapBudgetBegin(bool enabled) { s_enabled = enabled; }

bool heapBudgetWebAdmissible() {
    if (!s_enabled) return true;
    portENTER_CRITICAL(&s_mux);
    const bool ok = s_core.webAdmissible();
    if (!ok) s_webRefused++;
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

unsigned heapBudgetRetryAfter() {
    portENTER_CRITICAL(&s_mux);
    const bool heavy = s_core.heavyActive || s_core.heavyWaiting;
    portEXIT_CRITICAL(&s_mux);
    return heavy ? 2 : 1;
}

HeapBudgetStats heapBudgetStats() {
    HeapBudgetStats st;
    portENTER_CRITICAL(&s_mux);
    st.enabled        = s_enabled;
    st.total          = s_core.total;
    st.used           = s_core.used;
    st.heavyActive    = s_core.heavyActive > 0;
    st.heavyWaiting   = s_core.heavyWaiting > 0;
    st.webRefused     = s_webRefused;
    st.heavyRuns      = s_heavyRuns;
    st.heavyForced    = s_heavyForced;
    st.heavyWaitMaxMs = s_heavyWaitMax;
    portEXIT_CRITICAL(&s_mux);
    return st;
}

WebUnit::WebUnit() {
    if (!s_enabled) { _ok = true; return; }
    portENTER_CRITICAL(&s_mux);
    _ok = _counted = s_core.tryWeb();
    if (!_ok) s_webRefused++;
    portEXIT_CRITICAL(&s_mux);
}

void WebUnit::release() {
    if (!_counted) return;
    portENTER_CRITICAL(&s_mux);
    s_core.giveWeb();
    portEXIT_CRITICAL(&s_mux);
    _counted = false;
}

HeavyLease::HeavyLease() {
    if (!s_enabled) return;
    const uint32_t t0 = millis();
    bool waiting = false;
    for (;;) {
        portENTER_CRITICAL(&s_mux);
        const bool got = s_core.tryHeavy(waiting);
        if (!got && (uint32_t)(millis() - t0) >= HEAP_BUDGET_WAIT_MS) {
            s_core.forceHeavy(true);
            s_heavyForced++;
            portEXIT_CRITICAL(&s_mux);
            break;
        }
        portEXIT_CRITICAL(&s_mux);
        if (got) break;
        waiting = true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    const uint32_t waited = millis() - t0;
    portENTER_CRITICAL(&s_mux);
    s_heavyRuns++;
    if (waited > s_heavyWaitMax) s_heavyWaitMax = waited;
    portEXIT_CRITICAL(&s_mux);
    _held = true;
}

HeavyLease::~HeavyLease() {
    if (!_held) return;
    portENTER_CRITICAL(&s_mux);
    s_core.giveHeavy();
    portEXIT_CRITICAL(&s_mux);
}
