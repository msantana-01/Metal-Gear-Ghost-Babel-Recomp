#pragma once

#include "platform_sdl.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum RoomControlMacro {
    ROOM_CONTROL_MACRO_NONE = 0,
    ROOM_CONTROL_MACRO_WEAPON_NEXT = 1,
    ROOM_CONTROL_MACRO_WEAPON_PREV = 2,
    ROOM_CONTROL_MACRO_ITEM_NEXT = 3,
    ROOM_CONTROL_MACRO_CODEC = 4,
    ROOM_CONTROL_MACRO_TITLE_START = 5,
} RoomControlMacro;

typedef enum RoomControlPhase {
    ROOM_CONTROL_PHASE_IDLE = 0,
    ROOM_CONTROL_PHASE_HOLD_OPEN = 1,
    ROOM_CONTROL_PHASE_WAIT_OPEN = 2,
    ROOM_CONTROL_PHASE_HOLD_ENTER = 3,
    ROOM_CONTROL_PHASE_WAIT_ENTER = 4,
    ROOM_CONTROL_PHASE_HOLD_STEP = 5,
    ROOM_CONTROL_PHASE_WAIT_STEP = 6,
    ROOM_CONTROL_PHASE_HOLD_CONFIRM = 7,
    ROOM_CONTROL_PHASE_HOLD_CODEC_START = 8,
    ROOM_CONTROL_PHASE_COOLDOWN = 9,
} RoomControlPhase;

/* Empirically verified frame timings for the Ghost Babel SELECT sub-menu
 * (measured on METALGEARGBBMGE.state1: SELECT opens overview -> wait 29f ->
 * 1st DIR enters weapon/item carousel -> wait 12f -> 2nd DIR steps item ->
 * wait 8f -> A equips & closes carousel; START from overview opens Codec). */
#define ROOM_CONTROL_HOLD_OPEN_FRAMES 3u
#define ROOM_CONTROL_WAIT_OPEN_FRAMES 29u
#define ROOM_CONTROL_HOLD_ENTER_FRAMES 3u
#define ROOM_CONTROL_WAIT_ENTER_FRAMES 12u
#define ROOM_CONTROL_HOLD_STEP_FRAMES 3u
#define ROOM_CONTROL_WAIT_STEP_FRAMES 8u
#define ROOM_CONTROL_HOLD_CONFIRM_FRAMES 3u
#define ROOM_CONTROL_HOLD_CODEC_START_FRAMES 3u
#define ROOM_CONTROL_COOLDOWN_FRAMES 4u

typedef struct RoomControls {
    RoomControlMacro active_macro;
    RoomControlPhase phase;
    uint32_t frames_remaining;
} RoomControls;

typedef struct RoomControlsInput {
    bool gameplay_active;
    bool flat_screen;
    bool menu_open;
    uint32_t button_edges; /* GB_REMASTER_BTN_* rising edges */
} RoomControlsInput;

typedef struct RoomControlsOutput {
    bool override_active;
    uint8_t dpad_mask;    /* Active-low Game Boy D-pad mask (0xFF = released) */
    uint8_t buttons_mask; /* Active-low Game Boy buttons mask (0xFF = released) */
    const char* toast_label;
} RoomControlsOutput;

/* Reset macro state machine cleanly (on boot, shutdown, or savestate load). */
void room_controls_reset(RoomControls* controls);

/* Step deterministic input-only macro state machine once per frame. */
void room_controls_step(RoomControls* controls,
                        const RoomControlsInput* in,
                        RoomControlsOutput* out);
