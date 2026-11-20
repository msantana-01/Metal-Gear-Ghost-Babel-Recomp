/* Persistent room actor rendering for widescreen margins.
 *
 * Ghost Babel culls off-viewport actors inside its bank 0E / 60 / 61 / 62 / 64
 * renderers (setting actor +0x3A = 0xFF when outside the 136x144 GBC camera),
 * while still advancing their positions, directions, and animations every frame
 * in WRAM bank 0 ($C700..$CB3F). Reading that table lets the widescreen host
 * draw patrolling guards and other room actors across the full side margins
 * without writing anything back into the guest (guest_writeback_allowed = false).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gbrt_presentation.h"

#define ROOM_ACTOR_BANK_SEL_ADDR 0x06D6u      /* $C6D6, WRAM0 (0..95) */
#define ROOM_ACTOR_SHADOW_DISABLE_ADDR 0x04EBu /* $C4EB, WRAM0 (nonzero disables shadows) */
#define ROOM_ACTOR_TABLE_ADDR 0x0700u         /* $C700..$CB3F, WRAM0 */
#define ROOM_ACTOR_STRIDE 0x40u
#define ROOM_ACTOR_RAW_SLOTS 17u /* $C700..$CB00 inclusive; index 8 ($C900) is unused gap */
#define ROOM_ACTOR_TABLE_BYTES (ROOM_ACTOR_RAW_SLOTS * ROOM_ACTOR_STRIDE)

typedef struct RoomActorSnapshot {
    bool valid;
    uint8_t bank_selector;
    uint8_t shadow_disabled;
    uint8_t table[ROOM_ACTOR_TABLE_BYTES];
} RoomActorSnapshot;

typedef struct RoomActorRenderArgs {
    const uint8_t* rom;
    size_t rom_size;
    const GBPresentationHardwareSnapshot* hw;
    bool cgb;
    uint32_t width;
    uint32_t height;
    uint32_t stride_pixels;
    uint32_t* pixels;
    const uint8_t* bg_color_index;
    const uint8_t* bg_attr_priority;
    int32_t camera_x;
    int32_t camera_y;
    uint32_t game_left;
    uint32_t picture_width;
    uint32_t hud_left;
    uint32_t scene_left;
    uint32_t scene_right;
    uint32_t overlay_left;
    uint32_t scene_map;
    uint32_t bottom_non_scene_rows;
} RoomActorRenderArgs;

typedef struct RoomActorRenderStats {
    uint32_t actors_drawn;
    uint32_t pieces_drawn;
    uint32_t pixels_drawn;
} RoomActorRenderStats;

void room_actors_reset(RoomActorSnapshot* snap);
void room_actors_capture(const uint8_t* wram, size_t wram_size, RoomActorSnapshot* out);
void room_actors_render(const RoomActorSnapshot* snap,
                        const RoomActorRenderArgs* args,
                        RoomActorRenderStats* stats_out);
