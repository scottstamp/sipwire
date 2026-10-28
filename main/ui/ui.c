#include "ui.h"

void ui_init(lv_display_t *disp)
{
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "sipwire");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6EDF3), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *status = lv_label_create(scr);
    lv_label_set_text(status, "Idle");
    lv_obj_set_style_text_color(status, lv_color_hex(0x8B949E), 0);
    lv_obj_align_to(status, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
}
