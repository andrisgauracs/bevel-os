// The palette, defined once. 64 colours: (B << 4) | (G << 2) | R, each channel 0..3.
#pragma once
#include "display.h"

#define C_BLACK     RGB222(0, 0, 0)
#define C_WHITE     RGB222(3, 3, 3)
#define C_GREY      RGB222(2, 2, 2)   // panel face
#define C_DKGREY    RGB222(1, 1, 1)   // shadow
#define C_NAVY      RGB222(0, 0, 2)   // active title bar, selection
#define C_BLUE      RGB222(0, 0, 3)
#define C_TEAL      RGB222(0, 2, 2)   // default desktop
#define C_RED       RGB222(3, 0, 0)
#define C_DKRED     RGB222(2, 0, 0)
#define C_GREEN     RGB222(0, 3, 0)
#define C_DKGREEN   RGB222(0, 2, 0)
#define C_YELLOW    RGB222(3, 3, 0)
#define C_OLIVE     RGB222(2, 2, 0)
#define C_CYAN      RGB222(0, 3, 3)
#define C_MAGENTA   RGB222(3, 0, 3)
#define C_PURPLE    RGB222(2, 0, 2)
#define C_ORANGE    RGB222(3, 2, 0)
#define C_CREAM     RGB222(3, 3, 2)   // tooltips
#define C_LTGREY    RGB222(3, 3, 3)

// Roles
#define C_FACE        C_GREY
#define C_HILIGHT     C_WHITE
#define C_SHADOW      C_DKGREY
#define C_DKSHADOW    C_BLACK
#define C_TEXT        C_BLACK
#define C_TEXT_DIS    C_DKGREY
#define C_FIELD       C_WHITE
#define C_SEL         C_NAVY
#define C_SEL_TEXT    C_WHITE
#define C_TITLE       C_NAVY
#define C_TITLE_TEXT  C_WHITE
#define C_TITLE_OFF   C_DKGREY
#define C_TITLE_OFF_TEXT C_GREY
