#include "room_rumble.h"

#include <string.h>

static void select_pulse(RoomRumbleOutput* out,
                         uint16_t low_freq,
                         uint16_t high_freq,
                         uint32_t duration_ms) {
    out->trigger = true;
    out->low_freq = low_freq;
    out->high_freq = high_freq;
    out->duration_ms = duration_ms;
}

static int abs_i32(int value) {
    return value < 0 ? -value : value;
}

uint8_t room_rumble_read_hp(const uint8_t* wram, size_t wram_size) {
    if (wram == NULL || wram_size <= ROOM_RUMBLE_HP_P2_ADDR) {
        return 0u;
    }
    const bool p2 = (wram[ROOM_RUMBLE_P2_FLAG_ADDR] != 0u);
    return wram[p2 ? ROOM_RUMBLE_HP_P2_ADDR : ROOM_RUMBLE_HP_P1_ADDR];
}

void room_rumble_reset(RoomRumbleTracker* tracker) {
    if (tracker == NULL) {
        return;
    }
    memset(tracker, 0, sizeof(*tracker));
}

void room_rumble_step(RoomRumbleTracker* tracker,
                      const RoomRumbleInput* input,
                      RoomRumbleOutput* out) {
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (tracker == NULL || input == NULL || out == NULL) {
        return;
    }

    if (tracker->shot_cooldown_frames > 0u) {
        tracker->shot_cooldown_frames--;
    }

    if (!tracker->initialized) {
        tracker->initialized = true;
        tracker->prev_gameplay_active = input->gameplay_active;
        tracker->prev_area = input->area;
        tracker->prev_room = input->room;
        tracker->prev_hp = input->hp;
        tracker->prev_alert = input->alert_state;
        tracker->prev_shake_dx = input->shake_dx;
        tracker->prev_shake_dy = input->shake_dy;
        tracker->prev_ch4_triggers = input->audio.ch4_trigger_count;
        return;
    }

    const bool same_room =
        (input->area == tracker->prev_area && input->room == tracker->prev_room);
    const bool live_scene =
        input->gameplay_active && tracker->prev_gameplay_active && same_room;

    /* Priority 1: Player took damage (HP decreased). */
    if (live_scene && tracker->prev_hp > 0u && input->hp < tracker->prev_hp) {
        if (input->hp == 0u) {
            select_pulse(out, 0xF000u, 0x9000u, 320u);
        } else {
            const uint32_t raw_delta = (uint32_t)(tracker->prev_hp - input->hp);
            const uint32_t step = (raw_delta > 6u) ? 6u : (raw_delta - 1u);
            const uint16_t low = (uint16_t)(0x9000u + (step * (0xD800u - 0x9000u)) / 6u);
            const uint16_t high = (uint16_t)(0x7000u + (step * (0xA800u - 0x7000u)) / 6u);
            const uint32_t duration = 150u + (step * 70u) / 6u;
            select_pulse(out, low, high, duration);
        }
    }

    /* Priority 2: Alert (!) rising edge ($C4E3 bit 7). */
    if (!out->trigger && input->gameplay_active &&
        ((tracker->prev_alert & 0x80u) == 0u) && ((input->alert_state & 0x80u) != 0u)) {
        select_pulse(out, 0xA000u, 0xE800u, 180u);
    }

    /* Priority 3: Deep APU CH4 explosion. */
    const bool ch4_fired =
        live_scene && (input->audio.ch4_trigger_count != tracker->prev_ch4_triggers);
    if (!out->trigger && ch4_fired && input->audio.ch4_volume >= 10u &&
        input->audio.ch4_clock_shift >= 5u && !input->audio.ch4_width_7bit) {
        select_pulse(out, 0xD000u, 0x5000u, 210u);
    }

    /* Priority 4: Active screen shake relative to room camera. */
    if (!out->trigger && live_scene) {
        const int mag = abs_i32((int)input->shake_dx) + abs_i32((int)input->shake_dy);
        if (mag >= 2 &&
            (input->shake_dx != tracker->prev_shake_dx ||
             input->shake_dy != tracker->prev_shake_dy)) {
            const uint32_t clamped = (uint32_t)(mag > 8 ? 8 : mag);
            const uint16_t low =
                (uint16_t)(0x8000u + ((clamped - 2u) * (0xFFFFu - 0x8000u)) / 6u);
            const uint16_t high =
                (uint16_t)(0x3000u + ((clamped - 2u) * (0x7000u - 0x3000u)) / 6u);
            select_pulse(out, low, high, 95u);
        }
    }

    /* Priority 5: Punchy gunshot / melee burst on CH4 with anti-spam cooldown. */
    if (!out->trigger && ch4_fired && input->audio.ch4_volume >= 9u &&
        input->audio.ch4_clock_shift <= 4u && tracker->shot_cooldown_frames == 0u) {
        select_pulse(out, 0x5000u, 0x9000u, 55u);
        tracker->shot_cooldown_frames = 5u;
    }

    /* Priority 6: Entering Codec from active gameplay. */
    if (!out->trigger && tracker->prev_gameplay_active && !input->gameplay_active &&
        !input->flat_screen && same_room) {
        select_pulse(out, 0x1800u, 0x6000u, 65u);
    }

    tracker->prev_gameplay_active = input->gameplay_active;
    tracker->prev_area = input->area;
    tracker->prev_room = input->room;
    tracker->prev_hp = input->hp;
    tracker->prev_alert = input->alert_state;
    tracker->prev_shake_dx = input->shake_dx;
    tracker->prev_shake_dy = input->shake_dy;
    tracker->prev_ch4_triggers = input->audio.ch4_trigger_count;
}
