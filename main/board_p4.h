// Guition JC1060P470-V2 (ESP32-P4 + ESP32-C6): every pin and bus the OS uses, in one place.
// The full hardware notes are in the board's own docs folder. Only the -V2 version of the board: the
// JC1060P470 exists in three versions, and on the others the display does not work.
#pragma once

#define BOARD_NAME            "Guition JC1060P470-V2"
#define BOARD_CHIP            "ESP32-P4"

// What this board has wired up. The OS skips what is missing instead of failing.
#define BOARD_HAS_TOUCH       1       // GT911 on the shared I2C bus
#define BOARD_HAS_RTC         1       // RX8025T at 0x32
#define BOARD_HAS_I2C         1
#define BOARD_HAS_AUDIO       1       // PCM5102 on the JP1 header; the on-board ES8311 is unused
#define BOARD_HAS_SD          1       // microSD on SDMMC slot 0
#define BOARD_HAS_ETH         1       // IP101GR PHY on RMII
#define BOARD_HAS_CAMERA      1       // OV02C10 on MIPI-CSI, SCCB on the shared I2C bus (app_camera.c)
#define BOARD_HAS_WIFI_REMOTE 1       // the esp_wifi API is a remote call into the ESP32-C6
#define BOARD_CONSOLE_USB_JTAG 1      // console, logs and the command channel on the USB-C "USB1" port

// Loadable apps may ask for at most this much PSRAM.
#define BOARD_APP_MEM_MAX     (16 * 1024 * 1024)

// Display, backlight, touch
#define BOARD_PIN_LCD_RST     5
#define BOARD_PIN_LCD_BL      23
#define BOARD_MIPI_LDO_CHAN   3       // P4 internal LDO channel feeding the MIPI DSI/CSI PHY
#define BOARD_MIPI_LDO_MV     2500

// Shared I2C bus: GT911 touch (0x5D), ES8311 codec (0x18), RX8025T clock (0x32), camera SCCB (0x36)
#define BOARD_PIN_I2C_SDA     7
#define BOARD_PIN_I2C_SCL     8
#define BOARD_RTC_ADDR        0x32

// Audio: a PCM5102 module plugged onto the JP1 header (3.3 V, GND, GPIO1-4)
#define BOARD_I2S_WS          1       // PCM5102 LRCK
#define BOARD_I2S_DOUT        2       // PCM5102 DIN
#define BOARD_I2S_BCLK        3       // PCM5102 BCK
#define BOARD_I2S_MCLK        4       // PCM5102 SCK
#define BOARD_PIN_PA_EN       11      // on-board NS4150 speaker amplifier enable (kept off)

// microSD: SDMMC slot 0, 4 bit, powered from the P4's internal LDO channel 4
#define BOARD_SD_CLK          43
#define BOARD_SD_CMD          44
#define BOARD_SD_D0           39
#define BOARD_SD_D1           40
#define BOARD_SD_D2           41
#define BOARD_SD_D3           42
#define BOARD_SD_LDO_CHAN     4

// Ethernet: RMII, IP101GR PHY, 50 MHz reference clock from the PHY
#define BOARD_PIN_ETH_MDC     31
#define BOARD_PIN_ETH_MDIO    52
#define BOARD_PIN_ETH_RST     51
#define BOARD_PIN_ETH_REF_CLK 50
#define BOARD_PIN_ETH_TX_EN   49
#define BOARD_PIN_ETH_TXD0    34
#define BOARD_PIN_ETH_TXD1    35
#define BOARD_PIN_ETH_CRS_DV  28
#define BOARD_PIN_ETH_RXD0    29
#define BOARD_PIN_ETH_RXD1    30
