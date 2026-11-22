/* Native presenter: re-render the captured frame from hardware tile state.
 *
 * The runtime snapshot already carries everything needed to draw the layer
 * without asking the guest anything: both tile-ID maps, both banks of tile
 * patterns, the CGB attribute maps, the palettes, and SCX/SCY/LCDC. This file
 * turns that into pixels.
 *
 * Why not use gbrt_presentation_compose_widescreen: that compositor draws
 * semantic blocks as flat coloured rectangles. It needs scene.kind ==
 * OVERWORLD and scene.map.sprites_valid, which only a port module can supply,
 * and it deliberately renders no ROM art so the engine repo never displays
 * copyrighted content. It is a headless diagnostic. Actual tile art is sitting
 * in VRAM, so we draw that.
 *
 * Nothing here writes back to the guest. The snapshot is a copy and the
 * capture API declares guest_writeback_allowed = false; this file only reads.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gbrt_presentation.h"

#include "learned_links.h"
#include "room_actors.h"
#include "room_compositor.h"
#include "sprite_shadow.h"

/* The accurate PPU window. Wider output is produced by widening the camera
 * rectangle, not by changing any of these. */
#define NATIVE_HEIGHT GB_PRESENTATION_ACCURATE_HEIGHT

/* Hardware budget: at most ten objects per scanline, chosen in OAM order. */
#define NATIVE_MAX_SPRITES_PER_LINE 10

typedef struct NativePresenter {
    uint32_t abi_version;
    /* Output surface, width >= 160, height 144, owned by the caller. */
    uint32_t width;
    uint32_t height;
    uint32_t* pixels; /* RGB555, same encoding as hardware.accurate_pixels */
    uint32_t stride_pixels;

    /* Per-pixel background colour index (0-3) and CGB attribute priority bit.
     * The sprite pass needs both to decide who wins a pixel: on CGB an object
     * still loses to a background pixel whose colour index is non-zero. */
    uint8_t* bg_color_index;
    uint8_t* bg_attr_priority;

    /* Counters for the divergence report. */
    uint32_t lines_rendered;
    uint32_t window_pixels;
    uint32_t guest_pixels;   /* columns copied straight from the PPU's framebuffer */
    /* Per-slot screen_x from the previous frame, used to notice a sprite
     * crossing the edge of the game's own view. 0xFF means "was off screen",
     * which is where the shadow list takes over. */
    int16_t previous_object_x[GB_PRESENTATION_SPRITE_COUNT];
    /* -1000 means "never seen": a slot that was not live has no position, and
     * a step measured from a position that does not exist is a step in
     * nowhere. */
    int16_t previous_object_y[GB_PRESENTATION_SPRITE_COUNT];
    uint8_t previous_object_count;
    /* Previous frame's full OAM boxes, so column_remember can mask both
     * post-DMA and pre-DMA sprite footprints after drain_after_frame. */
    int16_t prev_sprite_x[GB_PRESENTATION_SPRITE_COUNT];
    int16_t prev_sprite_y[GB_PRESENTATION_SPRITE_COUNT];
    uint8_t prev_sprite_h[GB_PRESENTATION_SPRITE_COUNT];
    /* Column cache: what each world column looked like while it was inside the
     * game's own view, where the pixels are the PPU's and therefore exact.
     *
     * The guest streams tiles: walk away from an object and its tile slots get
     * reused, so reconstructing those columns later reads whatever is in VRAM
     * now. Measured, the copied zone never diverges - 833 of 833 frames - so the
     * only place a wrong picture can come from is the band the panel used to
     * cover and the margins, which are ours to reconstruct. This is what feeds
     * them instead: a column, once seen exactly, is ours for good.
     *
     * Keyed by world coordinate, not by surface, so it survives the camera.
     *
     * The key is the tilemap byte - the world coordinate modulo 256 - and that
     * is not enough on its own. A room wider than 256 pixels makes the guest
     * rewrite the tilemap as the camera advances, so the same byte holds a
     * different piece of the room at each pass through it, and a byte-keyed
     * replay happily serves the older piece. Measured on a 900-frame walk, the
     * 24 columns behind the status panel came back as the yellow floor from a
     * corridor back where the tilemap had the hedge: a band of the picture that
     * changed as the player walked.
     *
     * So every entry also carries the *page* it was written for: the world
     * coordinate divided by 256, one byte per axis. A replay is only a replay if
     * the page agrees. That is exact rather than a heuristic - a column written
     * at game x = g while the camera was at C holds the room at world C + g, and
     * the page is C + g divided by 256, so a request for the same world position
     * matches and a request one corridor over does not. The trailing margin,
     * whose columns were drawn one to thirty-six pixels ago at the very world
     * position being asked for, keeps its entries; the leading margin and the
     * band, whose bytes come from the previous pass, no longer do, and they fall
     * back to the map as they should. */
    uint16_t *column_cache;   /* 256 x 256, RGB555 */
    uint8_t *column_seen;     /* 256 x 256 */
    uint16_t *column_page;    /* 256 x 256, (page_x) | (page_y << 8) */
    /* The guest's own tilemap, shadowed a frame behind so a write can be seen,
     * and a page recorded for each byte the moment it is seen to change. Both
     * maps are shadowed because LCDC bit 3 selects between them and the guest
     * switches; a byte is only "written" if it changed, so the shadow has to be
     * per map or the switch itself would read as a write to every byte at once.
     * See map_write_observe. */
    uint8_t map_shadow[2][GB_PRESENTATION_TILE_MAP_BYTES];
    /* False until the shadow has been seeded from the map as it stood on the
     * first frame, because an empty shadow is not the same thing as a map
     * nothing has written. See map_write_observe. */
    bool map_shadow_seeded;
    uint8_t map_written[2][GB_PRESENTATION_TILE_MAP_BYTES];
    int32_t map_written_x[2][GB_PRESENTATION_TILE_MAP_BYTES];
    int32_t map_written_y[2][GB_PRESENTATION_TILE_MAP_BYTES];
    /* The camera's own position, accumulated in pixels from one frame's SCX/SCY
     * deltas, so a world coordinate can be absolute rather than a byte. Signed
     * and unbounded; only differences are compared. */
    int32_t camera_abs_x;
    int32_t camera_abs_y;
    int32_t camera_prev_scx;
    int32_t camera_prev_scy;
    bool camera_abs_valid;
    /* Which way the camera last travelled: +1 right, -1 left, 0 before it has
     * moved at all. The side it is heading toward is the side the guest has
     * streamed *ahead* and not yet drawn, so it is the side the tilemap may
     * answer for; the side it has passed is answered by the column cache. See
     * margin_is_leading. */
    int8_t camera_dir_x;
    bool column_cache_enabled;
    uint32_t cache_hits;
    uint32_t cache_misses;
    /* The margin's three sources. "Replayed" is the PPU's own pixels, "map" is
     * the guest's forward copy of the room, "backdrop" is a flat picture or a
     * margin the room does not continue into. */
    uint32_t margin_replayed, margin_from_map, margin_backdrop, margin_letterbox;
    /* The room's own map, when the cartridge describes this room. Once aligned
     * it answers the margin before the cache and the guest's map do, because it
     * is the whole room rather than whatever the guest happened to stream. */
    RoomCompositor room;
    bool room_map_enabled;
    uint32_t margin_from_room;
    uint32_t margin_room_miss;
    /* The cartridge the room and its neighbours are decoded from. Held here so
     * the per-frame neighbour check does not need a second set_room call. */
    const uint8_t* rom;
    size_t rom_size;
    uint8_t room_bank;
    /* $CBC2: the transition index the guest has under the player, or 0xFF.
     * A room can have more than one door on the same edge; the margin should
     * show the room behind the door the player is about to take. */
    uint8_t source_node;
    uint32_t margin_from_neighbour;
    uint32_t neighbour_placements;
    /* The edges the cartridge cannot name. The door table stops at area 14, so
     * for the rooms past it the room behind an edge is learned from the guest
     * crossing it, and kept for the session only (learned_links.h). */
    LearnedLinks learned;
    /* Frames whose margin came from a learned room, and the room changes
     * refused because the camera was not parked on an edge to blame. */
    uint32_t learned_placements;
    uint32_t learned_refused;
    /* The map's three answers, and they are not the same answer.
     * `fresh` is a byte the guest wrote for exactly this world tile, `untouched`
     * is a byte nothing has written since this run began watching - the room's
     * own first frames, taken as the room - and `stale` is a byte the guest did
     * write, but for another piece of the room. */
    uint32_t margin_map_fresh, margin_map_untouched, margin_map_stale;
    uint32_t margin_map_fresh_left, margin_map_untouched_left, margin_map_stale_left;
    /* What each slot looked like while it was still wholly inside the game's
     * own view.
     *
     * Copying a sprite at the instant it leaves was wrong, and the fix is the
     * obvious one in hindsight: that is exactly when the guest has started
     * rewriting the tiles. The trace shows the slot holding the soldier at
     * frame 101, wrapping x at 102, and already holding a different sprite by
     * 106 - and the tiles behind the departing sprite are recycled over the
     * same window. Rasterising there captured whatever was mid-write, which is
     * what came out as dirt shaped like a soldier walking through the margin.
     *
     * So the copy is made every frame the sprite is entirely on screen, and the
     * shadow is admitted from the last one of those. */
    ShadowCandidate slot_picture[GB_PRESENTATION_SPRITE_COUNT];
    uint64_t slot_picture_valid;
    SpriteShadowSet shadows;
    uint32_t shadows_admitted;
    uint32_t shadows_refused;
    /* Why the carried shadows are not there. A hand-over that never happens is
     * a different bug from a hand-over that happens and is thrown away, and
     * the counters say which. */
    uint32_t sprites_exiting;      /* a live slot crossed the game's edge */
    uint32_t sprites_exiting_left; /* and it left to the left */
    uint32_t shadows_blocked_panel; /* the game was not playing a room */
    uint32_t shadows_blocked_picture; /* no whole picture was ever taken */
    uint32_t shadows_blocked_room;    /* the room does not continue that way */
    /* The last hand-over, so a run can be looked at where it happened. */
    uint32_t shadows_last_frame;
    int32_t shadows_last_x;
    int32_t shadows_last_y;
    uint32_t shadows_last_width;
    /* Where the last frame's carried pixels landed, in surface columns. */
    uint32_t shadow_pixels_last_frame;
    int32_t shadow_box_x0;
    int32_t shadow_box_x1;
    int32_t shadow_box_y0;
    int32_t shadow_box_y1;
    uint32_t shadow_box_frame;
    uint32_t bg_pixels;
    uint32_t outside_pixels; /* columns the room does not continue into */
    uint32_t sprites_considered;
    uint32_t sprites_over_budget;
    uint32_t sprite_pixels_drawn;
    uint32_t sprite_pixels_hidden_by_bg;
    /* Persistent room actors captured from WRAM0 ($C700..$CB3F) and props ($CB40..$CBBF). */
    RoomActorSnapshot actors;
    RoomActorTracker actor_tracker;
    uint32_t room_actors_drawn;
    uint32_t room_actor_pixels_drawn;
    /* Tileset dump for offline colour proof. Set via native_presenter_set_dump_tiles_dir(). */
    const char* dump_tiles_dir;
    uint8_t last_dump_area;
    uint16_t last_dump_room;
} NativePresenter;

/* Render one background frame. width must be >= 160. `cgb` selects the VRAM
 * palette path and must come from the console mode, not from LCDC. */
bool native_presenter_render_background(
    NativePresenter* presenter,
    const GBPresentationFrame* frame,
    const uint32_t margin_left,
    const uint32_t margin_right,
    bool cgb);

void native_presenter_enable_column_cache(NativePresenter* presenter, bool enabled);

/* Point the presenter at the room the guest is in. The map is decoded once per
 * room change, so this is not a per-frame call. Returns false when the cartridge
 * does not describe the room; the presenter then keeps its previous margin
 * sources instead of drawing a guess. */
bool native_presenter_set_room(NativePresenter* presenter,
                               const uint8_t* rom,
                               size_t rom_size,
                               uint8_t switchable_bank,
                               uint8_t area,
                               uint8_t room);

/* Sync the current room's cell table from the guest's live WRAM bank 5 ($D000)
 * copy when the guest's live atlas pointer matches the loaded room. */
void native_presenter_sync_room_cells(NativePresenter* presenter,
                                      const uint8_t* wram_visual,
                                      size_t wram_bytes,
                                      uint16_t live_atlas_ptr,
                                      uint8_t live_atlas_bank);

/* The guest's own camera, in world pixels ($FFB0/$FFB1, $FFB2). Call once per
 * frame, before rendering: it is what keeps a room wider than 256 pixels from
 * being drawn a page off. Returns false when there is no room map. */
bool native_presenter_place_camera(NativePresenter* presenter, int32_t camera_x, int32_t camera_y);

/* The door the guest has under the player ($CBC2), or 0xFF when there is none.
 * The margin uses it to pick between several doors on the same edge. */
void native_presenter_set_source_node(NativePresenter* presenter, uint8_t node);

/* Supply the persistent room-actor snapshot captured from WRAM0. */
void native_presenter_set_actors(NativePresenter* presenter, const RoomActorSnapshot* actors);

/* Learn which room is across the edge the guest just crossed.
 *
 * Call on the frame $CBC0/$CBC1 changes, *before* set_room: the room the player
 * is leaving is still in place, with the camera the presenter last placed from
 * $FFB0/$FFB1. Which edge that is comes from the camera sitting on one of the
 * room's walkable limits (learned_links_exit_edge); a room change with the
 * camera in the middle of the room is counted in `learned_refused` and teaches
 * nothing, because a door's direction is not visible there.
 *
 * Session only, on purpose: no file, no artifact identity (ADR-009), and two
 * runs over the same frames start from an empty graph and draw the same
 * pixels. */
void native_presenter_learn_transition(NativePresenter* presenter,
                                       uint8_t to_area,
                                       uint8_t to_room);

/* Off only for the audit, which needs to see what the older sources said. */
void native_presenter_enable_room_map(NativePresenter* presenter, bool enabled);

/* Forget every remembered column. Required when the player changes room: the
 * tilemap wraps at 256 pixels and areas repeat inside it, so a column seen in
 * the room just left is not the column being looked at now. */
void native_presenter_clear_column_cache(NativePresenter* presenter);
void native_presenter_set_dump_tiles_dir(NativePresenter* presenter, const char* dir);
void native_presenter_init(
    NativePresenter* presenter,
    uint32_t width,
    uint32_t height,
    uint32_t* pixels,
    uint32_t stride_pixels,
    uint8_t* bg_color_index,
    uint8_t* bg_attr_priority);