#include "room_controls.h"

#include <string.h>

/* Active-low GB joypad bit positions matching gbrt / platform_sdl.cpp. */
#define GB_DPAD_RIGHT_BIT 0x01u
#define GB_DPAD_LEFT_BIT 0x02u
#define GB_DPAD_DOWN_BIT 0x08u
#define GB_BTN_A_BIT 0x01u
#define GB_BTN_SELECT_BIT 0x04u
#define GB_BTN_START_BIT 0x08u

static RoomControlMacro decode_requested_macro(const RoomControlsInput* in) {
    if (in->flat_screen) {
        return ROOM_CONTROL_MACRO_NONE;
    }
    if (!in->gameplay_active) {
        if ((in->button_edges & GB_REMASTER_BTN_START) != 0u) {
            return ROOM_CONTROL_MACRO_TITLE_START;
        }
        return ROOM_CONTROL_MACRO_NONE;
    }
    if (in->menu_open) {
        return ROOM_CONTROL_MACRO_NONE;
    }
    if ((in->button_edges & GB_REMASTER_BTN_START) != 0u) {
        return ROOM_CONTROL_MACRO_CODEC;
    }
    if ((in->button_edges & GB_REMASTER_BTN_RB) != 0u) {
        return ROOM_CONTROL_MACRO_WEAPON_NEXT;
    }
    if ((in->button_edges & GB_REMASTER_BTN_LT) != 0u) {
        return ROOM_CONTROL_MACRO_WEAPON_PREV;
    }
    if ((in->button_edges & GB_REMASTER_BTN_LB) != 0u) {
        return ROOM_CONTROL_MACRO_ITEM_NEXT;
    }
    return ROOM_CONTROL_MACRO_NONE;
}

static const char* macro_toast_label(RoomControlMacro macro) {
    switch (macro) {
        case ROOM_CONTROL_MACRO_WEAPON_NEXT:
            return "NEXT WEAPON";
        case ROOM_CONTROL_MACRO_WEAPON_PREV:
            return "PREV WEAPON";
        case ROOM_CONTROL_MACRO_ITEM_NEXT:
            return "NEXT ITEM";
        case ROOM_CONTROL_MACRO_CODEC:
            return "CODEC";
        case ROOM_CONTROL_MACRO_TITLE_START:
        case ROOM_CONTROL_MACRO_NONE:
        default:
            return NULL;
    }
}

static uint8_t macro_direction_dpad_mask(RoomControlMacro macro) {
    switch (macro) {
        case ROOM_CONTROL_MACRO_WEAPON_NEXT:
            return (uint8_t)(0xFFu & ~GB_DPAD_RIGHT_BIT);
        case ROOM_CONTROL_MACRO_WEAPON_PREV:
            return (uint8_t)(0xFFu & ~GB_DPAD_LEFT_BIT);
        case ROOM_CONTROL_MACRO_ITEM_NEXT:
            return (uint8_t)(0xFFu & ~GB_DPAD_DOWN_BIT);
        case ROOM_CONTROL_MACRO_CODEC:
        case ROOM_CONTROL_MACRO_TITLE_START:
        case ROOM_CONTROL_MACRO_NONE:
        default:
            return 0xFFu;
    }
}

static void advance_phase(RoomControls* controls) {
    switch (controls->phase) {
        case ROOM_CONTROL_PHASE_HOLD_OPEN:
            controls->phase = ROOM_CONTROL_PHASE_WAIT_OPEN;
            controls->frames_remaining = ROOM_CONTROL_WAIT_OPEN_FRAMES;
            break;
        case ROOM_CONTROL_PHASE_WAIT_OPEN:
            if (controls->active_macro == ROOM_CONTROL_MACRO_CODEC) {
                controls->phase = ROOM_CONTROL_PHASE_HOLD_CODEC_START;
                controls->frames_remaining = ROOM_CONTROL_HOLD_CODEC_START_FRAMES;
            } else {
                controls->phase = ROOM_CONTROL_PHASE_HOLD_ENTER;
                controls->frames_remaining = ROOM_CONTROL_HOLD_ENTER_FRAMES;
            }
            break;
        case ROOM_CONTROL_PHASE_HOLD_ENTER:
            controls->phase = ROOM_CONTROL_PHASE_WAIT_ENTER;
            controls->frames_remaining = ROOM_CONTROL_WAIT_ENTER_FRAMES;
            break;
        case ROOM_CONTROL_PHASE_WAIT_ENTER:
            controls->phase = ROOM_CONTROL_PHASE_HOLD_STEP;
            controls->frames_remaining = ROOM_CONTROL_HOLD_STEP_FRAMES;
            break;
        case ROOM_CONTROL_PHASE_HOLD_STEP:
            controls->phase = ROOM_CONTROL_PHASE_WAIT_STEP;
            controls->frames_remaining = ROOM_CONTROL_WAIT_STEP_FRAMES;
            break;
        case ROOM_CONTROL_PHASE_WAIT_STEP:
            controls->phase = ROOM_CONTROL_PHASE_HOLD_CONFIRM;
            controls->frames_remaining = ROOM_CONTROL_HOLD_CONFIRM_FRAMES;
            break;
        case ROOM_CONTROL_PHASE_HOLD_CONFIRM:
        case ROOM_CONTROL_PHASE_HOLD_CODEC_START:
            controls->phase = ROOM_CONTROL_PHASE_COOLDOWN;
            controls->frames_remaining = ROOM_CONTROL_COOLDOWN_FRAMES;
            break;
        case ROOM_CONTROL_PHASE_COOLDOWN:
        case ROOM_CONTROL_PHASE_IDLE:
        default:
            room_controls_reset(controls);
            break;
    }
}

void room_controls_reset(RoomControls* controls) {
    if (controls == NULL) {
        return;
    }
    controls->active_macro = ROOM_CONTROL_MACRO_NONE;
    controls->phase = ROOM_CONTROL_PHASE_IDLE;
    controls->frames_remaining = 0u;
}

void room_controls_step(RoomControls* controls,
                        const RoomControlsInput* in,
                        RoomControlsOutput* out) {
    if (out != NULL) {
        out->override_active = false;
        out->dpad_mask = 0xFFu;
        out->buttons_mask = 0xFFu;
        out->toast_label = NULL;
    }
    if (controls == NULL || in == NULL || out == NULL) {
        return;
    }

    if (controls->phase != ROOM_CONTROL_PHASE_IDLE) {
        const bool is_title = (controls->active_macro == ROOM_CONTROL_MACRO_TITLE_START);
        if (in->flat_screen || (!is_title && !in->gameplay_active)) {
            room_controls_reset(controls);
            return;
        }
    } else {
        const RoomControlMacro macro = decode_requested_macro(in);
        if (macro == ROOM_CONTROL_MACRO_NONE) {
            return;
        }
        controls->active_macro = macro;
        if (macro == ROOM_CONTROL_MACRO_TITLE_START) {
            controls->phase = ROOM_CONTROL_PHASE_HOLD_CODEC_START;
            controls->frames_remaining = ROOM_CONTROL_HOLD_CODEC_START_FRAMES;
        } else {
            controls->phase = ROOM_CONTROL_PHASE_HOLD_OPEN;
            controls->frames_remaining = ROOM_CONTROL_HOLD_OPEN_FRAMES;
        }
        out->toast_label = macro_toast_label(macro);
    }

    out->override_active = true;
    out->dpad_mask = 0xFFu;
    out->buttons_mask = 0xFFu;

    switch (controls->phase) {
        case ROOM_CONTROL_PHASE_HOLD_OPEN:
            out->buttons_mask = (uint8_t)(0xFFu & ~GB_BTN_SELECT_BIT);
            break;
        case ROOM_CONTROL_PHASE_HOLD_ENTER:
        case ROOM_CONTROL_PHASE_HOLD_STEP:
            out->dpad_mask = macro_direction_dpad_mask(controls->active_macro);
            break;
        case ROOM_CONTROL_PHASE_HOLD_CONFIRM:
            out->buttons_mask = (uint8_t)(0xFFu & ~GB_BTN_A_BIT);
            break;
        case ROOM_CONTROL_PHASE_HOLD_CODEC_START:
            out->buttons_mask = (uint8_t)(0xFFu & ~GB_BTN_START_BIT);
            break;
        case ROOM_CONTROL_PHASE_WAIT_OPEN:
        case ROOM_CONTROL_PHASE_WAIT_ENTER:
        case ROOM_CONTROL_PHASE_WAIT_STEP:
        case ROOM_CONTROL_PHASE_COOLDOWN:
        case ROOM_CONTROL_PHASE_IDLE:
        default:
            break;
    }

    if (controls->frames_remaining > 1u) {
        controls->frames_remaining--;
    } else {
        advance_phase(controls);
    }
}
