# Minimal ESP-IDF project — we only need this to build the bootloader.
# The app itself (main.c) is a no-op; we extract bootloader.bin and
# partition-table.bin from the build directory.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bootloader_build)
