#pragma once

#include "esp_err.h"

/** Start the keypad matrix scanner.
 *
 *  Configures the row and column GPIOs from board_cfg() (board_init() must run
 *  first) and starts the scan task on core 0. Each debounced key transition is
 *  posted to SIPWIRE_KEY_EVENT on the default event loop as key_event_t.
 *
 *  Returns ESP_ERR_INVALID_ARG if any row or column is GPIO_NUM_NC or invalid,
 *  ESP_ERR_INVALID_STATE if already started or the key map is not set,
 *  ESP_ERR_NO_MEM if the task could not be created. */
esp_err_t keypad_init(void);
