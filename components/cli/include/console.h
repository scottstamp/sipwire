#pragma once

#include "esp_console.h"
#include "esp_err.h"

/** Register an extra REPL command (e.g. "call", "hangup" from phone/sip).
 *  May be called before or after console_start(). The `command` string must stay valid
 *  (use a string literal); esp_console keeps the pointer. */
esp_err_t console_register_command(const esp_console_cmd_t *cmd);

/** Start the `sipwire> ` REPL on the configured console (UART / USB-serial-JTAG / CDC)
 *  and register the built-in commands: cfg, wifi, ip, free, tasks, reboot, help. */
esp_err_t console_start(void);
