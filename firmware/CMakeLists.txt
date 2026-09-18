# =============================================================================
# firmware/CMakeLists.txt: top-level ESP-IDF project file
#
# Every ESP-IDF project needs this file at its root. It pulls in the IDF build
# system and names the project; the project name becomes the binary name
# (build/ble_wifi_prov.bin).
# Application code lives in the "main" component (main/CMakeLists.txt).
# See docs/DESIGN.md §7 (Repository layout) and §8 (Firmware).
# =============================================================================

# ESP-IDF v5.x requires CMake 3.16 or newer.
cmake_minimum_required(VERSION 3.16)

# project.cmake sets up the IDF toolchain, component discovery and the
# Component Manager (which downloads deps listed in main/idf_component.yml).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project name: used for the output .bin/.elf file names.
#
# Firmware version: ESP-IDF automatically reads firmware/version.txt (added
# 2026-09-18 for v1.2) and embeds it in the image. The device reports it via
# GET /info and compares it with the cloud manifest to decide whether an
# internet update is needed, so bump version.txt with every release.
project(ble_wifi_prov)
