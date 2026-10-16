// ============================================================================
// src/core/PanicCapture.cpp — a panic's report into the RTC log
//
// The IDF panic handler prints "Guru Meditation", the register dump and the
// backtrace with its own panic_print_* functions, straight to the UART/USB:
// Serial, and so `Log`, never sees them. Nobody has a cable on the Collector,
// so a PANIC in the event log came with no cause at all.
//
// The linker wraps the entry points the rest of the IDF calls (-Wl,--wrap in
// platformio.ini): each __wrap_ copies what it is given into the RTC ring
// (logRingPanicPut) and calls the real one, so the console output is
// unchanged. RTC memory survives the panic reset, and /api/log shows the
// report at the end of "previous boot".
//
// Only calls from other IDF objects can be wrapped. The "Guru Meditation
// Error: Core 0 panic'ed (...)" line itself is printed inside panic.c, so
// __wrap_esp_panic_handler writes its own line with the same facts.
//
// Decoding: MEPC (RISC-V) or PC (Xtensa) and the backtrace addresses go into
//   riscv32-esp-elf-addr2line -pfiaC -e firmware.elf 0x4200xxxx
// with the .elf of the exact build that crashed.
// ============================================================================
#include <stddef.h>
#include <string.h>

#include <esp_attr.h>
#include <esp_arduino_version.h>
#include <esp_private/panic_internal.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include "LogRing.h"
#include "PanicCapture.h"

extern "C" {

void __real_panic_print_str(const char* str);
void __real_panic_print_hex(int h);
void __real_panic_print_dec(int d);
#if ESP_ARDUINO_VERSION_MAJOR < 3
void __real_esp_panic_handler(panic_info_t* info);
#endif
void __real_panic_abort(const char* details) __attribute__((noreturn));

// abort()'s message, kept until the handler runs (see __wrap_panic_abort).
static const char* s_abortDetails = nullptr;

static void put(const char* s) {
    if (s) logRingPanicPut(s, strlen(s));
}

static void putHex(uint32_t v) {           // 8 digits, as panic_print_hex
    char b[8];
    for (int i = 7; i >= 0; i--, v >>= 4) b[i] = "0123456789abcdef"[v & 0xf];
    logRingPanicPut(b, sizeof(b));
}

void __wrap_panic_print_str(const char* str) {
    put(str);
    __real_panic_print_str(str);
}

void __wrap_panic_print_hex(int h) {
    putHex((uint32_t)h);
    __real_panic_print_hex(h);
}

void __wrap_panic_print_dec(int d) {         // 2 digits, as panic_print_dec
    const char b[2] = { d / 10 ? (char)('0' + d / 10 % 10) : ' ', (char)('0' + d % 10) };
    logRingPanicPut(b, sizeof(b));
    __real_panic_print_dec(d);
}

// Arduino core 3.x defines __wrap_esp_panic_handler itself (its own panic
// hook, set_arduino_panic_handler) and links with the same --wrap: a second
// definition here would not link. Under core 3 the [PANIC] line is not
// written; the register dump still is.
#if ESP_ARDUINO_VERSION_MAJOR < 3
void __wrap_esp_panic_handler(panic_info_t* info) {
    if (info) {
        put("\n[PANIC] core ");
        const char c = (char)('0' + (info->core & 7));
        logRingPanicPut(&c, 1);
        put(": ");
        put(info->reason ? info->reason : "?");
        if (info->description) { put(" / "); put(info->description); }
        put(" at 0x");
        putHex((uint32_t)(uintptr_t)info->addr);
        put("\n");
    }
    if (s_abortDetails) {
        put("[PANIC] abort: ");
        put(s_abortDetails);
        put("\n");
    }
    __real_esp_panic_handler(info);
}
#endif

// abort(), assert(), a stack overflow: the message is handed to panic_abort()
// and printed later inside panic.c. panic_abort() is in IRAM because it can
// run with the flash cache off, so this only keeps the pointer (the text may
// be in flash); __wrap_esp_panic_handler copies it once the cache is back.
void IRAM_ATTR __wrap_panic_abort(const char* details) {
    s_abortDetails = details;
    __real_panic_abort(details);
}

// ── Task watchdog: where the starved task was ──────────────────────────────
// A TASK_WDT reset came with no cause at all: the watchdog's own message
// ("did not reset the watchdog in time", the task names) goes out with
// ESP_EARLY_LOGE, not panic_print_*, and the register dump that follows is
// the watchdog ISR's own, on the ISR stack. The IDF calls this weak hook from
// that ISR before it aborts, so it writes two lines first:
//   [TWDT] run <task> pc .. ra .. bt ..      the task the ISR interrupted
//   [TWDT] <watched> pc .. ra .. bt ..       the watched task, e.g. async_tcp
// pc/ra are the task's saved registers, bt the code addresses found on its
// stack above them (a rough backtrace, newest first). Decode each with
// addr2line, as above.
//
// RISC-V on core 2.x only: the saved frame is found the way the port stores
// it (rtos_int_enter / a context switch leave the task's RvExcFrame at
// pxTopOfStack, the TCB's first word: mepc, ra, sp, ...).
#if ESP_ARDUINO_VERSION_MAJOR < 3 && CONFIG_IDF_TARGET_ARCH_RISCV
static void* s_watched = nullptr;

static bool isCode(uint32_t a) {
    return (a >= 0x42000000u && a < 0x42800000u) ||   // flash .text
           (a >= 0x40380000u && a < 0x403E0000u);     // IRAM
}

static void putTask(const char* tag, void* task, int maxBt) {
    if (!task) return;
    const uint32_t* frame = *(const uint32_t* const*)task;   // pxTopOfStack
    put("[TWDT] ");
    put(tag);
    put(pcTaskGetName((TaskHandle_t)task));
    if (!frame || (uint32_t)(uintptr_t)frame < 0x3FC80000u ||
        (uint32_t)(uintptr_t)frame >= 0x3FCE0000u) { put("\n"); return; }
    put(" pc ");  putHex(frame[0]);
    put(" ra ");  putHex(frame[1]);
    put(" bt");
    const uint32_t* sp = (const uint32_t*)(uintptr_t)frame[2];
    int found = 0;
    for (int i = 0; i < 128 && found < maxBt; i++) {
        const uint32_t* w = sp + i;
        if ((uint32_t)(uintptr_t)w < 0x3FC80000u || (uint32_t)(uintptr_t)w >= 0x3FCE0000u) break;
        if (isCode(*w)) { put(" "); putHex(*w); found++; }
    }
    put("\n");
}

void esp_task_wdt_isr_user_handler(void) {
    void* cur = xTaskGetCurrentTaskHandle();
    put("\n");
    putTask("run ", cur, 6);
    if (s_watched && s_watched != cur) putTask("", s_watched, 10);
}
#endif

} // extern "C"

void panicCaptureWatchTask(void* task) {
#if ESP_ARDUINO_VERSION_MAJOR < 3 && CONFIG_IDF_TARGET_ARCH_RISCV
    if (task) s_watched = task;
#else
    (void)task;
#endif
}
