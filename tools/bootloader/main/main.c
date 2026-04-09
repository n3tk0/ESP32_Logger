/*
 * Stub app — the GitHub Actions workflow only needs the BOOTLOADER
 * and partition table from this ESP-IDF project. This file satisfies
 * the build system's requirement for an app entry point.
 */
#include <stdio.h>

void app_main(void) {
    printf("Bootloader build stub — not intended to run.\n");
}
