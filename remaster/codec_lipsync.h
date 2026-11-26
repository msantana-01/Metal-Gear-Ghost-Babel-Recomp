#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gbrt_presentation.h"

/* Read-only Codec portrait lip-sync and async eye-blink state.
 * Never writes back to guest RAM/VRAM (ADR-006 / guest_writeback_allowed = false). */

typedef struct CodecBlinkChannel {
    uint32_t timer;
    uint8_t step;
} CodecBlinkChannel;

typedef struct CodecLipsyncState {
    bool initialized;
    bool codec_active;
    uint32_t frame_counter;
    uint32_t rng;
    uint32_t dialogue_hash;
    uint32_t dialogue_hold_frames;
    uint32_t mouth_tick;
    CodecBlinkChannel blink_left;
    CodecBlinkChannel blink_right;
} CodecLipsyncState;

/* Reset animation state cleanly (on init, savestate load, or leaving Codec). */
void codec_lipsync_reset(CodecLipsyncState* state);

/* Detect active Codec scene structurally and paint surgical mouth + eye-blink
 * micro-patches onto the host RGB555 surface using live CGB palettes. */
void codec_lipsync_apply(CodecLipsyncState* state,
                         const GBPresentationHardwareSnapshot* hw,
                         const uint8_t* wram,
                         size_t wram_size,
                         uint32_t* surface,
                         uint32_t surface_width,
                         uint32_t surface_stride,
                         uint32_t margin_left,
                         bool enabled);
