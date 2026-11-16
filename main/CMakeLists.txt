set(ASSET_DIR "assets")
idf_component_register(SRCS "main.c" "board.c" "display.c" "touch.c" "audio.c" "clock.c" "gfx.c" "comp.c" "input.c" "serial.c" "perf.c"
                            "wm.c" "ui.c" "menu.c" "shell.c" "dlg.c" "apps.c" "apps_basic.c"
                            "storage.c" "app_loader.c" "app_files.c" "app_editor.c" "app_misc.c" "net.c" "app_net.c" "app_browser.c"
                            "app_player.c" "app_camera.c" "usb_input.c" "${ASSET_DIR}/assets_gen.c"
                       INCLUDE_DIRS "." "${ASSET_DIR}"
                       REQUIRES esp_lcd esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_driver_i2s esp_driver_sdmmc
                                esp_driver_usb_serial_jtag esp_psram spi_flash esp_mm sdmmc
                                esp_timer esp_hw_support fatfs wear_levelling vfs riscv mbedtls esp_app_format
                                esp_wifi esp_eth esp_netif esp_event nvs_flash esp_http_client esp-tls
                       EMBED_FILES "../disk/Documents/Welcome.txt" "../disk/Pictures/Bevel.bmp" "../disk/Music/Chime.wav"
                                   "../disk/Apps/Snake.elf" "../disk/Apps/Hello.elf" "../disk/Apps/CrashTest.elf" "../disk/Apps/Doom.elf")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
# A fault in a loaded app's own code is caught and turned into an error dialog: every CPU exception
# passes through app_loader.c before ESP-IDF's panic handler sees it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=xt_unhandled_exception")
