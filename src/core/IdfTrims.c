// IdfTrims.c — two pieces of the prebuilt ESP-IDF that the 4 MB C3 image pays
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
