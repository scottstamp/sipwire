#include "status_led_pattern.h"

/* Priority: call state, then registration/network, then idle heartbeat.
 *   incoming        blue,  fast blink 150/150 ms
 *   outgoing        cyan,  blink 500/500 ms
 *   active          green, solid
 *   held            green, blink 500/500 ms
 *   booting         white, solid
 *   no network      red,   blink 200/1800 ms
 *   registering     amber, blink 500/500 ms
 *   reg failed      red,   solid
 *   registered      green, 100 ms pulse every 5 s
 *   ...+ voicemail  amber 150 ms, blue 150 ms 250 ms later, every 3 s
 *   not registered  amber, 100 ms pulse every 5 s (no account configured) */

typedef struct {
    sl_rgb_t color;     /* full scale */
    uint16_t on_ms;
    uint16_t period_ms; /* 0 = solid */
} pattern_t;

#define RED   { 255, 0, 0 }
#define GREEN { 0, 255, 0 }
#define BLUE  { 0, 0, 255 }
#define CYAN  { 0, 200, 255 }
#define AMBER { 255, 120, 0 }
#define WHITE { 255, 255, 255 }

#define MWI_PERIOD_MS 3000u
#define MWI_PULSE_MS  150u
#define MWI_SECOND_MS 400u   /* start of the second (blue) pulse */

/* Idle with voicemail waiting: a slow amber/blue double pulse. */
static bool mwi_idle(const sl_state_t *st)
{
    return st->mwi && st->call == SL_CALL_IDLE && !st->booting && st->net == SL_NET_UP &&
           st->reg == SL_REG_REGISTERED;
}

static pattern_t select(const sl_state_t *st)
{
    switch (st->call) {
    case SL_CALL_INCOMING: return (pattern_t){ BLUE, 150, 300 };
    case SL_CALL_OUTGOING: return (pattern_t){ CYAN, 500, 1000 };
    case SL_CALL_ACTIVE:   return (pattern_t){ GREEN, 0, 0 };
    case SL_CALL_HELD:     return (pattern_t){ GREEN, 500, 1000 };
    case SL_CALL_IDLE:     break;
    }
    if (st->booting) {
        return (pattern_t){ WHITE, 0, 0 };
    }
    if (st->net == SL_NET_DOWN) {
        return (pattern_t){ RED, 200, 2000 };
    }
    switch (st->reg) {
    case SL_REG_REGISTERING: return (pattern_t){ AMBER, 500, 1000 };
    case SL_REG_FAILED:      return (pattern_t){ RED, 0, 0 };
    case SL_REG_REGISTERED:  return (pattern_t){ GREEN, 100, 5000 };
    case SL_REG_NONE:        break;
    }
    return (pattern_t){ AMBER, 100, 5000 };
}

static uint8_t scale(uint8_t v, uint8_t max_level)
{
    return (uint8_t)(((unsigned)v * max_level + 127) / 255);
}

sl_rgb_t sl_pattern(const sl_state_t *st, uint32_t now_ms, uint8_t max_level)
{
    if (mwi_idle(st)) {
        uint32_t t = now_ms % MWI_PERIOD_MS;
        sl_rgb_t c = { 0, 0, 0 };
        if (t < MWI_PULSE_MS) {
            c = (sl_rgb_t)AMBER;
        } else if (t >= MWI_SECOND_MS && t < MWI_SECOND_MS + MWI_PULSE_MS) {
            c = (sl_rgb_t)BLUE;
        }
        return (sl_rgb_t){ scale(c.r, max_level), scale(c.g, max_level), scale(c.b, max_level) };
    }
    pattern_t p = select(st);
    if (p.period_ms != 0 && now_ms % p.period_ms >= p.on_ms) {
        return (sl_rgb_t){ 0, 0, 0 };
    }
    return (sl_rgb_t){ scale(p.color.r, max_level), scale(p.color.g, max_level),
                       scale(p.color.b, max_level) };
}
