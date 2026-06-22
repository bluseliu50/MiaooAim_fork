# Host-side unit test project for main/lunar.c
#
# Run on the ESP-IDF Linux host target (no flashing required):
#   idf.py --preview set-target linux
#   idf.py build
#   ./build/lunar_unit_test.elf          # Linux/macOS
#   .\build\lunar_unit_test.elf          # Windows (MinGW)
#
# This project ONLY compiles the pure-C module main/lunar.c plus its tests.
# It does not touch or rebuild the firmware, and changes no existing logic.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lunar_unit_test)
