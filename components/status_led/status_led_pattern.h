#pragma once

/* Status LED pattern selection: pure C (host-tested), no IDF headers. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SL_NET_DOWN,
    SL_NET_UP,
} sl_net_t;

typedef enum {
    SL_REG_NONE,
    SL_REG_REGISTERING,
    SL_REG_REGISTERED,
    SL_REG_FAILED,
} sl_reg_t;

typedef enum {
    SL_CALL_IDLE,
    SL_CALL_OUTGOING,   /* dialing or ringing out */
    SL_CALL_INCOMING,
    SL_CALL_ACTIVE,
    SL_CALL_HELD,
} sl_call_t;

typedef struct {
    bool booting;       /* until the first network event */
    sl_net_t net;
    sl_reg_t reg;
    sl_call_t call;
    bool mwi;           /* voicemail waiting */
} sl_state_t;

typedef struct {
    uint8_t r, g, b;
} sl_rgb_t;

/** Color to show at `now_ms`, already scaled to `max_level` (0-255). */
sl_rgb_t sl_pattern(const sl_state_t *st, uint32_t now_ms, uint8_t max_level);
