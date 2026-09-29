// IdfTrims.c — three pieces of the prebuilt ESP-IDF that the 4 MB C3 image pays
// for and gets nothing back from. Each is opt-in per env in platformio.ini,
// so the S3 targets (which have the room, and a core dump partition) keep the
// stock behaviour, and so does an Arduino IDE build, which sets neither flag.
//
// HOW A C FILE CAN REMOVE LIBRARY CODE: the linker pulls an object out of an
// archive (.a) only to satisfy a symbol nothing has defined yet. Our sources
// are linked before the framework archives, so a definition here satisfies the
// reference first and the archive member that would have supplied it — and
// everything only IT needed — is never linked. The size is only saved while
// that stays true; tools/check_flash_trims.py reads the ELF and fails the
// build when a member these replace shows up again.
//
// Measured on xiao_esp32c3 with every optional feature on (firmware.bin):
//   LOGGER_TERSE_TLS_ERRORS   -15,584 bytes
//   LOGGER_NO_COREDUMP        -11,888 bytes
//   LOGGER_TERSE_ESP_ERRORS    -7,312 bytes (all of esp_err_to_name.c, not only
//                                           the name table)

#include <stddef.h>
#include <stdio.h>

#if defined(LOGGER_TERSE_TLS_ERRORS) && LOGGER_TERSE_TLS_ERRORS

// mbedTLS error codes as a number instead of a sentence. The only caller in
// this firmware is WiFiClientSecure (lastError() and its log_e lines), and
// nothing here reads either: the log lines are compiled out at the default
// CORE_DEBUG_LEVEL, and no page shows lastError(). The sentence table behind
// the real one (mbedtls/library/error.c) is ~15 KB of strings.
//
// The code is still enough to diagnose: `-0x2700` is looked up in
// mbedtls/error.h, or with `programs/util/strerror` from any mbedTLS tree.
void mbedtls_strerror(int ret, char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0) return;
    unsigned code = (unsigned)(ret < 0 ? -ret : ret);
    snprintf(buf, buflen, "mbedTLS error -0x%04X", code);
}

#endif

#if defined(LOGGER_NO_COREDUMP) && LOGGER_NO_COREDUMP

// The prebuilt IDF for the C3 is configured to write a core dump to flash on
// a panic, but partitions_balanced.csv has no `coredump` partition to write it
// to. So today, at a panic, all ~12 KB of that code does is look for the
// partition, fail to find it and say so. These two are the entry points the
// IDF calls (startup.c and panic.c); with them defined here, the rest of
// libespcoredump never links.
//
// The panic handler itself is unaffected: the register dump and backtrace
// still go to the console, and the reset reason is still recorded.
//
// IF A coredump PARTITION IS EVER ADDED to the C3 table, remove
// LOGGER_NO_COREDUMP from those envs, or the partition will stay empty.
void esp_core_dump_init(void) {}
void esp_core_dump_to_flash(void *info) { (void)info; }

#endif

#if defined(LOGGER_TERSE_ESP_ERRORS) && LOGGER_TERSE_ESP_ERRORS

// esp_err_t codes as a number instead of their macro name. The real pair
// (esp_common/src/esp_err_to_name.c) carries a table of every ESP_ERR_* name,
// 1.7 KB of entries and the names they point to, ~7.3 KB in all, for log
// lines: the firmware's own callers are two serial lines in
// OtaManager.cpp, and the IDF's are ESP_ERROR_CHECK's abort message and log
// lines compiled out at the default CORE_DEBUG_LEVEL. Both functions are
// defined because both live in that one archive member: a reference to
// either would link it, and with it a second definition of the other.
//
// "ESP_ERR 0x3001" is looked up in esp_err.h or the component's own header
// (0x3000 + n is ESP_ERR_WIFI_BASE, 0x1500 + n ESP_ERR_OTA_BASE, ...).
#include "esp_err.h"

const char *esp_err_to_name_r(esp_err_t code, char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0) return buf;
    if (code == ESP_OK)        snprintf(buf, buflen, "ESP_OK");
    else if (code == ESP_FAIL) snprintf(buf, buflen, "ESP_FAIL");
    else                       snprintf(buf, buflen, "ESP_ERR 0x%x", (unsigned)code);
    return buf;
}

// The real one returns a pointer into its table, which is why nobody frees
// it. This returns one static buffer: two tasks formatting an error at the
// same instant can garble each other's log text, and nothing else.
const char *esp_err_to_name(esp_err_t code)
{
    static char buf[20];
    return esp_err_to_name_r(code, buf, sizeof(buf));
}

#endif
