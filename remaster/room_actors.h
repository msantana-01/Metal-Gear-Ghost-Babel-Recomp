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
#define ROOM_CUTSCENE_MODE_ADDR 0x04E8u       /* $C4E8, WRAM0 (2 or 3 = 71:420C cutscene active) */
#define ROOM_ACTOR_SHADOW_DISABLE_ADDR 0x04EBu /* $C4EB, WRAM0 (nonzero disables shadows) */
#define ROOM_ACTOR_TABLE_ADDR 0x0700u         /* $C700..$CB3F, WRAM0 */
#define ROOM_ACTOR_STRIDE 0x40u
#define ROOM_ACTOR_RAW_SLOTS 17u /* $C700..$CB00 inclusive; index 8 ($C900) is unused gap */
#define ROOM_ACTOR_TABLE_BYTES (ROOM_ACTOR_RAW_SLOTS * ROOM_ACTOR_STRIDE)
#define ROOM_PROP_TABLE_ADDR 0x0B40u /* $CB40..$CBBF, WRAM0 (Bank 21 props/crates) */
#define ROOM_PROP_STRIDE 0x10u
#define ROOM_PROP_SLOTS 8u
#define ROOM_PROP_TABLE_BYTES (ROOM_PROP_SLOTS * ROOM_PROP_STRIDE)
#define ROOM_AREA_ADDR 0x0BC0u /* $CBC0, WRAM0 (area index; <8 selects b1=0x01 for type 0x1C) */
#define ROOM_CUTSCENE_ACTOR_TABLE_ADDR 0x4CEDu /* $DCED..$DD8C, WRAM Bank 4 (71:420C) */
#define ROOM_CUTSCENE_ACTOR_STRIDE 0x14u
#define ROOM_CUTSCENE_ACTOR_SLOTS 8u
#define ROOM_CUTSCENE_ACTOR_TABLE_BYTES (ROOM_CUTSCENE_ACTOR_SLOTS * ROOM_CUTSCENE_ACTOR_STRIDE)

#define ROOM_ACTOR_LOGICAL_SLOTS 16u
#define ROOM_ACTOR_MAX_CARRIED 4u

typedef struct RoomActorSnapshot {
    bool valid;
    uint8_t bank_selector;
    uint8_t shadow_disabled;
    uint8_t table[ROOM_ACTOR_TABLE_BYTES];
    bool props_valid;
    uint8_t props[ROOM_PROP_TABLE_BYTES];
    bool cutscene_actors_valid;
    uint8_t cutscene_mode;
    uint8_t area;
    uint8_t cutscene_actors[ROOM_CUTSCENE_ACTOR_TABLE_BYTES];
} RoomActorSnapshot;

typedef struct RoomActorPrevSlot {
    bool active;
    uint16_t active_frames;
    int32_t world_x;
    int32_t world_y;
    int32_t vx_q8;
    int32_t vy_q8;
    int32_t walk_vx_q8;
    int32_t walk_vy_q8;
    uint8_t idle_frames;
    int32_t last_screen_x;
    int32_t last_screen_y;
    uint8_t bank_selector;
    uint8_t shadow_disabled;
    uint8_t actor_bytes[ROOM_ACTOR_STRIDE];
} RoomActorPrevSlot;

typedef struct RoomActorCarried {
    bool active;
    uint8_t source_slot;
    uint8_t bank_selector;
    uint8_t shadow_disabled;
    uint16_t frames_carried;
    int32_t world_x_q8;
    int32_t world_y_q8;
    int32_t vx_q8;
    int32_t vy_q8;
    uint8_t anim_timer;
    uint8_t actor_bytes[ROOM_ACTOR_STRIDE];
} RoomActorCarried;

typedef struct RoomActorTracker {
    RoomActorPrevSlot prev[ROOM_ACTOR_LOGICAL_SLOTS];
    RoomActorCarried carried[ROOM_ACTOR_MAX_CARRIED];
} RoomActorTracker;

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
    RoomActorTracker* tracker;
} RoomActorRenderArgs;

typedef struct RoomActorRenderStats {
    uint32_t actors_drawn;
    uint32_t pieces_drawn;
    uint32_t pixels_drawn;
} RoomActorRenderStats;

void room_actors_reset(RoomActorSnapshot* snap);
void room_actors_tracker_reset(RoomActorTracker* tracker);
void room_actors_capture(const uint8_t* wram, size_t wram_size, RoomActorSnapshot* out);
void room_actors_render(const RoomActorSnapshot* snap,
                        const RoomActorRenderArgs* args,
                        RoomActorRenderStats* stats_out);
