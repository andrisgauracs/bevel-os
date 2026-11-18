set(ASSET_DIR "assets")

# The two boards differ in what is wired to them, so each brings its own display driver and its own
# peripherals. Everything else - window manager, compositor, apps, drive, network - is common code
# that asks board.h what the board has.
if(IDF_TARGET STREQUAL "esp32s3")
    # ESP32-S3 VGA devkit: the LCD-peripheral scan-out, the PIE kernels that fill it, and the
    # self-test that measures the sync signals on the pins
    set(BOARD_SRCS "display_vga.c" "vga.c" "pie.S" "diag.c")
    set(BOARD_REQUIRES esp_driver_uart esp_driver_pcnt xtensa)
    set(APP_DIR "../disk/Apps/esp32s3")
else()
    # Guition JC1060P470-V2: MIPI-DSI panel, capacitive touch, camera connector, wired Ethernet,
    # microSD on the second SDMMC slot
    set(BOARD_SRCS "display_dsi.c" "touch.c" "app_camera.c")
    set(BOARD_REQUIRES esp_driver_ledc esp_driver_sdmmc sdmmc esp_mm esp_eth riscv espressif__esp_video)
    set(APP_DIR "../disk/Apps/esp32p4")
endif()

# Audio (I2S mixer) and the RTC on I2C are compiled for both boards: on the S3 they are asked for and
# answer "no such hardware" (board.h: BOARD_HAS_AUDIO, BOARD_HAS_RTC), which keeps the Sound Player,
# the Settings sliders and the taskbar clock the same shape on both.
# Loadable programs are built for one CPU each: same source, two toolchains.
# tools/build_apps.sh and tools/build_doom.sh take the target as an argument.
# What goes into the image: the sample files and the four programs for this board. On the S3 the
# 705 kB chime is not worth embedding (there is nothing to make noise), so it is not in the list.
set(EMBEDDED "../disk/Documents/Welcome.txt" "../disk/Pictures/Bevel.bmp"
             "${APP_DIR}/Snake.elf" "${APP_DIR}/Hello.elf" "${APP_DIR}/CrashTest.elf" "${APP_DIR}/Doom.elf")
if(NOT IDF_TARGET STREQUAL "esp32s3")
    list(APPEND EMBEDDED "../disk/Music/Chime.wav")
endif()

idf_component_register(SRCS "main.c" "boot.c" "board.c" "audio.c" "clock.c" ${BOARD_SRCS}
                            "gfx.c" "comp.c" "input.c" "layout_gen.c" "serial.c" "perf.c"
                            "wm.c" "ui.c" "menu.c" "shell.c" "dlg.c" "apps.c" "apps_basic.c"
                            "storage.c" "usb_storage.c" "app_loader.c" "app_files.c" "app_editor.c" "app_misc.c" "net.c" "remote.c" "remote_files.c" "app_net.c" "app_browser.c"
                            "app_player.c" "usb_input.c" "${ASSET_DIR}/assets_gen.c"
                       INCLUDE_DIRS "." "${ASSET_DIR}"
                       REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_usb_serial_jtag
                                esp_psram spi_flash
                                esp_timer esp_hw_support fatfs wear_levelling vfs mbedtls esp_app_format
                                esp_wifi esp_netif esp_event nvs_flash esp_http_client esp_http_server esp-tls
                                ${BOARD_REQUIRES}
                       EMBED_FILES ${EMBEDDED})
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
if(NOT IDF_TARGET STREQUAL "esp32s3")
    # The drawing loops run from cached memory, and where they fall across cache lines changed text and
    # fill speed by 20% from one build to the next, with no change to the code that was measured.
    set_source_files_properties(gfx.c comp.c display_dsi.c PROPERTIES
                                COMPILE_OPTIONS "-falign-functions=32;-falign-loops=32")
    # A fault in a loaded app's own code is caught and turned into an error dialog: every CPU exception
    # passes through app_loader.c before ESP-IDF's panic handler sees it. On the S3 the exception
    # handlers are installed per core instead (app_loader.c).
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xt_unhandled_exception")
endif()
