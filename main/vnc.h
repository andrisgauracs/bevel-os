// VNC (RFB 3.3/3.7/3.8) server: the desktop over the network, with keyboard and mouse input.
// Enabled with CONFIG_BEVEL_VNC; with it off these functions do nothing.
#pragma once
#include <stdint.h>

void vnc_start(void);                    // starts the server task (it waits for the network)
void vnc_status(char *buf, int n);       // one line for System Information and the serial console
void vnc_damage(int x, int y, int w, int h);   // framebuffer pixels changed (see display_damage)

// Classic VNC authentication (DES), exposed for the self-test.
void vnc_des_encrypt(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);
void vnc_auth_response(const char *password, const uint8_t challenge[16], uint8_t response[16]);
