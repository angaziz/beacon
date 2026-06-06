#pragma once
// Panel + safe-area geometry. SAFE_INSET / CORNER_R are FROZEN once the
// cyan-border test (Task 1) confirms them on hardware (FR-PLAT-9, DESIGN.md).
#define SCREEN_W   466
#define SCREEN_H   466
#define CORNER_R   90    // ~20% of 466; confirm on hardware
#define SAFE_INSET 40    // every screen lays out inside this inset; do not reduce

// CO5300 visible-window offset inside its 480x480 GRAM. The visible 466 panel is
// CENTERED in GRAM, so logical (0,0) maps to GRAM (7,7): margin = (480-466)/2 = 7 each side.
// Measured on hardware (P0-A Task 1): offset 0 lost top-left, 14 lost bottom-right, 7 centers it.
// True-even is 7.5 (sub-pixel); 7 leaves a <=1px asymmetry at the extreme edge, immaterial vs SAFE_INSET.
#define LCD_X_OFFSET 7
#define LCD_Y_OFFSET 7
