// Mouse pointer sprite state, shared by the display backends (vga.c reads it from its scan-out
// interrupt, so it lives in internal RAM).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "vga.h"

extern uint8_t vga_cur_px[VGA_CUR_MAX_W * VGA_CUR_MAX_H];   // 0xFF = transparent
extern volatile int16_t vga_cur_x, vga_cur_y;                // top-left of the sprite on screen
extern volatile int8_t vga_cur_w, vga_cur_h, vga_cur_hx, vga_cur_hy;
extern volatile bool vga_cur_on;
