#pragma once
/* Game events (coin picked up, pill cleared, ...) detected from the sound chip and RAM writes.
 * Used to drive the controller light, rumble and speaker. */
#include <stdint.h>

typedef struct {
    const char *id;
    const char *name;
    const char *desc;
    int sfx_mask;            /* sound channels (bit0..3) mirrored on the controller speaker; 0 = synthesized beep */
    int sfx_ms;              /* how long the mirrored channels stay open */
    int beep;                /* synthesized beep kind when sfx_mask == 0 */
    uint32_t color;          /* light flash colour */
    int rumble_ms, rumble_str;   /* 0..100 */
} EventDef;

int  events_count(int game);
const EventDef *events_def(int game, int i);

void events_begin(void);        /* before the emulation thread starts */
void events_end(void);
void events_frame(void);        /* emulation thread, once per frame */
int  events_pop(void);          /* main thread: next event index, or -1 */

extern int events_log;          /* developer: print sound triggers */
