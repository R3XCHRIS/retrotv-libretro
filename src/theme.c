#include "theme.h"

#include "osd.h"

static const rtv_theme THEMES[THEME_COUNT] = {
  { "Cable Navy",
    OSD_RGB(8, 16, 72), OSD_RGB(240, 200, 40), OSD_RGB(8, 16, 72), OSD_RGB(255, 214, 64), OSD_RGB(255, 255, 255),
    OSD_RGB(160, 200, 255), OSD_RGB(150, 150, 185), OSD_RGB(28, 44, 120), OSD_RGB(50, 96, 200), OSD_RGB(16, 26, 84),
    OSD_RGB(40, 60, 130) },
  { "Teletext",   /* black page, the eight teletext colours */
    OSD_RGB(0, 0, 0), OSD_RGB(0, 255, 255), OSD_RGB(0, 0, 0), OSD_RGB(255, 255, 0), OSD_RGB(255, 255, 255),
    OSD_RGB(0, 255, 0), OSD_RGB(255, 0, 255), OSD_RGB(0, 0, 170), OSD_RGB(170, 0, 170), OSD_RGB(20, 20, 20),
    OSD_RGB(0, 0, 170) },
  { "Green Phosphor",   /* monochrome terminal */
    OSD_RGB(0, 18, 4), OSD_RGB(60, 255, 110), OSD_RGB(0, 18, 4), OSD_RGB(140, 255, 160), OSD_RGB(110, 255, 140),
    OSD_RGB(60, 200, 90), OSD_RGB(40, 150, 70), OSD_RGB(0, 50, 16), OSD_RGB(0, 110, 40), OSD_RGB(0, 30, 10),
    OSD_RGB(0, 70, 25) },
};

const rtv_theme *TH = &THEMES[THEME_NAVY];

void theme_set(int id) { TH = &THEMES[id >= 0 && id < THEME_COUNT ? id : THEME_NAVY]; }
