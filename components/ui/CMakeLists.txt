# Pure LVGL: no IDF APIs here, this component is also built by sim/web.
idf_component_register(
    SRCS "ui.c"
         "ui_theme.c"
         "ui_statusbar.c"
         "scr_home.c"
         "scr_dialer.c"
         "scr_outgoing.c"
         "scr_incoming.c"
         "scr_call.c"
         "scr_ended.c"
         "scr_settings.c"
         "scr_calibrate.c"
         "scr_transfer.c"
         "ui_touch_cal.c"
    INCLUDE_DIRS "include"
)
