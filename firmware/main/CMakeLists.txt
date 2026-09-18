# =============================================================================
# firmware/main/CMakeLists.txt: registers the "main" application component
#
# Lists every C source file of the application and the include directory.
# Managed dependencies (network_provisioning, led_strip) are declared in
# idf_component.yml next to this file and linked automatically.
# Module overview: docs/DESIGN.md §3 and §8.
# =============================================================================

idf_component_register(
    SRCS
        "main.c"          # app_main: boot sequence and module wiring
        "prov.c"          # BLE provisioning manager start/stop + events
        "wifi.c"          # Wi-Fi station init, connect/reconnect, IP events
        "status_led.c"    # On-board RGB LED state machine (GPIO8)
        "reset_button.c"  # BOOT button long-press → erase credentials
        "ota.c"           # Wireless firmware updates (HTTP server + mDNS)
        "color_api.c"     # Number 1-5 -> LED colour + random reply (added 2026-09-18)
        "cloud_update.c"  # Automatic firmware updates pulled from the internet (v1.2)
    INCLUDE_DIRS
        "."               # Headers (prov.h, wifi.h, ...) live beside sources
)
