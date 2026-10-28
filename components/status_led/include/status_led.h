#pragma once

#include "esp_err.h"

/** Drive the WS2812 status LED on board_cfg()->status_led from app events
 *  (network, registration, call state). No-op if the pin is not connected. */
esp_err_t status_led_init(void);
