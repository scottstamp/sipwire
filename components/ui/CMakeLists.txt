# Pure LVGL: no IDF APIs here, this component is also built by sim/web.
idf_component_register(
    SRCS "ui.c"
    INCLUDE_DIRS "include"
)
