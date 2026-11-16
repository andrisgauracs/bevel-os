set(ASSET_DIR "assets")
set(srcs "main.c" "cursor.c" "pie.S" "gfx.c" "comp.c" "input.c" "serial.c" "perf.c"
         "wm.c" "ui.c" "menu.c" "shell.c" "dlg.c" "apps.c" "apps_basic.c"
         "storage.c" "app_loader.c" "app_elf.c" "app_files.c" "app_editor.c" "app_misc.c" "net.c" "app_net.c" "app_browser.c"
         "diag.c" "usb_input.c" "vnc.c" "vnc_des.c" "${ASSET_DIR}/assets_gen.c")
if(CONFIG_BEVEL_DISPLAY_VGA)
    list(APPEND srcs "vga.c")
else()
    list(APPEND srcs "headless.c")
endif()
idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "." "${ASSET_DIR}"
                       REQUIRES esp_lcd esp_driver_pcnt esp_driver_gpio esp_driver_uart esp_psram spi_flash
                                esp_timer esp_hw_support fatfs wear_levelling vfs xtensa mbedtls esp_app_format usb esp_wifi esp_netif esp_event nvs_flash esp_http_client esp-tls
                                esp_eth
                       EMBED_FILES "../disk/Documents/Welcome.txt" "../disk/Pictures/Bevel.bmp"
                                   "../disk/Apps/Snake.elf" "../disk/Apps/Hello.elf" "../disk/Apps/CrashTest.elf" "../disk/Apps/Doom.elf")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
