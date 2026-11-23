#pragma once

#include "audio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Verified Ghost Babel WRAM0 offsets (read-only observation). */
#define ROOM_RUMBLE_P2_FLAG_ADDR 0x05BAu /* $C5BA: 0 = P1, nonzero = P2 */
#define ROOM_RUMBLE_HP_P1_ADDR 0x05E3u   /* $C5E3: Snake health */
#define ROOM_RUMBLE_HP_P2_ADDR 0x0663u   /* $C663: P2 health slot */
#define ROOM_RUMBLE_ALERT_ADDR 0x04E3u   /* $C4E3: bit 7 set during ALERT */

typedef struct RoomRumbleInput {
    bool gameplay_active;
    bool flat_screen;
    uint8_t area;
    uint8_t room;
    uint8_t hp;
    uint8_t alert_state;
    int8_t shake_dx;
    int8_t shake_dy;
    GBAudioRumbleState audio;
} RoomRumbleInput;

typedef struct RoomRumbleOutput {
    bool trigger;
    uint16_t low_freq;
    uint16_t high_freq;
    uint32_t duration_ms;
} RoomRumbleOutput;

typedef struct RoomRumbleTracker {
    bool initialized;
    bool prev_gameplay_active;
    uint8_t prev_area;
    uint8_t prev_room;
    uint8_t prev_hp;
    uint8_t prev_alert;
    int8_t prev_shake_dx;
    int8_t prev_shake_dy;
    uint32_t prev_ch4_triggers;
    uint8_t shot_cooldown_frames;
} RoomRumbleTracker;

/* Read active player HP from WRAM bank 0 ($C5E3 for P1, $C663 for P2). */
uint8_t room_rumble_read_hp(const uint8_t* wram, size_t wram_size);

/* Reset tracker baseline so state loads and startup never emit false triggers. */
void room_rumble_reset(RoomRumbleTracker* tracker);

/* Advance one frame of read-only telemetry and decide any haptic pulse. */
void room_rumble_step(RoomRumbleTracker* tracker,
                      const RoomRumbleInput* input,
                      RoomRumbleOutput* out);
