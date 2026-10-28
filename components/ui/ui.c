#include "ui.h"

#include <string.h>

static const ui_actions_t *s_actions;
static ui_model_t s_model;
static lv_obj_t *s_status;

static const char *reg_text(ui_reg_state_t reg)
{
    switch (reg) {
    case UI_REG_REGISTERING: return "Registering...";
    case UI_REG_REGISTERED:  return "Ready";
    case UI_REG_FAILED:      return "Registration failed";
    default:                 return "Offline";
    }
}

void ui_init(lv_display_t *disp, const ui_actions_t *actions)
{
    s_actions = actions;
    memset(&s_model, 0, sizeof(s_model));

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "sipwire");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6EDF3), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -12);

    s_status = lv_label_create(scr);
    lv_label_set_text(s_status, reg_text(UI_REG_NONE));
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x8B949E), 0);
    lv_obj_align_to(s_status, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
}

void ui_update(const ui_model_t *model)
{
    if (model->reg != s_model.reg) {
        lv_label_set_text(s_status, reg_text(model->reg));
    }
    s_model = *model;
}

void ui_key(char key, ui_key_action_t action)
{
    (void)key;
    (void)action;
}
