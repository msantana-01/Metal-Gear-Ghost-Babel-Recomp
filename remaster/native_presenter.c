/* Native presenter: background layer from hardware tile state.
 *
 * See native_presenter.h for why this exists and why it is not the engine's
 * gbrt_presentation_compose_widescreen.
 *
 * Background only. Sprites are a separate pass: the 10-sprites-per-line
 * budget, the priority bit and the 8x16 tile pairing are the parts of a PPU
 * that a whole-frame renderer gets wrong most easily, so they get their own
 * numbers rather than being averaged into the background's.
 */

#include "native_presenter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "camera_extent.h"

/* LCDC bits, spelled out because the snapshot hands us LCDC as a raw byte and
 * a magic number here would be unauditable. */
#define LCDC_BG_ENABLE 0x01u
#define LCDC_OBJ_ENABLE 0x02u
#define LCDC_OBJ_SIZE 0x04u
#define LCDC_BG_MAP 0x08u
#define LCDC_TILE_DATA 0x10u
#define LCDC_WINDOW_ENABLE 0x20u
#define LCDC_WINDOW_MAP 0x40u
#define LCDC_LCD_ENABLE 0x80u

/* CGB tile attribute bits. */
#define ATTR_PALETTE 0x07u
#define ATTR_VRAM_BANK 0x08u
#define ATTR_FLIP_X 0x20u
#define ATTR_FLIP_Y 0x40u
#define ATTR_PRIORITY 0x80u

#define SCREEN_WIDTH GB_PRESENTATION_ACCURATE_WIDTH
#define TILE_BYTES 16u
#define MAP_DIM 32u
#define TILE_AREA 0x1800u
/* What the surface shows where the room does not continue. */
#define k_outside_colour 0x0000u
#define SECOND_TILE_BANK_OFFSET 0x1000u

/* DMG has no VRAM palettes. The runtime keeps this table for the compat path;
 * we only reach it when the PPU reports a non-CGB mode. */
static const uint16_t k_dmg_palette[4] = {0x7FFF, 0x5294, 0x294A, 0x0000};

/* Per-room snapshot of VRAM tile patterns and BG palettes for neighbour margin
 * rendering. Ghost Babel streams room-specific tile graphics into VRAM bank 1
 * ($9000..$97FF) even between adjacent rooms sharing the same metatile atlas,
 * so drawing a neighbour's metatile IDs with the active room's VRAM corrupts
 * tiles in the widescreen margin. Module-scoped to keep stack-allocated
 * NativePresenter small. */
#define ROOM_VRAM_CACHE_SLOTS 32u

typedef struct RoomVramEntry {
    bool valid;
    uint8_t area;
    uint8_t room;
    uint8_t lcdc;
    uint32_t last_used;
    uint8_t tile_data[2][TILE_AREA];
    uint8_t background_palettes[0x40];
} RoomVramEntry;

static RoomVramEntry g_room_vram_cache[ROOM_VRAM_CACHE_SLOTS];
static uint32_t g_room_vram_cache_tick = 0u;

static void room_vram_cache_clear(void) {
    memset(g_room_vram_cache, 0, sizeof(g_room_vram_cache));
    g_room_vram_cache_tick = 0u;
}

static const RoomVramEntry* room_vram_cache_find(uint8_t area, uint8_t room) {
    for (uint32_t i = 0u; i < ROOM_VRAM_CACHE_SLOTS; ++i) {
        if (g_room_vram_cache[i].valid && g_room_vram_cache[i].area == area &&
            g_room_vram_cache[i].room == room) {
            g_room_vram_cache[i].last_used = ++g_room_vram_cache_tick;
            return &g_room_vram_cache[i];
        }
    }
    return NULL;
}

static void room_vram_cache_store(
    uint8_t area, uint8_t room, const GBPresentationHardwareSnapshot* hw) {
    RoomVramEntry* target = NULL;
    for (uint32_t i = 0u; i < ROOM_VRAM_CACHE_SLOTS; ++i) {
        if (g_room_vram_cache[i].valid && g_room_vram_cache[i].area == area &&
            g_room_vram_cache[i].room == room) {
            target = &g_room_vram_cache[i];
            break;
        }
        if (!g_room_vram_cache[i].valid && target == NULL) {
            target = &g_room_vram_cache[i];
        }
    }
    if (target == NULL) {
        target = &g_room_vram_cache[0];
        for (uint32_t i = 1u; i < ROOM_VRAM_CACHE_SLOTS; ++i) {
            if (g_room_vram_cache[i].last_used < target->last_used) {
                target = &g_room_vram_cache[i];
            }
        }
    }
    target->valid = true;
    target->area = area;
    target->room = room;
    target->lcdc = hw->lcdc;
    target->last_used = ++g_room_vram_cache_tick;
    memcpy(target->tile_data, hw->tile_data, sizeof(target->tile_data));
    memcpy(target->background_palettes, hw->background_palettes,
           sizeof(target->background_palettes));
}

void native_presenter_init(
    NativePresenter* presenter,
    uint32_t width,
    uint32_t height,
    uint32_t* pixels,
    uint32_t stride_pixels,
    uint8_t* bg_color_index,
    uint8_t* bg_attr_priority) {
    memset(presenter, 0, sizeof(*presenter));
    for (uint32_t i = 0; i < GB_PRESENTATION_SPRITE_COUNT; ++i) {
        presenter->previous_object_x[i] = -1000;
        presenter->previous_object_y[i] = -1000;
    }
    presenter->width = width;
    presenter->height = height;
    presenter->pixels = pixels;
    presenter->stride_pixels = stride_pixels;
    presenter->bg_color_index = bg_color_index;
    presenter->bg_attr_priority = bg_attr_priority;
    presenter->source_node = 0xFFu;
    learned_links_init(&presenter->learned);
    room_actors_reset(&presenter->actors);
    room_actors_tracker_reset(&presenter->actor_tracker);
    room_vram_cache_clear();
}

bool native_presenter_set_room(NativePresenter* presenter,
                               const uint8_t* rom,
                               size_t rom_size,
                               uint8_t switchable_bank,
                               uint8_t area,
                               uint8_t room) {
    if (presenter == NULL) {
        return false;
    }
    presenter->rom = rom;
    presenter->rom_size = rom_size;
    presenter->room_bank = switchable_bank;
    room_actors_tracker_reset(&presenter->actor_tracker);
    return room_compositor_set_room(&presenter->room, rom, rom_size, switchable_bank, area, room);
}

void native_presenter_sync_room_cells(NativePresenter* presenter,
                                      const uint8_t* wram_visual,
                                      size_t wram_bytes,
                                      uint16_t live_atlas_ptr,
                                      uint8_t live_atlas_bank) {
    if (presenter == NULL || !presenter->room.has_room || !presenter->room.map.valid) {
        return;
    }
    if (presenter->room.map.atlas_ptr != live_atlas_ptr ||
        presenter->room.map.atlas_bank != live_atlas_bank) {
        return;
    }
    room_map_sync_live_cells(&presenter->room.map, wram_visual, wram_bytes);
}

void native_presenter_learn_transition(NativePresenter* presenter,
                                       uint8_t to_area,
                                       uint8_t to_room) {
    if (presenter == NULL) {
        return;
    }
    /* Nothing to learn from a room that was never mapped, or a camera the
     * presenter did not place itself: without those two the edge is a guess. */
    if (!presenter->room_map_enabled || !presenter->room.has_room ||
        !presenter->room.camera_valid) {
        return;
    }
    if (presenter->room.map.area == to_area && presenter->room.map.room == to_room) {
        return;
    }
    uint8_t edge = 0u;
    if (!learned_links_exit_edge(presenter->room.camera_x, presenter->room.map.limit_min_x,
                                 presenter->room.map.limit_max_x, &edge)) {
        presenter->learned_refused++;
        return;
    }
    (void)learned_links_record(&presenter->learned, presenter->room.map.area,
                               presenter->room.map.room, edge, to_area, to_room);
}

/* Which edge the learned graph is asked about first: the side the camera was
 * last travelling toward, because that is the margin the player is walking into.
 * Before the camera has moved at all, the right side - the same order the rest
 * of the presenter reads the surface in. */
static RoomLinkEdge learned_edge_probe(const NativePresenter* presenter, uint32_t attempt) {
    const RoomLinkEdge first = presenter->camera_dir_x < 0 ? ROOM_LINK_EDGE_LEFT
                                                           : ROOM_LINK_EDGE_RIGHT;
    const RoomLinkEdge second = (first == ROOM_LINK_EDGE_RIGHT) ? ROOM_LINK_EDGE_LEFT
                                                                : ROOM_LINK_EDGE_RIGHT;
    return attempt == 0u ? first : second;
}

bool native_presenter_place_camera(NativePresenter* presenter, int32_t camera_x, int32_t camera_y) {
    if (presenter == NULL) {
        return false;
    }
    return room_compositor_place(&presenter->room, camera_x, camera_y);
}

void native_presenter_set_source_node(NativePresenter* presenter, uint8_t node) {
    if (presenter != NULL) {
        presenter->source_node = node;
    }
}

void native_presenter_set_actors(NativePresenter* presenter, const RoomActorSnapshot* actors) {
    if (presenter == NULL) {
        return;
    }
    if (actors == NULL) {
        room_actors_reset(&presenter->actors);
    } else {
        presenter->actors = *actors;
    }
}

void native_presenter_enable_room_map(NativePresenter* presenter, bool enabled) {
    if (presenter != NULL) {
        presenter->room_map_enabled = enabled;
    }
}

/* Byte offset of an 8x8 pattern inside tile_data[].
 *
 * tile_data[] is a copy of VRAM 0x8000-0x97FF, so 0x1000 inside the array is
 * 0x9000. The two addressing modes are not "two banks of 128" the way it first
 * looks:
 *
 *   LCDC bit 4 clear (0x8000): tile N lives at 0x8000 + N*16 for ALL N in
 *     0x00-0xFF. Tile 0xFF therefore sits at 0x8FF0, not 0x97F0. There is no
 *     0x80 boundary here.
 *   LCDC bit 4 clear (0x8800): the index is signed and the base is 0x9000, so
 *     tile 0x80 is 0x8800 and tile 0x00 is 0x9000.
 *
 * The two branches below were swapped: bit 4 *set* selects unsigned addressing
 * from 0x8000, not signed addressing from 0x9000. Reading the base off the
 * wrong bit shifts every tile by 0x1000, which on the boot screens lands in a
 * region the game never wrote and renders one flat colour.
 *
 * Verified against the accurate PPU's own offset arithmetic. */
static uint32_t tile_pattern_offset(uint8_t lcdc, uint8_t tile_id) {
    if ((lcdc & LCDC_TILE_DATA) == 0u) {
        /* Signed base 0x9000. The arithmetic has to stay signed: casting the
         * negative index to uint32_t first turns it into 0xFFFFFFE1, and
         * adding the 0x1000 base wraps past 32 bits to 0xFFF1. That lands
         * outside the tile area, trips the bounds guard, and the fail-loud
         * colour 0x7FFF - white - paints a full tile row on every frame whose
         * map uses IDs above 0x7F. */
        const int32_t offset =
            (int32_t)SECOND_TILE_BANK_OFFSET + (int32_t)(int8_t)tile_id * (int32_t)TILE_BYTES;
        return (uint32_t)offset;
    }
    return (uint32_t)tile_id * TILE_BYTES;
}

/* 2bpp colour index. Each row is two bitplanes: the low byte holds the low bit
 * of the eight pixels, the high byte the high bit, leftmost pixel at bit 7. */
static uint8_t pattern_color_index(const uint8_t* pattern, uint32_t px, uint32_t py) {
    const uint8_t lo = pattern[py * 2u];
    const uint8_t hi = pattern[py * 2u + 1u];
    const uint32_t bit = 7u - px;
    return (uint8_t)(((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1));
}

static uint16_t read_palette(const uint8_t* palette_ram, uint8_t palette, uint8_t index) {
    const size_t offset = (size_t)palette * 8u + (size_t)index * 2u;
    return (uint16_t)((uint16_t)palette_ram[offset] | ((uint16_t)palette_ram[offset + 1u] << 8));
}

/* The LCDC the guest had while this scanline was drawn.
 *
 * `hw->lcdc` is the register as the frame ended. Ghost Babel rewrites it
 * mid-frame - it selects the second background map for the bottom HUD band and
 * restores the register at vblank - so the end-of-frame value describes the
 * rows that were still on the first map and says nothing about the band's. The
 * runtime records the register per visible row; this reads that record.
 *
 * A row with no record is one the PPU never drew: the LCD was off, or the
 * session has not completed a frame yet. There the register is the only
 * answer. */
static uint8_t row_lcdc(const GBPresentationHardwareSnapshot *hw, uint32_t y) {
    if (y >= GB_PRESENTATION_ACCURATE_HEIGHT) {
        return hw->lcdc;
    }
    const uint8_t recorded = hw->scanline_lcdc[y];
    if ((recorded & LCDC_LCD_ENABLE) == 0u) {
        return hw->lcdc;
    }
    return recorded;
}

static uint32_t lcdc_map_index(uint8_t lcdc) {
    return (lcdc & LCDC_BG_MAP) != 0u ? 1u : 0u;
}

/* Which map the frame's scene was drawn from.
 *
 * Not the register's value at the end of the frame. On a room change the guest
 * flips the map mid-frame and the last write can name a map no row was drawn
 * from: measured at the 2:4 -> 2:3 door, all 144 rows were assembled from map 0
 * and the frame ended on map 1. Every row then looked like the HUD band, the
 * panel's columns were copied verbatim, and the status read-out appeared twice
 * on the surface - once in the picture, once in the strip. The scene is
 * whichever map most rows were drawn from; on every other frame that is the
 * register's value, because the band is 8 rows and the scene is 136. */
static uint32_t frame_scene_map(const GBPresentationHardwareSnapshot *hw) {
    uint32_t zero = 0u;
    for (uint32_t y = 0; y < GB_PRESENTATION_ACCURATE_HEIGHT; ++y) {
        if (lcdc_map_index(row_lcdc(hw, y)) == 0u) {
            zero++;
        }
    }
    return (zero * 2u > GB_PRESENTATION_ACCURATE_HEIGHT) ? 0u : 1u;
}

/* Was this row assembled from the frame's scene map?
 *
 * Rows that were not are the ones the guest composited from the other map -
 * the HUD band, in this game. They are HUD, not room: the cache must not
 * remember them (a replayed band row is HUD pixels in the margin) and must not
 * answer for them. */
static bool row_is_scene_map(const GBPresentationHardwareSnapshot *hw, uint32_t y,
                             uint32_t scene_map) {
    return lcdc_map_index(row_lcdc(hw, y)) == scene_map;
}

/* Resolve one background pixel. x/y are screen coordinates; camera_x is added
 * so the same code serves 160px and a wider window without a second path.
 *
 * The map read here is the *room's*: `hw->lcdc` is the register as the frame
 * ended, which is the map the room was drawn from. The rows the guest assembled
 * from the other map are its HUD band, and the margin still wants the room
 * under it - the band is HUD, and the surface's scene belongs to the room on
 * every row. */
/* One background tile, resolved to a colour. The tile id and attribute are
 * passed in rather than looked up here, because the margin takes them from the
 * room's cartridge map and the picture takes them from the guest's live one.
 * Everything after that point is the same hardware rule for both.
 *
 * The pixel position is in *world* space: the flips mirror the tile, so which
 * bit of the pattern a pixel reads depends on where the tile sits in the room,
 * not on where it sits on the surface. */
static uint16_t sample_pattern_raw(
    uint8_t lcdc,
    const uint8_t tile_data[2][TILE_AREA],
    const uint8_t background_palettes[0x40],
    uint8_t tile_id,
    uint8_t attribute,
    uint32_t world_x,
    uint32_t world_y,
    bool cgb,
    uint8_t* out_color_index,
    uint8_t* out_attr_priority) {
    uint32_t px = world_x & 7u;
    uint32_t py = world_y & 7u;
    if ((attribute & ATTR_FLIP_X) != 0u) {
        px = 7u - px;
    }
    if ((attribute & ATTR_FLIP_Y) != 0u) {
        py = 7u - py;
    }

    const uint32_t bank = (cgb && (attribute & ATTR_VRAM_BANK) != 0u) ? 1u : 0u;
    const uint32_t pattern_offset = tile_pattern_offset(lcdc, tile_id);
    if (pattern_offset + TILE_BYTES > TILE_AREA) {
        return 0x7FFFu; /* unreachable for a valid tile ID; fail loud in colour */
    }
    const uint8_t* pattern = &tile_data[bank][pattern_offset];
    const uint8_t color_index = pattern_color_index(pattern, px, py);

    *out_color_index = color_index;
    *out_attr_priority = (uint8_t)((attribute & ATTR_PRIORITY) != 0u ? 1u : 0u);
    if (!cgb) {
        return k_dmg_palette[color_index];
    }
    return read_palette(background_palettes, (uint8_t)(attribute & ATTR_PALETTE), color_index);
}

static uint16_t sample_pattern(
    const GBPresentationHardwareSnapshot* hw,
    uint8_t tile_id,
    uint8_t attribute,
    uint32_t world_x,
    uint32_t world_y,
    bool cgb,
    uint8_t* out_color_index,
    uint8_t* out_attr_priority) {
    return sample_pattern_raw(hw->lcdc, hw->tile_data, hw->background_palettes, tile_id,
                              attribute, world_x, world_y, cgb, out_color_index,
                              out_attr_priority);
}

static uint16_t sample_background(
    const GBPresentationHardwareSnapshot* hw,
    uint32_t screen_x,
    uint32_t screen_y,
    uint32_t camera_x,
    bool cgb,
    uint8_t* out_color_index,
    uint8_t* out_attr_priority) {
    /* The row's own map, not the register's: the guest can switch the map
     * mid-frame, and a pixel was drawn from the map that was selected on its
     * row. */
    const uint8_t lcdc = row_lcdc(hw, screen_y);
    const uint32_t map_index = lcdc_map_index(lcdc);
    const uint32_t world_x = screen_x + camera_x + (uint32_t)hw->scx;
    const uint32_t world_y = screen_y + (uint32_t)hw->scy;
    const uint32_t map_offset =
        ((world_y & 0xFFu) >> 3) * MAP_DIM + ((world_x & 0xFFu) >> 3);

    return sample_pattern(hw,
                          hw->tile_ids[map_index][map_offset],
                          hw->tile_attributes[map_index][map_offset],
                          world_x,
                          world_y,
                          cgb,
                          out_color_index,
                          out_attr_priority);
}

/* The tone that surrounds the picture when the room does not continue.
 *
 * Derived from the frame's exact five-bit colour histogram: the colour with
 * the most pixels wins (breaking ties toward black 0u).
 *
 * Previously a near-black branch (mean brightness <= 2) fell back to the most
 * saturated non-black colour on the frame, which flooded mostly-black menus,
 * splash screens, and intro cutscenes with green (SELECT GAME LEVEL), red
 * (KCET), or flashing blue/purple/green borders during fades. Returning the
 * dominant colour directly keeps white logos on white, solid colour fades
 * smooth, and dark screens (KCET, menus, intro cutscenes) cleanly letterboxed
 * in black.
 *
 * 32768 counters is 64KB of static array, cleared and refilled per frame, and
 * the two scans over it are 32768 comparisons each. That is the whole cost. */
#define BACKDROP_COLOURS 32768u

static uint16_t derive_backdrop(const GBPresentationHardwareSnapshot* hw) {
    static uint16_t counts[BACKDROP_COLOURS];
    memset(counts, 0, sizeof(counts));

    for (uint32_t i = 0; i < (uint32_t)SCREEN_WIDTH * (uint32_t)NATIVE_HEIGHT; ++i) {
        const uint16_t c = hw->accurate_pixels[i] & 0x7FFFu;
        ++counts[c];
    }

    uint32_t dominant = 0u;
    for (uint32_t i = 1u; i < BACKDROP_COLOURS; ++i) {
        if (counts[i] > counts[dominant]) {
            dominant = i;
        }
    }
    if (counts[dominant] == 0u) {
        return 0u;
    }
    return (uint16_t)dominant;
}


#define OAM_BG_PRIORITY 0x80u
#define OAM_OBJ_PALETTE 0x10u
#define OAM_CGB_BANK 0x08u
#define OAM_OBJ_FLIP_Y 0x40u
#define OAM_OBJ_FLIP_X 0x20u
#define OAM_CGB_PALETTE 0x07u

/* DMG has two four-colour object palettes, both greyscale-to-white ramps. */
static const uint16_t k_dmg_object_palette[4] = {0x7FFF, 0x5294, 0x294A, 0x0000};

#define SPRITE_SHADOW_MAX_SHADOW_X 40u

/* Rasterise an object into colour indices 0..3 and resolve its palette once.
 *
 * The shadow owns its pixels rather than pointing at a tile. The guest is free
 * to recycle tile 16 into something else the frame after it frees the slot, and
 * a carried sprite that keeps a borrowed tile reference becomes a different
 * sprite a few frames later - which is not a thing that announces itself. */
static void rasterise_object(const GBPresentationHardwareSnapshot *hw,
                             const GBPresentationSprite *sprite,
                             bool cgb,
                             ShadowCandidate *out) {
    const uint32_t bank = (cgb && (sprite->attributes & OAM_CGB_BANK) != 0u) ? 1u : 0u;
    const uint8_t palette = cgb ? (uint8_t)(sprite->attributes & OAM_CGB_PALETTE)
                                : (uint8_t)((sprite->attributes & OAM_OBJ_PALETTE) ? 1u : 0u);
    if (!cgb) {
        memcpy(out->colour, k_dmg_object_palette, sizeof(out->colour));
    } else {
        for (uint32_t i = 0; i < 4u; ++i) {
            const size_t offset = (size_t)palette * 8u + i * 2u;
            out->colour[i] = (uint16_t)((uint16_t)hw->object_palettes[offset] |
                                         ((uint16_t)hw->object_palettes[offset + 1u] << 8));
        }
    }
    out->behind_bg = (sprite->attributes & OAM_BG_PRIORITY) != 0u;
    uint32_t tile = sprite->tile;
    if (out->height == 16u) {
        tile &= 0xFEu;
    }
    for (uint32_t py = 0; py < out->height; ++py) {
        uint32_t row = py;
        if ((sprite->attributes & OAM_OBJ_FLIP_Y) != 0u) {
            row = out->height - 1u - row;
        }
        const uint32_t offset = tile * TILE_BYTES + (row & 7u) * 2u + ((row >> 3) & 1u) * TILE_BYTES;
        if (offset + 1u >= TILE_AREA) {
            continue;
        }
        const uint8_t *pattern = &hw->tile_data[bank][offset];
        for (uint32_t px = 0; px < out->width; ++px) {
            const uint32_t bit = ((sprite->attributes & OAM_OBJ_FLIP_X) != 0u)
                ? (out->width - 1u - px)
                : px;
            const uint8_t lo = (bit < 8u) ? pattern[0] : pattern[1];
            const uint32_t shift = (bit < 8u) ? (7u - bit) : (15u - bit);
            const uint8_t color_index = (uint8_t)(((lo >> shift) & 1u) |
                                                  (((pattern[1] >> shift) & 1u) << 1));
            out->index[py * out->width + px] = color_index;
        }
    }
}

/* Notice sprites that have just left the game's own 160 columns and hand a
 * copy to the shadow list.
 *
 * The guest frees the slot within a handful of frames - a traced soldier
 * survives 3 to 9 - while crossing the margin takes about 48, so by the time it
 * would be visible out there the slot is gone and the tile may be recycled. So
 * the copy is made here, at the last instant there is one, and it carries its
 * own pixels.
 *
 * Velocity comes from the trace rather than a constant: the recorded walk goes
 * 8, 7, 5, 4, 2, 1 on its last six frames, one to two pixels a frame.
 *
 * `capture_allowed` is the panel: a hand-over is only meaningful while the game
 * is playing a room. The bookkeeping runs either way - the slot's own picture
 * and the record of where it was - so that a slot's history is not stale when
 * play resumes; only the admission is skipped. */
static void notice_exiting_sprites(NativePresenter *presenter,
                                   const GBPresentationHardwareSnapshot *hw,
                                   bool cgb,
                                   bool capture_allowed) {
    /* First, keep a current copy of everything still wholly on screen. This is
     * the frame where the guest has no reason to be rewriting the sprite's
     * tiles. */
    for (uint32_t index = 0; index < GB_PRESENTATION_SPRITE_COUNT; ++index) {
        const GBPresentationSprite *sprite = &hw->sprites[index];
        if (sprite->raw_y == 0u) {
            continue;
        }
        const int32_t x = (int32_t)sprite->screen_x;
        if (x < 0 || x + 8 > (int32_t)SCREEN_WIDTH) {
            continue; /* clipped by the edge: not a whole picture yet */
        }
        ShadowCandidate *picture = &presenter->slot_picture[index];
        memset(picture, 0, sizeof(*picture));
        picture->screen_x = x;
        picture->screen_y = (int32_t)sprite->screen_y;
        picture->behind_bg = (sprite->attributes & OAM_BG_PRIORITY) != 0u;
        picture->height = (sprite->height != 0u) ? 16u : 8u;
        picture->width = 8u;
        if (picture->width > SPRITE_SHADOW_MAX_W || picture->height > SPRITE_SHADOW_MAX_H) {
            continue;
        }
        rasterise_object(hw, sprite, cgb, picture);

        presenter->slot_picture_valid |= (uint64_t)(1ull << index);
    }

    /* Then, hand over anything that has just left. */
    for (uint32_t index = 0; index < GB_PRESENTATION_SPRITE_COUNT; ++index) {
        const GBPresentationSprite *sprite = &hw->sprites[index];
        const int32_t x = (sprite->raw_y != 0u) ? (int32_t)sprite->screen_x : -1000;
        const int32_t previous = (index < presenter->previous_object_count)
            ? (int32_t)presenter->previous_object_x[index]
            : -1000;
        const int32_t previous_y = (index < presenter->previous_object_count)
            ? (int32_t)presenter->previous_object_y[index]
            : -1000;

        const bool was_inside = previous >= 0 && previous < (int32_t)SCREEN_WIDTH;
        /* A free slot is not a sprite leaving the view. Both make `now_inside`
         * false, and conflating them is what filled the shadow list: measured
         * on a 1000-frame outdoor run, 92 of 97 admissions were slots the guest
         * had simply cleared, carried at the sentinel position and thrown away
         * by the renderer - harmless on their own, but each one occupied a slot
         * a real walk-off could have used. A sprite that despawns inside the
         * game's own view has despawned; only a live slot that crosses the edge
         * is a hand-over. */
        const bool slot_live = sprite->raw_y != 0u;
        const bool now_inside = x >= 0 && x < (int32_t)SCREEN_WIDTH;

        if (slot_live && was_inside && !now_inside) {
            presenter->sprites_exiting++;
            if (x < previous) {
                presenter->sprites_exiting_left++;
            }
            if (!capture_allowed) {
                presenter->shadows_blocked_panel++;
            } else if ((presenter->slot_picture_valid & (uint64_t)(1ull << index)) == 0ull) {
                presenter->shadows_blocked_picture++;
            }
        }

        if (capture_allowed && slot_live && was_inside && !now_inside &&
            (presenter->slot_picture_valid & (uint64_t)(1ull << index)) != 0ull) {
            ShadowCandidate carried = presenter->slot_picture[index];
            carried.screen_x = x;
            /* The guest's last word on where it was: the y of the frame it
             * left on, not the y of the frame the picture was copied. */
            carried.screen_y = (int32_t)sprite->screen_y;
            uint32_t free_slot = 0u;
            while (free_slot < SPRITE_SHADOW_COUNT && presenter->shadows.items[free_slot].alive) {
                free_slot++;
            }
            if (sprite_shadow_admit(&presenter->shadows, &carried)) {
                presenter->shadows_admitted++;
                presenter->shadows_last_frame = presenter->lines_rendered / presenter->height;
                presenter->shadows_last_x = carried.screen_x;
                presenter->shadows_last_y = carried.screen_y;
                presenter->shadows_last_width = carried.width;
                /* Where it was going, as measured rather than assumed: the
                 * step it took on the frame it left. The old constant of two
                 * pixels a frame was right for a soldier walking level and
                 * wrong for anything else - a soldier climbing a slope changes
                 * his y as he goes, and the copy would walk off at the height
                 * where he was last seen. */
                if (free_slot < SPRITE_SHADOW_COUNT) {
                    SpriteShadow *shadow = &presenter->shadows.items[free_slot];
                    shadow->vx = (x - previous);
                    shadow->vy = (previous_y > -900) ? ((int32_t)sprite->screen_y - previous_y) : 0;
                }
            } else {
                presenter->shadows_refused++;
            }
            presenter->slot_picture_valid &= ~(uint64_t)(1ull << index);
        }

        if (index < SPRITE_SHADOW_MAX_SHADOW_X) {
            presenter->previous_object_x[index] = (int16_t)(x > 300 ? 300 : x);
            presenter->previous_object_y[index] =
                (int16_t)(slot_live ? sprite->screen_y : -1000);
        }
    }
    presenter->previous_object_count =
        (uint8_t)(GB_PRESENTATION_SPRITE_COUNT < SPRITE_SHADOW_MAX_SHADOW_X
                      ? GB_PRESENTATION_SPRITE_COUNT
                      : SPRITE_SHADOW_MAX_SHADOW_X);
}

/* Where the margin's pixels came from, over a run.
 *
 * Fed by the margin branch alone, because it is meant to describe the margin.
 * Letting the band's own lookups count here as well made the number unreadable:
 * the two uses look the same in the code and mean opposite things.
 *
 * Three sources, and they are not interchangeable:
 *
 *  - "replayed" is a column the PPU drew while it was inside the game's own
 *    view. It is the guest's own output and it is exact. It is also the only
 *    source there is for the side the camera has already passed, because the
 *    guest reuses the tile slots behind it: measured, the map's copy of a
 *    column the camera has left is 6-24 mean levels away from the picture, while
 *    the cache's copy is 0.
 *  - "map" is the tilemap, and it is the guest's *forward* copy of the room. It
 *    is what the leading side has, because nothing has drawn it yet: at frame
 *    10 the right margin replays nothing and the map there is 5 mean levels
 *    from the picture the PPU draws 160 pixels of travel later. Fix A banned the
 *    map from the margin on a measurement of the *trailing* margin - 83% of the
 *    left columns disagree - and read it as a property of the map rather than of
 *    the side. It is a property of the side.
 *  - "backdrop" is what is left: a flat picture (the game's own fade), or a
 *    margin the room rectangle says is not the room. */
/* Margin counters now live on each NativePresenter instance (presenter->margin_*),
 * so multiple instances or resets never leak stale totals through file globals. */

/* The camera's absolute position, in pixels, accumulated from the SCX/SCY
 * deltas the snapshot hands us.
 *
 * SCX and SCY are bytes of a tilemap that wraps at 256, so they cannot say
 * which pass the camera is on - one byte is a different world position every
 * 256 pixels. The step between two frames is small (a walk is one pixel, the
 * fastest scroll anywhere is five), so the short way round the byte *is* the
 * step, and the sum of those steps is an absolute coordinate. The guest's own
 * camera limits already tell us the short way round; a step past the jump
 * threshold is a room change, and the cache is cleared for those. */
#define CAMERA_ABS_JUMP_PX 127

static void camera_abs_advance(NativePresenter* presenter, uint8_t scx, uint8_t scy) {
    if (!presenter->camera_abs_valid) {
        presenter->camera_abs_x = (int32_t)scx;
        presenter->camera_abs_y = (int32_t)scy;
        presenter->camera_abs_valid = true;
    } else {
        const int32_t dx = (int32_t)(int8_t)(uint8_t)(scx - (uint8_t)presenter->camera_prev_scx);
        const int32_t dy = (int32_t)(int8_t)(uint8_t)(scy - (uint8_t)presenter->camera_prev_scy);
        if (dx > -CAMERA_ABS_JUMP_PX && dx < CAMERA_ABS_JUMP_PX) {
            presenter->camera_abs_x += dx;
            /* Remembered rather than re-derived: the margin needs the direction
             * on the frames the camera is still, which is exactly when it is not
             * in the delta. A room change jumps past the threshold and leaves
             * the direction alone, which is right - the camera resumes the way
             * it was going. */
            if (dx > 0) {
                presenter->camera_dir_x = 1;
            } else if (dx < 0) {
                presenter->camera_dir_x = -1;
            }
        }
        if (dy > -CAMERA_ABS_JUMP_PX && dy < CAMERA_ABS_JUMP_PX) {
            presenter->camera_abs_y += dy;
        }
    }
    presenter->camera_prev_scx = (int32_t)scx;
    presenter->camera_prev_scy = (int32_t)scy;
}

static uint16_t world_page(int32_t world_x, int32_t world_y) {
    const uint32_t page_x = ((uint32_t)world_x >> 8) & 0xFFu;
    const uint32_t page_y = ((uint32_t)world_y >> 8) & 0xFFu;
    return (uint16_t)(page_x | (page_y << 8));
}

/* The page the pixel at `surface_x`, row `y` belongs to.
 *
 * `surface_x + camera_x` is the game column modulo 256, which is what the cache
 * key wants, but the absolute world coordinate wants the *signed* game column:
 * a margin column left of the picture is -36, not 220. The picture's left edge
 * is where the sum wraps, so it is recovered from camera_x, which is the one
 * number the caller already has. */
static uint16_t surface_page(const NativePresenter *presenter,
                             uint32_t surface_x,
                             uint32_t y,
                             uint32_t camera_x) {
    const int32_t game_left = (int32_t)((0u - camera_x) & 0xFFu);
    const int32_t world_x = presenter->camera_abs_x + (int32_t)surface_x - game_left;
    const int32_t world_y = presenter->camera_abs_y + (int32_t)y;
    return world_page(world_x, world_y);
}

/* The world tile a tilemap *byte* was written for, taken from where the camera
 * is now.
 *
 * A byte of the tilemap is one tile, and the guest writes it for the world tile
 * it is about to be scrolled into, so the byte's own coordinate modulo 256 plus
 * the camera's absolute position is that world tile. A page (256 pixels) is not
 * a fine enough stamp on its own - the guest rewrites a byte once per pass
 * through the byte space, so within one page a byte holds whichever column last
 * passed it, and a stamp of "same page" accepts a column a hundred pixels away.
 * The absolute coordinate does not: two world tiles 256 apart share a byte and
 * differ in this number. */
static void map_byte_world(const NativePresenter* presenter,
                           const GBPresentationHardwareSnapshot* hw,
                           uint32_t byte_index,
                           int32_t* out_world_x,
                           int32_t* out_world_y) {
    const uint32_t tile_x = (byte_index % MAP_DIM) * 8u;
    const uint32_t tile_y = (byte_index / MAP_DIM) * 8u;
    const uint32_t game_x = (tile_x - (uint32_t)hw->scx) & 0xFFu;
    const uint32_t game_y = (tile_y - (uint32_t)hw->scy) & 0xFFu;
    *out_world_x = presenter->camera_abs_x + (int32_t)game_x;
    *out_world_y = presenter->camera_abs_y + (int32_t)game_y;
}

/* Nowhere in the guest is there a "this tile is current" bit, so it is derived
 * from the only thing that says so: the byte changed.
 *
 * The guest streams the room into the tilemap as the camera travels, and it
 * reuses the byte space on every pass through it - a room wider than 256 pixels
 * has to. So a byte holds the guest's copy of *one* world tile, the last one it
 * wrote there, and the ones it has not reached hold whatever the previous piece
 * left. Diffing against a shadow of the map says which is which without a write
 * hook: a byte that has not moved since the room before this one was on screen
 * was never written for this room.
 *
 * The value being right for the column is what makes this sound rather than a
 * heuristic. A byte the guest wrote is the tile for the world tile the write was
 * for, and that is recorded beside it; a byte it wrote and then wrote the same
 * value again is still that column's tile. What this cannot see - a byte whose
 * content happens to be identical across two different pieces of the room and
 * which was therefore never observed to change - costs a column of margin, not a
 * wrong one. */
static void map_write_observe(NativePresenter* presenter,
                              const GBPresentationHardwareSnapshot* hw) {
    if (!presenter->map_shadow_seeded) {
        /* The first frame is not a change.
         *
         * The shadow starts empty, so diffing the map against it on frame one
         * marks every non-zero byte as the guest's write - stamped with the
         * camera position of frame one, which is not where any of those bytes was
         * written. The map reads as "written for a world tile at the camera",
         * and a byte whose margin column was streamed before the surface came up
         * reads as written for a different world tile and is refused.
         *
         * Nothing has been observed to change yet, so nothing is stamped. What
         * the bytes hold is the guest's own streaming as it stood when the
         * surface came up, and `map_column_untouched` is the rule that may read
         * it. */
        for (uint32_t mi = 0; mi < 2u; ++mi) {
            memcpy(presenter->map_shadow[mi], hw->tile_ids[mi], GB_PRESENTATION_TILE_MAP_BYTES);
        }
        presenter->map_shadow_seeded = true;
        return;
    }
    for (uint32_t mi = 0; mi < 2u; ++mi) {
        const uint8_t* ids = hw->tile_ids[mi];
        for (uint32_t k = 0; k < GB_PRESENTATION_TILE_MAP_BYTES; ++k) {
            if (ids[k] == presenter->map_shadow[mi][k]) {
                continue;
            }
            presenter->map_shadow[mi][k] = ids[k];
            presenter->map_written[mi][k] = 1u;
            map_byte_world(presenter, hw, k, &presenter->map_written_x[mi][k],
                           &presenter->map_written_y[mi][k]);
        }
    }
}

/* May the map answer for this pixel?
 *
 * Only if the byte behind it was written for exactly this world tile. That is
 * the whole test, and it is exact rather than a heuristic: a byte written for a
 * different world tile is a byte that still describes the piece of the room the
 * camera was in earlier, and reading it is how a static room's margin came back
 * as the yellow floor from a corridor back. */
static bool map_column_usable(const NativePresenter* presenter,
                              const GBPresentationHardwareSnapshot* hw,
                              uint32_t surface_x,
                              uint32_t y,
                              uint32_t camera_x) {
    const uint32_t map_index = (hw->lcdc & LCDC_BG_MAP) != 0u ? 1u : 0u;
    const uint32_t world_x = (surface_x + camera_x + (uint32_t)hw->scx) & 0xFFu;
    const uint32_t world_y = (y + (uint32_t)hw->scy) & 0xFFu;
    const uint32_t k = (world_y >> 3) * MAP_DIM + (world_x >> 3);
    if (k >= GB_PRESENTATION_TILE_MAP_BYTES || presenter->map_written[map_index][k] == 0u) {
        return false;
    }
    /* The tile the write was for, and the tile that is wanted: they agree only
     * when the byte really is this piece of the room. The row is part of it for
     * the same reason it is part of the cache key - a tile row of the same tile
     * column is a different byte and can come from a different pass.
     *
     * Both sides are taken at tile granularity, because that is what a tilemap
     * byte is. The stamp is the byte's *tile*, so it is a multiple of eight; the
     * request is a pixel inside a tile, so it is floored to one. Comparing the
     * pixel row against the tile row was a comparison that could only hold on
     * every eighth row - one row in eight of a byte that is exactly right was
     * answered, and the other seven were refused as another piece of the room.
     * The x side always had that tolerance (the request is a pixel, the stamp a
     * tile edge), which is why the column test looked healthy while the row test
     * was throwing the margin away. */
    const int32_t want_x =
        presenter->camera_abs_x + (int32_t)surface_x - (int32_t)((0u - camera_x) & 0xFFu);
    /* Floored to the tile, in unsigned arithmetic: the floor has to be the
     * arithmetic one for a camera that has also gone left of zero, which is
     * where a room's left end puts it. */
    const int32_t want_y = (int32_t)((uint32_t)(presenter->camera_abs_y + (int32_t)y) & ~7u);
    if (presenter->map_written_y[map_index][k] != want_y) {
        return false;
    }
    /* The tile grid is eight pixels wide and the write is stamped with the
     * camera the frame it was seen, which is at most one frame of travel away
     * from where the column sat when the guest actually wrote it. One frame is
     * under eight pixels everywhere in the game - the fastest scroll measured is
     * five - so the stamp and the request can only disagree within the tile that
     * straddles them, and a tile of tolerance is what that costs. */
    const int32_t dx = presenter->map_written_x[map_index][k] - want_x;
    return dx <= 0 && dx > -8;
}

/* May the map answer for a pixel whose byte the guest has not written since this
 * run began watching?
 *
 * `map_column_usable` refuses every byte it never saw change, and on the margin
 * that refusal is a black column: the cache has nothing for a column the camera
 * has not crossed, and this is the third source.
 *
 * The byte is a leftover, but a leftover from *here* whenever the run began
 * inside this room: the first frames of a session, and of a room entered from a
 * save state. That is not a corner - it is where every session starts - and the
 * measurement is against the picture the PPU draws for exactly those world
 * columns, 36 pixels of travel later: the untouched byte answers 58.9-72.1% of
 * the trailing margin exactly and is 17.6-32.1 mean levels off over the rest,
 * where refusing it is 0.0% and 111.8-128.8 levels. Refusing it is a black
 * column where the room is.
 *
 * What keeps this from being a licence to read anything: a byte the guest *was*
 * seen to write still has to pass the stamp test above. The reused tile slot
 * behind the camera - the corridor that test's comment warns about - is a byte
 * the guest wrote, so it is still refused, and this does not reach it. What
 * this reaches is the byte whose content never had to change: the guest rewrites
 * the same tile into the same byte on every pass, so a byte streaming the same
 * tile again is never observed to change and carries no stamp at all.
 *
 * The residual is the room before this one. An untouched byte carries no
 * generation, so once the player has changed rooms it can still be the room they
 * left. That is bounded to columns nothing has drawn - the cache and the stamp
 * test both come first - and it is the same residual the leading margin has
 * carried since fix B, which reads the map with no stamp test at all. */
static bool map_column_untouched(const NativePresenter* presenter,
                                 const GBPresentationHardwareSnapshot* hw,
                                 uint32_t surface_x,
                                 uint32_t y,
                                 uint32_t camera_x) {
    const uint32_t map_index = (hw->lcdc & LCDC_BG_MAP) != 0u ? 1u : 0u;
    const uint32_t world_x = (surface_x + camera_x + (uint32_t)hw->scx) & 0xFFu;
    const uint32_t world_y = (y + (uint32_t)hw->scy) & 0xFFu;
    const uint32_t k = (world_y >> 3) * MAP_DIM + (world_x >> 3);
    return k < GB_PRESENTATION_TILE_MAP_BYTES && presenter->map_written[map_index][k] == 0u;
}

/* Is this margin column on the side the camera is heading toward?
 *
 * The guest streams the room into the tilemap *ahead* of the camera and reuses
 * the tile slots *behind* it. The two sides therefore have different honest
 * answers and the difference is the whole of fix A: the tilemap is the guest's
 * own forward copy of the room on the side the camera has not drawn yet, and on
 * the side it has left it holds whatever the next pass over the same byte space
 * has already written - measured against the picture the PPU drew, the cache is
 * 0 there and the map is 6-24 mean levels off.
 *
 * `map_column_usable` cannot see the leading side on its own. It only knows a
 * byte changed, and the guest rewrites a byte for the same world tile on every
 * pass whether or not the value differs; a byte that streams the same tile
 * again never changes and is never re-stamped, so the test reads its stamp as
 * one full page (256 px) old and refuses it. On the leading margin that refusal
 * is wrong: the byte is the guest's forward copy of the room.
 *
 * Measured against the picture the PPU draws for the same world columns, the
 * rightward leading margin from the map is 80-85% exact (90-93% at the one
 * pixel the camera decelerates by), where the stamp test was turning ~96% of
 * it black. The leftward leading margin is filled from the same map and is no
 * longer black, but the guest writes a shorter window on that side: its inner
 * 16 px reach 82-90% and the outer 20 px less. That residual is ADR-007's, and
 * it is a limit of what the tilemap holds - not of this test.
 *
 * Which side is leading is the direction of travel, not the side of the
 * picture: walking left makes the left margin the leading one. The direction is
 * the last one the camera moved in, so a camera standing still keeps the
 * margin it had. Before it has moved at all there is no leading side. */
static bool margin_is_leading(const NativePresenter* presenter,
                              uint32_t surface_x,
                              uint32_t game_left,
                              uint32_t picture_width) {
    if (presenter->camera_dir_x > 0) {
        return surface_x >= game_left + picture_width;
    }
    if (presenter->camera_dir_x < 0) {
        return surface_x < game_left;
    }
    return false;
}

/* One column of the world out of the tilemap, through the cache when the cache
 * can answer for it.
 *
 * This is the band's source, and the margin's source in `--no-column-cache`,
 * which is the comparison build: it reads the map exactly as the margin used to,
 * so that a dump of it can say what the map would have drawn. A column the camera
 * has never reached has no honest answer here - the guest writes a map column
 * only when it scrolls into view, so the map still holds the room before this one
 * in it.
 *
 * The cache is a scene cache, and a row the guest drew from the other map is
 * its HUD band, not scene: the cache is not consulted for those rows at all -
 * what it holds there is the band the PPU drew - and the room comes from the
 * map instead. */
static uint16_t column_sample(NativePresenter *presenter,
                              const GBPresentationHardwareSnapshot *hw,
                              uint32_t surface_x,
                              uint32_t y,
                              uint32_t camera_x,
                              bool cgb,
                              uint32_t scene_map,
                              uint8_t *out_ci,
                              uint8_t *out_prio) {
    if (presenter->column_cache_enabled && presenter->column_cache != NULL &&
        row_is_scene_map(hw, y, scene_map)) {
        const uint32_t world_x = (surface_x + camera_x + (uint32_t)hw->scx) & 0xFFu;
        const uint32_t world_y = (y + (uint32_t)hw->scy) & 0xFFu;
        const size_t key = (size_t)world_y * 256u + world_x;
        if (presenter->column_seen[key] != 0u &&
            presenter->column_page[key] == surface_page(presenter, surface_x, y, camera_x)) {
            presenter->cache_hits++;
            *out_ci = 0u;
            *out_prio = 0u;
            return presenter->column_cache[key];
        }
        presenter->cache_misses++;
    }
    return sample_background(hw, surface_x, y, camera_x, cgb, out_ci, out_prio);
}

/* Replay a column the PPU has already drawn, and say whether there is one.
 *
 * This is the exact source, and the only one for the side the camera has
 * passed. Reading the map there returns whatever the guest has since written
 * into that tile slot - it reuses the ones behind the camera - and measured
 * against the pixels the PPU drew it is 6-24 mean levels off on the left
 * margin, where the cache is 0.
 *
 * The map is not, however, wrong everywhere, which is what fix A got wrong: it
 * is wrong *behind* the camera and right *in front of* it. See the call site.
 *
 * The page test is what keeps the replay honest. A byte of the tilemap is the
 * world position modulo 256, so a byte the PPU drew for one piece of the room
 * is a byte the guest can be holding a different piece of a moment later, and
 * the leading margin and the band ask for exactly those bytes - the ones whose
 * only copy is one corridor back. Requiring the page makes them miss and take
 * the map, which is the guest's own answer for where the camera has not been. */
static bool column_replay(NativePresenter *presenter,
                          const GBPresentationHardwareSnapshot *hw,
                          uint32_t surface_x,
                          uint32_t y,
                          uint32_t camera_x,
                          uint8_t *out_ci,
                          uint8_t *out_prio,
                          uint16_t *out_colour) {
    if (!presenter->column_cache_enabled || presenter->column_cache == NULL) {
        return false;
    }
    const uint32_t world_x = (surface_x + camera_x + (uint32_t)hw->scx) & 0xFFu;
    const uint32_t world_y = (y + (uint32_t)hw->scy) & 0xFFu;
    const size_t key = (size_t)world_y * 256u + world_x;
    if (presenter->column_seen[key] == 0u ||
        presenter->column_page[key] != surface_page(presenter, surface_x, y, camera_x)) {
        presenter->cache_misses++;
        return false;
    }
    presenter->cache_hits++;
    *out_ci = 0u;
    *out_prio = 0u;
    *out_colour = presenter->column_cache[key];
    return true;
}

static void column_remember(NativePresenter *presenter,
                            const GBPresentationHardwareSnapshot *hw,
                            uint32_t game_x,
                            uint32_t y,
                            uint16_t colour) {
    if (!presenter->column_cache_enabled || presenter->column_cache == NULL) {
        return;
    }
    const uint32_t world_x = (game_x + (uint32_t)hw->scx) & 0xFFu;
    const uint32_t world_y = (y + (uint32_t)hw->scy) & 0xFFu;
    const size_t key = (size_t)world_y * 256u + world_x;
    presenter->column_cache[key] = colour;
    presenter->column_seen[key] = 1u;
    /* Absolute x is the camera's plus the column's offset into the picture; the
     * offset is the whole difference, so the byte is subtracted back off. The row
     * is part of the y page for the same reason the column is part of the x one:
     * rows 200 apart are in different pages and the test is per row. */
    presenter->column_page[key] =
        world_page(presenter->camera_abs_x + (int32_t)game_x, presenter->camera_abs_y + (int32_t)y);
}

void native_presenter_clear_column_cache(NativePresenter *presenter) {
    if (presenter == NULL || presenter->column_seen == NULL) {
        return;
    }
    memset(presenter->column_seen, 0, 256u * 256u);
    presenter->cache_hits = 0u;
    presenter->cache_misses = 0u;
}

void native_presenter_enable_column_cache(NativePresenter *presenter, bool enabled) {
    if (presenter == NULL) {
        return;
    }
    presenter->column_cache_enabled = enabled;
    if (!enabled) {
        free(presenter->column_cache);
        free(presenter->column_seen);
        free(presenter->column_page);
        presenter->column_cache = NULL;
        presenter->column_seen = NULL;
        presenter->column_page = NULL;
        presenter->cache_hits = 0u;
        presenter->cache_misses = 0u;
    } else if (presenter->column_cache == NULL) {
        presenter->column_cache = calloc(256u * 256u, sizeof(uint16_t));
        presenter->column_seen = calloc(256u * 256u, sizeof(uint8_t));
        presenter->column_page = calloc(256u * 256u, sizeof(uint16_t));
        if (presenter->column_cache == NULL || presenter->column_seen == NULL ||
            presenter->column_page == NULL) {
            free(presenter->column_cache);
            free(presenter->column_seen);
            free(presenter->column_page);
            presenter->column_cache = NULL;
            presenter->column_seen = NULL;
            presenter->column_page = NULL;
            presenter->column_cache_enabled = false;
        }
    }
}

void native_presenter_set_dump_tiles_dir(NativePresenter *presenter, const char *dir) {
    if (presenter == NULL) {
        return;
    }
    presenter->dump_tiles_dir = dir;
    presenter->last_dump_area = 0xFF;
    presenter->last_dump_room = 0xFF;
}

/* Defined with the object layer, below: the composition calls it as its last
 * step, once it knows where the picture and the status strip sit. */
static void render_margin_sprites(
    NativePresenter* presenter,
    const GBPresentationHardwareSnapshot* hw,
    const bool cgb,
    const uint32_t game_left,
    const uint32_t picture_width,
    const uint32_t hud_left,
    const uint32_t scene_map);

/* Is the picture one colour?
 *
 * A room change fades the screen to a single colour while the tilemap still
 * holds the room on both sides of the fade - the guest loaded it before the
 * fade. Reconstruction from the map then paints margins the game is not
 * showing: the picture goes black, the margins keep the room, and the screen
 * reads as broken. The game's own answer is its own picture, so when the
 * background is one colour the margin is that colour too.
 *
 * Only the background columns count - the panel is the window, not the room -
 * and only rows above 132, because the status read-out is drawn in the
 * background at the bottom of the screen and is not the room either. Measured
 * on the walk that found this: real frames are 46-67% one colour over that
 * region, the fade is 100%. */
static bool flat_picture_colour(const GBPresentationHardwareSnapshot* hw,
                                uint32_t columns,
                                uint16_t* out_colour) {
    if (columns == 0u || columns > (uint32_t)SCREEN_WIDTH) {
        return false;
    }
    const uint16_t corner = hw->accurate_pixels[0];
    const uint16_t middle =
        hw->accurate_pixels[(size_t)64u * SCREEN_WIDTH + columns / 2u];
    uint32_t corner_count = 0u;
    uint32_t middle_count = 0u;
    uint32_t total = 0u;
    for (uint32_t y = 0; y < 132u; ++y) {
        for (uint32_t x = 0; x < columns; ++x) {
            const uint16_t pixel = hw->accurate_pixels[(size_t)y * SCREEN_WIDTH + x];
            ++total;
            if (pixel == corner) {
                ++corner_count;
            }
            if (pixel == middle) {
                ++middle_count;
            }
        }
    }
    /* Both candidates agreeing means the region is one colour; the threshold
     * over the larger of the two keeps a two-tone screen from passing. */
    const uint32_t best = corner_count > middle_count ? corner_count : middle_count;
    if ((uint64_t)best * 100u < (uint64_t)total * 95u) {
        return false;
    }
    *out_colour = (corner_count >= middle_count) ? corner : middle;
    return true;
}

/* The margin past the picture, resolved the way it was before the room map
 * existed: the column cache for what the PPU already drew, the guest's map
 * for what it has not, and the backdrop when neither can answer. Used only
 * where the room's own grid has no cell - the layer ends there, or the room
 * is shorter than a screen - so the room's data still wins wherever it
 * exists and the guest's own copy covers the seam. */
static void resolve_margin_from_guest(NativePresenter* presenter,
                                      const GBPresentationHardwareSnapshot* hw,
                                      uint32_t x,
                                      uint32_t y,
                                      uint32_t camera_x,
                                      uint32_t game_left,
                                      uint32_t picture_width,
                                      uint16_t backdrop,
                                      bool cgb,
                                      uint32_t scene_map,
                                      uint32_t* row,
                                      uint8_t* ci,
                                      uint8_t* prio) {
    uint16_t replayed = 0u;
    if (presenter->column_cache_enabled) {
        /* A row the guest assembled from the other map is its HUD
         * band, so the cache is not asked for it: what the cache
         * holds for those world pixels is the band the PPU drew
         * there, and replaying it is HUD leaking sideways into the
         * margin. The room comes from the map instead - the same
         * map every other row's margin comes from - so both sides
         * of the band's rows show the room continuously. */
        if (row_is_scene_map(hw, y, scene_map) &&
            column_replay(presenter, hw, x, y, camera_x, ci, prio, &replayed)) {
            row[x] = replayed;
            presenter->margin_replayed++;
            presenter->bg_pixels++;
        } else if (row_is_scene_map(hw, y, scene_map) &&
                   (map_column_usable(presenter, hw, x, y, camera_x) ||
                    margin_is_leading(presenter, x, game_left, picture_width))) {
            row[x] = sample_background(hw, x, y, camera_x, cgb, ci, prio);
            presenter->margin_from_map++;
            presenter->margin_map_fresh++;
            if (x < game_left) {
                presenter->margin_map_fresh_left++;
            }
            presenter->bg_pixels++;
        } else if (row_is_scene_map(hw, y, scene_map) &&
                   map_column_untouched(presenter, hw, x, y, camera_x)) {
            /* The byte is a leftover, and here it is a leftover from
             * *this* room: nothing has written it since the surface
             * came up, so what it holds is what the guest had
             * streamed there before the session started - which is
             * the room the camera is in, one screen's start at a
             * time. See map_column_untouched for the measurement:
             * where the cache has nothing and the stamp test refuses,
             * this is 59-72% of the picture the PPU itself draws for
             * those columns 36 pixels of travel later, and refusing
             * it is a black column. */
            row[x] = sample_background(hw, x, y, camera_x, cgb, ci, prio);
            presenter->margin_from_map++;
            presenter->margin_map_untouched++;
            if (x < game_left) {
                presenter->margin_map_untouched_left++;
            }
            presenter->bg_pixels++;
        } else {
            /* What is left: a byte the guest *did* write, but for
             * another piece of the room. The guest reuses the tile
             * slots behind the camera, so the byte the map would
             * answer from describes a corridor the player is not in,
             * and the surface says so rather than painting it. */
            row[x] = backdrop;
            /* Opaque: the bar is not something to stand on. */
            *ci = 1u;
            *prio = 1u;
            presenter->margin_map_stale++;
            if (x < game_left) {
                presenter->margin_map_stale_left++;
            }
            presenter->margin_backdrop++;
            presenter->outside_pixels++;
        }
    } else {
        /* --no-column-cache: the map, which is what this replaces.
         * Kept because it is the only way to see what the map would
         * have said, and the audit measures the two builds against
         * each other. */
        row[x] = column_sample(presenter, hw, x, y, camera_x, cgb, scene_map, ci, prio);
        if (map_column_usable(presenter, hw, x, y, camera_x)) {
            presenter->margin_map_fresh++;
        } else {
            presenter->margin_map_stale++;
        }
        presenter->bg_pixels++;
        }
}
/* Mark one sprite box's horizontal footprint on scanline `y` so background
 * column caching skips pixels that may have been covered by OAM. */
static void mask_sprite_row(
    uint8_t row_has_sprite[SCREEN_WIDTH], uint32_t y, int32_t x, int32_t top, int32_t height) {
    if (height <= 0 || (int32_t)y < top || (int32_t)y >= top + height) {
        return;
    }
    int32_t sx0 = x;
    int32_t sx1 = x + 8;
    if (sx0 < 0) {
        sx0 = 0;
    }
    if (sx1 > (int32_t)SCREEN_WIDTH) {
        sx1 = (int32_t)SCREEN_WIDTH;
    }
    for (int32_t sx = sx0; sx < sx1; ++sx) {
        row_has_sprite[sx] = 1u;
    }
}

/* Save this frame's OAM boxes so the next frame's column cache also excludes
 * sprites drawn before VBlank OAM DMA ran during drain_after_frame. */
static void remember_frame_sprites(
    NativePresenter* presenter, const GBPresentationHardwareSnapshot* hw) {
    for (uint32_t s = 0u; s < GB_PRESENTATION_SPRITE_COUNT; ++s) {
        const GBPresentationSprite* sprite = &hw->sprites[s];
        if (sprite->raw_y == 0u) {
            presenter->prev_sprite_h[s] = 0u;
        } else {
            presenter->prev_sprite_x[s] = (int16_t)sprite->screen_x;
            presenter->prev_sprite_y[s] = (int16_t)sprite->screen_y;
            presenter->prev_sprite_h[s] = (sprite->height != 0u) ? 16u : 8u;
        }
    }
}

/* Upper-half secondary-map boxes (e.g. Mei Ling radar tutorial) and tall
 * bottom pickup/speech boxes (rows above the 8px bottom LIFE strip when >16
 * non-scene bottom rows are active) centre inside the playfield, while the
 * bottom 8px LIFE/status strip always stays anchored at the left edge. */
static bool row_is_centered_overlay(uint32_t y, uint32_t height, uint32_t bottom_non_scene_rows) {
    return y < height / 2u || (bottom_non_scene_rows > 16u && y + 8u < height);
}

bool native_presenter_render_background(
    NativePresenter* presenter,
    const GBPresentationFrame* frame,
    const uint32_t margin_left,
    const uint32_t margin_right,
    const bool cgb) {
    if (presenter == NULL || frame == NULL || presenter->pixels == NULL) {
        return false;
    }
    if (presenter->width < SCREEN_WIDTH || presenter->height < NATIVE_HEIGHT) {
        return false;
    }

    const GBPresentationHardwareSnapshot* hw = &frame->hardware;
    /* Which map the scene was drawn from, before any pixel asks. */
    const uint32_t scene_map = frame_scene_map(hw);
    /* Before any pixel: where the camera is, absolutely. The cache needs it to
     * tell one pass through the tilemap from another. */
    camera_abs_advance(presenter, hw->scx, hw->scy);
    /* And which of the tilemap's bytes the guest has actually written, before
     * anything reads the map through the margin. */
    map_write_observe(presenter, hw);

    /* Tileset dump for offline colour proof: dump the live VRAM tileset when
     * the room changes. */
    if (presenter->dump_tiles_dir && presenter->room.camera_valid && presenter->room.map.valid) {
        uint8_t area = presenter->room.map.area;
        uint8_t room = presenter->room.map.room;
        if (area != presenter->last_dump_area || room != presenter->last_dump_room) {
            char path[512];
            snprintf(path, sizeof(path), "%s/area_%02u_room_%02u_tiles.bin", presenter->dump_tiles_dir, area, room);
            FILE *f = fopen(path, "wb");
            if (f) {
                /* Dump bank 1 patterns: the area's tile stream is decompressed there
                 * on room load and stays resident while the room is active. */
                fwrite(hw->tile_data[1], 1, 0x1800, f);
                fclose(f);
                presenter->last_dump_area = area;
                presenter->last_dump_room = room;
            }
        }
    }

    /* The composition, and it is driven by the hardware rather than by a scene
     * detector.
     *
     * The window layer is the game's own, so the panel is drawn when the game
     * draws it: gated on LCDC bit 5, which the intro and the radio leave off and
     * gameplay turns on. That gives "HUD only while playing" for free.
     *
     * The HUD owns a strip at the right edge that revealed scene never enters,
     * so the panel never sits on top of the widescreen. The picture is centred
     * in whatever is left: 256 columns when the panel is absent, 232 when it is
     * present. So the game view sits at 48..207 during the opening and at
     * 36..195 during play, and the revealed scene grows against whichever edge
     * the room actually has. It leaves that centre only when one side has no
     * room left to fill its band - camera_picture_left, ADR-011 - so mid-room
     * framing is the one described here.
     *
     * Centring the picture rather than hanging it off the left margin is what
     * the opening needed: with nothing learned, an adjacency rule put the game
     * at column 0 with 96 columns of black on one side only. */
    /* A strip is only reserved when the window is a status panel and there is
     * room for one. Two ways it is not, and both happened:
     *
     * On the 160px surface the game view already fills the width, so
     * subtracting the panel left less than the picture needed - an unsigned
     * wrap put the view at a column in the billions and cost 1284 pixels of the
     * boot. The band report named the rows before anything else did.
     *
     * WX is 0 or 7 for stretches of the boot, which is the game covering the
     * whole screen with the window, not drawing a panel. Measured: frame 90 has
     * WX=0, frames 120-270 have WX=7. Measuring the panel as
     * `160 - (WX - 7)` makes that 160 columns wide - the entire picture - and
     * subtracting it left 96, so the view wrapped out of the surface. The
     * Konami logo landed against the right edge with its border clipped and
     * "KONAMI COMPUTER ENTERTAINMENT JAPAN PA" lost its last letters, which is
     * what a frame drawn mostly off-screen looks like.
     *
     * So the panel has to be narrower than the picture, and what is left after
     * it has to still hold the picture. */
    uint32_t hud_source = 0u;
    uint32_t hud_width = 0u;
    if (presenter->width > (uint32_t)SCREEN_WIDTH &&
        (hw->lcdc & LCDC_WINDOW_ENABLE) != 0u) {
        const uint32_t source = (hw->wx >= 7u) ? (uint32_t)hw->wx - 7u : 0u;
        const uint32_t candidate = (uint32_t)SCREEN_WIDTH - source;
        if (candidate > 0u && candidate < (uint32_t)SCREEN_WIDTH &&
            presenter->width - candidate >= (uint32_t)SCREEN_WIDTH) {
            hud_source = source;
            hud_width = candidate;
        }
    }
    /* Flush to the right edge of the surface. Passing this in as a number
     * looked harmless and was not: `width - (SCREEN_WIDTH - 24)` is 120, not
     * 232, so the strip landed inside the picture, the game-view branch won,
     * and the status read-out disappeared from the game entirely. The strip's
     * width is not known here - it depends on WX - so its position cannot be
     * known here either. */
    const uint32_t hud_left = presenter->width - hud_width;
    const uint32_t play_width = presenter->width - hud_width;
    /* The room's own columns past the picture are only a thing while the game
     * is playing one.
     *
     * The panel is the hardware's own signal for that, and it is the same one
     * that puts the strip on the edge: on through gameplay, off through the
     * opening and the radio. With it off the guest is not in gameplay, and the
     * camera limits still in WRAM are the ones the last room left behind - so
     * the extent handed in here is stale by construction, not by accident.
     * Extending into it drew that room's tiles beside a picture that is not
     * that room, which is the codec's garbage: the limits read 504/256/432/632
     * (room 1:1) straight through the cutscene.
     *
     * With no room column there is nothing to replay and nothing to sample, so
     * the picture letterboxes into the backdrop instead. */
    const bool panel = (hud_width != 0u);
    /* Active playfield width inside the PPU view: during gameplay, columns
     * `hud_source..159` hold the HUD window, so only `hud_source` (136) columns
     * of scene sit in the PPU frame. Framing against 136 lets the picture slide
     * all 96 px to the right edge so the HUD never leaves a 24px gap. */
    const uint32_t picture_width = panel ? hud_source : (uint32_t)SCREEN_WIDTH;
    const uint32_t game_left = camera_picture_left_for(
        play_width, picture_width, panel ? margin_left : 0u, panel ? margin_right : 0u);
    const uint32_t band_left = game_left;
    const uint32_t band_right = play_width - game_left - picture_width;
    const uint32_t neighbour_right =
        (play_width > game_left + (uint32_t)SCREEN_WIDTH)
            ? (play_width - game_left - (uint32_t)SCREEN_WIDTH)
            : 0u;
    /* The room's travel is clamped into the band, not tested against it.
     *
     * Each band sits against the picture - 36 columns with the panel, 48
     * without, and up to 72 on one side once the picture has slid - and the
     * caller hands in travel that can exceed it, so a room with more pixels of
     * camera range than the band is wide has more travel than the band. This
     * used to be a test, `margin_left < game_left`, and it read "the travel
     * does not fit, so there is no room here" - which is the one conclusion
     * that is never true. Measured on the save state with $FFB0 = 505 against
     * room 2:4's own 256/616: 249 pixels of travel on the left and 111 on the
     * right, and both sides came out black at every column of every frame, the
     * picture letterboxed into the backdrop while the room continued behind it.
     * The same state dumped in room 1:1 did the same with 248 pixels of travel.
     *
     * What the travel still decides is how much of the band is the room: once
     * the camera reaches the limit the room's own world really does end there.
     * The transition table may still name an annexe on that side, and the
     * neighbour block below then widens the scene; with no such transition the
     * remainder is the backdrop, because the alternative is inventing a room
     * edge the guest never had. */
    uint32_t room_left = panel ? (margin_left < band_left ? margin_left : band_left) : 0u;
    uint32_t room_right = panel ? (margin_right < band_right ? margin_right : band_right) : 0u;
    const uint32_t own_room_left = room_left;
    const uint32_t own_room_right = room_right;
    /* How much of the band the room itself occupies is the guest's own answer,
     * handed in as margin_left and margin_right; this file does not have to
     * guess. The band can still be wider than the room: at the camera's limit
     * margin_left is zero while the annexe on that side is not. The transition
     * table owns that answer, and the neighbour block below widens the scene to
     * the surface margin only when it has actually placed one. */
    const uint32_t camera_x = (0u - game_left) & 0xFFu;
    /* What the room does not continue into, and the one case where neither the
     * cache nor the map may be asked: a picture the game has faded to a single
     * colour. See flat_picture_colour.
     *
     * The other use is a still the game is presenting rather than a room the
     * player is in. The panel being absent is the hardware's own way of saying
     * so: it is on through gameplay and off through the opening and the radio,
     * which is the same signal that puts the panel on the edge. */
    const uint32_t room_columns = panel ? hud_source : (uint32_t)SCREEN_WIDTH;
    uint16_t flat_colour = 0u;
    const bool flat = flat_picture_colour(hw, room_columns, &flat_colour);
    /* The colour the picture letterboxes into, and there are three cases, not
     * two.
     *
     * A flat picture is the game itself fading a still to one colour, so the
     * margin is that colour - and this is asked first, because it is the only
     * source that knows the game is presenting rather than playing.
     *
     * While the game is playing, past the room is black: the bar a 160px
     * picture letterboxes into.
     *
     * And with the panel absent but a room still loaded - the codec, the radio,
     * a cutscene inside a room - the answer is black too. Not the room's
     * colour: the guest is showing a still that the room's own columns do not
     * describe, and the columns beside it have no content at all. Measured, the
     * cutscene's palette-0 colour-0 is a green (28659) and the picture is not
     * flat, so nothing about the room or the palette was going to give black
     * here; it is the absence of a scene, so it is painted as one.
     *
     * What is left is a screen with no room loaded at all - the boot, the
     * logos, the title - and that is the one case where the tone has to be read
     * off the frame. */
    const bool cutscene = !panel && (margin_left != 0u || margin_right != 0u);
    const uint16_t backdrop = flat ? flat_colour
        : ((panel || cutscene) ? k_outside_colour : derive_backdrop(hw));
    if ((hw->lcdc & LCDC_LCD_ENABLE) == 0u) {
        /* The PPU is off. The accurate path still knows what the panel should
         * show, and it is not white - the engine draws the blank shade a real
         * Game Boy shows with the panel off. Copy it for the game's columns and
         * use it as the surrounding tone too, rather than painting a white
         * screen of our own: that white is what dropped the boot frames to
         * 98.98% while every other frame improved. */
        for (uint32_t y = 0; y < presenter->height; ++y) {
            uint32_t* row = presenter->pixels + (size_t)y * presenter->stride_pixels;
            for (uint32_t x = 0; x < presenter->width; ++x) {
                uint32_t in_game = x;
                if (x >= game_left && x < game_left + (uint32_t)SCREEN_WIDTH) {
                    in_game = x - game_left;
                } else if (hud_width != 0u && x >= hud_left && x < hud_left + hud_width) {
                    in_game = hud_source + (x - hud_left);
                } else {
                    in_game = 0u;
                }
                row[x] = hw->accurate_pixels[(size_t)y * SCREEN_WIDTH + in_game];
            }
        }
        return true;
    }

    /* Once per frame, before any pixel: take a copy of anything that has just
     * left the game's own view, then move the copies we are already carrying -
     * the move runs always, so they age out on schedule whether or not anything
     * draws them. */
    notice_exiting_sprites(presenter, hw, cgb, panel);
    sprite_shadow_advance(&presenter->shadows);

    /* Which camera the room's map is being read through. remastmain places it
     * from $FFB0/$FFB1 and $FFB2, which is exact; when a caller cannot supply
     * that, the search over the scroll registers runs instead. Either way the
     * placed camera is checked against the guest's own map, because a room whose
     * data disagrees with the guest is a defect that should be counted. */
    const RoomVramEntry* nbr_vram = NULL;
    if (presenter->room_map_enabled && presenter->room.has_room) {
        if (presenter->room.camera_from_guest) {
            (void)room_compositor_verify(&presenter->room, hw);
        } else {
            (void)room_compositor_align(&presenter->room, hw);
        }
        if (presenter->room.camera_valid && presenter->room.map.valid && panel && !flat &&
            (hw->lcdc & LCDC_LCD_ENABLE) != 0u) {
            room_vram_cache_store(presenter->room.map.area, presenter->room.map.room, hw);
        }
        /* A margin that leaves the room's own grid asks the transition table
         * for the room behind the door on that edge. The question is the
         * surface band, not the room's own travel: at the camera's limit the
         * travel is zero while the annexe is still 36 columns wide, and asking
         * with the travel made that annexe black even when the cartridge had a
         * room for it. The check is cheap when the band stays inside the room:
         * it does not decode a neighbour until the camera is actually leaving.
         */
        (void)room_compositor_set_neighbour(&presenter->room, presenter->rom,
                                            presenter->rom_size, presenter->room_bank,
                                            (int32_t)band_left, (int32_t)neighbour_right,
                                            presenter->source_node);
        /* The cartridge's table has nothing on these edges - it stops at area
         * 14. If the guest was watched crossing one of them, the room behind it
         * was learned, and the same placement rule draws it. The side the camera
         * was last heading toward is asked first: that is the margin the player
         * is walking into. */
        for (uint32_t attempt = 0u; attempt < 2u && !presenter->room.neighbour_ready;
             ++attempt) {
            const RoomLinkEdge edge = learned_edge_probe(presenter, attempt);
            uint8_t to_area = 0u;
            uint8_t to_room = 0u;
            if (!learned_links_lookup(&presenter->learned, presenter->room.map.area,
                                      presenter->room.map.room, (uint8_t)edge, &to_area,
                                      &to_room)) {
                continue;
            }
            if (room_compositor_set_neighbour_room(&presenter->room, presenter->rom,
                                                   presenter->rom_size, presenter->room_bank,
                                                   to_area, to_room, edge, (int32_t)band_left,
                                                   (int32_t)neighbour_right)) {
                presenter->learned_placements++;
            }
        }
        if (presenter->room.neighbour_ready) {
            nbr_vram = room_vram_cache_find(presenter->room.neighbour.area,
                                            presenter->room.neighbour.room);
            if (nbr_vram != NULL) {
                presenter->neighbour_placements++;
                /* The neighbour is an annexe of this room, so the scene extends to
                 * the surface band on the edge it was placed against. Without this
                 * the margin branch below never sees those columns: `room_left` is
                 * zero at the camera's limit and the whole band is painted as
                 * "past the room" before the compositor can answer. */
                if (panel && presenter->room.neighbour_place.edge == ROOM_LINK_EDGE_LEFT) {
                    room_left = band_left;
                } else if (panel && presenter->room.neighbour_place.edge == ROOM_LINK_EDGE_RIGHT) {
                    room_right = band_right;
                }
            }
        }
    }
    const uint32_t scene_left = game_left - room_left;
    const uint32_t scene_right = game_left + picture_width + room_right;
    const uint32_t own_scene_left = game_left - own_room_left;
    const uint32_t own_scene_right = game_left + picture_width + own_room_right;
    uint32_t bottom_non_scene_rows = 0u;
    if (panel) {
        for (uint32_t y = presenter->height / 2u; y < presenter->height; ++y) {
            if (!row_is_scene_map(hw, y, scene_map)) {
                bottom_non_scene_rows++;
            }
        }
    }
    const uint32_t overlay_left =
        (hud_left > picture_width) ? (hud_left - picture_width) / 2u : 0u;

    /* CGB vs DMG comes from the caller, never from LCDC. LCDC bit 0 is "BG and
     * window master priority", which on CGB only selects whether objects win
     * over the background - it is not a mode flag. Reading it as one sends
     * every frame with objects on top down the DMG palette path, a 4-entry grey
     * ramp, producing a scene that is structurally right and completely wrong
     * in colour. */
    for (uint32_t y = 0; y < presenter->height; ++y) {
        uint8_t row_has_sprite[SCREEN_WIDTH];
        memset(row_has_sprite, 0, sizeof(row_has_sprite));
        if (presenter->column_cache_enabled && !flat && panel &&
            (hw->lcdc & LCDC_OBJ_ENABLE) != 0u) {
            for (uint32_t s = 0u; s < GB_PRESENTATION_SPRITE_COUNT; ++s) {
                const GBPresentationSprite* sprite = &hw->sprites[s];
                if (sprite->raw_y != 0u) {
                    const int32_t h = (sprite->height != 0u) ? (int32_t)sprite->height : 8;
                    mask_sprite_row(
                        row_has_sprite, y, (int32_t)sprite->screen_x, (int32_t)sprite->screen_y, h);
                }
                if (presenter->prev_sprite_h[s] != 0u) {
                    mask_sprite_row(row_has_sprite, y, (int32_t)presenter->prev_sprite_x[s],
                                    (int32_t)presenter->prev_sprite_y[s],
                                    (int32_t)presenter->prev_sprite_h[s]);
                }
            }
        }
        const bool row_is_bottom_status =
            panel && !row_is_scene_map(hw, y, scene_map) &&
            !row_is_centered_overlay(y, presenter->height, bottom_non_scene_rows);
        uint32_t* row = presenter->pixels + (size_t)y * presenter->stride_pixels;
        for (uint32_t x = 0; x < presenter->width; ++x) {
            uint8_t ci = 0u;
            uint8_t prio = 0u;
            if (row_is_bottom_status) {
                /* Short bottom status strip (LIFE, boss HP, O2, alert):
                 * spans the full 160px Game Boy width contiguously at x = 0..159
                 * so right-side gauges like O2 are never split across the HUD gap. */
                if (x < (uint32_t)SCREEN_WIDTH) {
                    row[x] = hw->accurate_pixels[(size_t)y * SCREEN_WIDTH + x];
                    presenter->guest_pixels++;
                } else {
                    row[x] = flat ? backdrop : k_outside_colour;
                    presenter->outside_pixels++;
                }
                ci = 1u;
                prio = 1u;
            } else if (hud_width != 0u && x >= hud_left && x < hud_left + hud_width) {
                row[x] = hw->accurate_pixels[(size_t)y * SCREEN_WIDTH + hud_source + (x - hud_left)];
                presenter->window_pixels++;
            } else if (panel && !row_is_scene_map(hw, y, scene_map)) {
                if (x >= overlay_left && x < overlay_left + picture_width) {
                    /* Centred in-scene dialogue box (e.g. Mei Ling tutorial). */
                    row[x] = hw->accurate_pixels[(size_t)y * SCREEN_WIDTH + (x - overlay_left)];
                    presenter->guest_pixels++;
                    ci = 1u;
                    prio = 1u;
                } else if (!flat && x >= scene_left && x < scene_right &&
                           presenter->room_map_enabled && presenter->room.camera_valid) {
                    /* Flanks beside the centred dialogue box show the room scene,
                     * never the guest tilemap/cache (which hold dialogue tiles). */
                    uint8_t room_id = 0u;
                    uint8_t room_attr = 0u;
                    const int32_t core_x = (int32_t)x - (int32_t)game_left;
                    const RoomTileSource source = room_compositor_tile_from(
                        &presenter->room, core_x, (int32_t)y, &room_id, &room_attr);
                    if (source == ROOM_TILE_SOURCE_ROOM ||
                        (source == ROOM_TILE_SOURCE_NEIGHBOUR && nbr_vram != NULL)) {
                        const uint32_t world_x = (uint32_t)(presenter->room.camera_x + core_x);
                        const uint32_t world_y =
                            (uint32_t)(presenter->room.camera_y + (int32_t)y);
                        if (source == ROOM_TILE_SOURCE_NEIGHBOUR) {
                            row[x] = sample_pattern_raw(
                                nbr_vram->lcdc, nbr_vram->tile_data,
                                nbr_vram->background_palettes, room_id, room_attr, world_x,
                                world_y, cgb, &ci, &prio);
                            presenter->margin_from_neighbour++;
                        } else {
                            row[x] = sample_pattern(hw, room_id, room_attr, world_x, world_y, cgb,
                                                    &ci, &prio);
                            presenter->margin_from_room++;
                        }
                        presenter->margin_from_map++;
                        presenter->bg_pixels++;
                    } else {
                        row[x] = backdrop;
                        ci = 1u;
                        prio = 1u;
                        presenter->outside_pixels++;
                    }
                } else {
                    row[x] = flat ? backdrop : k_outside_colour;
                    ci = 1u;
                    prio = 1u;
                    presenter->outside_pixels++;
                }
            } else if (x >= game_left && x < game_left + picture_width) {
                const uint32_t game_x = x - game_left;
                const uint16_t exact =
                    hw->accurate_pixels[(size_t)y * SCREEN_WIDTH + game_x];
                row[x] = exact;
                if (!flat && panel && row_is_scene_map(hw, y, scene_map) &&
                    row_has_sprite[game_x] == 0u) {
                    column_remember(presenter, hw, game_x, y, exact);
                }
                presenter->guest_pixels++;
            } else if (x >= scene_left && x < scene_right) {
                if (flat) {
                    row[x] = backdrop;
                    ci = 1u;
                    prio = 1u;
                    presenter->margin_backdrop++;
                    presenter->outside_pixels++;
                } else if (presenter->room_map_enabled && presenter->room.camera_valid) {
                    uint8_t room_id = 0u;
                    uint8_t room_attr = 0u;
                    const int32_t core_x = (int32_t)x - (int32_t)game_left;
                    const RoomTileSource source = room_compositor_tile_from(
                        &presenter->room, core_x, (int32_t)y, &room_id, &room_attr);
                    if (source != ROOM_TILE_SOURCE_NONE) {
                        uint16_t replayed = 0u;
                        const bool replayable =
                            source == ROOM_TILE_SOURCE_ROOM || source == ROOM_TILE_SOURCE_BLANK;
                        const bool have_replay =
                            replayable && row_is_scene_map(hw, y, scene_map) &&
                            (source == ROOM_TILE_SOURCE_BLANK ||
                             !margin_is_leading(presenter, x, game_left, picture_width)) &&
                            column_replay(presenter, hw, x, y, camera_x, &ci, &prio, &replayed);
                        if (have_replay) {
                            row[x] = replayed;
                            presenter->margin_from_room++;
                            presenter->margin_replayed++;
                            presenter->bg_pixels++;
                        } else if (source == ROOM_TILE_SOURCE_BLANK) {
                            presenter->margin_room_miss++;
                            row[x] = backdrop;
                            ci = 1u;
                            prio = 1u;
                            presenter->margin_letterbox++;
                            presenter->outside_pixels++;
                        } else if (source == ROOM_TILE_SOURCE_NEIGHBOUR && nbr_vram == NULL) {
                            row[x] = backdrop;
                            ci = 1u;
                            prio = 1u;
                            presenter->margin_letterbox++;
                            presenter->outside_pixels++;
                        } else {
                            const uint32_t world_x = (uint32_t)(presenter->room.camera_x + core_x);
                            const uint32_t world_y =
                                (uint32_t)(presenter->room.camera_y + (int32_t)y);
                            if (source == ROOM_TILE_SOURCE_NEIGHBOUR) {
                                row[x] = sample_pattern_raw(
                                    nbr_vram->lcdc, nbr_vram->tile_data,
                                    nbr_vram->background_palettes, room_id, room_attr, world_x,
                                    world_y, cgb, &ci, &prio);
                                presenter->margin_from_neighbour++;
                            } else {
                                row[x] = sample_pattern(hw, room_id, room_attr, world_x, world_y,
                                                        cgb, &ci, &prio);
                                presenter->margin_from_room++;
                            }
                            presenter->margin_from_map++;
                            presenter->bg_pixels++;
                        }
                    } else {
                        presenter->margin_room_miss++;
                        if (x < own_scene_left || x >= own_scene_right) {
                            /* Outside the room's own travel, a missing neighbour
                             * cell must letterbox rather than sample the active
                             * room's wrapped PPU tilemap. */
                            row[x] = backdrop;
                            ci = 1u;
                            prio = 1u;
                            presenter->margin_letterbox++;
                            presenter->outside_pixels++;
                        } else {
                            resolve_margin_from_guest(presenter, hw, x, y, camera_x, game_left,
                                                      picture_width, backdrop, cgb, scene_map, row,
                                                      &ci, &prio);
                        }
                    }
                } else {
                    resolve_margin_from_guest(presenter, hw, x, y, camera_x, game_left,
                                              picture_width, backdrop, cgb, scene_map, row, &ci,
                                              &prio);
                }
            } else {
                row[x] = backdrop;
                ci = 1u;
                prio = 1u;
                if (!panel) {
                    presenter->margin_letterbox++;
                }
                presenter->outside_pixels++;
            }
            if (presenter->bg_color_index != NULL) {
                const size_t at = (size_t)y * presenter->width + x;
                presenter->bg_color_index[at] = ci;
                presenter->bg_attr_priority[at] = prio;
            }
        }
    }

    const bool have_room_actors =
        panel && !flat && !cutscene && presenter->actors.valid && presenter->rom != NULL &&
        presenter->room.camera_valid;
    if (have_room_actors) {
        const RoomActorRenderArgs actor_args = {
            .rom = presenter->rom,
            .rom_size = presenter->rom_size,
            .hw = hw,
            .cgb = cgb,
            .width = presenter->width,
            .height = presenter->height,
            .stride_pixels = presenter->stride_pixels,
            .pixels = presenter->pixels,
            .bg_color_index = presenter->bg_color_index,
            .bg_attr_priority = presenter->bg_attr_priority,
            .camera_x = presenter->room.camera_x,
            .camera_y = presenter->room.camera_y,
            .game_left = game_left,
            .picture_width = picture_width,
            .hud_left = hud_left,
            .scene_left = scene_left,
            .scene_right = scene_right,
            .overlay_left = overlay_left,
            .scene_map = scene_map,
            .bottom_non_scene_rows = bottom_non_scene_rows,
            .tracker = &presenter->actor_tracker,
        };
        RoomActorRenderStats actor_stats;
        room_actors_render(&presenter->actors, &actor_args, &actor_stats);
        presenter->room_actors_drawn += actor_stats.actors_drawn;
        presenter->room_actor_pixels_drawn += actor_stats.pixels_drawn;
    } else {
        room_actors_tracker_reset(&presenter->actor_tracker);
        const uint32_t shadow_room_right =
            panel ? (room_right > (uint32_t)SCREEN_WIDTH - picture_width
                         ? room_right - ((uint32_t)SCREEN_WIDTH - picture_width)
                         : 0u)
                  : room_right;
        /* Shadows go on last, over the margin the room revealed. They clip
         * themselves to the columns the PPU never drew, so this can only add
         * pixels the game did not have. */
        for (uint32_t i = 0; i < SPRITE_SHADOW_COUNT; ++i) {
            const SpriteShadow *shadow = &presenter->shadows.items[i];
            if (!shadow->alive) {
                continue;
            }
            const bool right = shadow->screen_x >= (int32_t)SCREEN_WIDTH;
            const bool left = shadow->screen_x < 0;
            if ((right && shadow_room_right == 0u) || (left && room_left == 0u)) {
                /* The room does not continue that way, so there is nothing for
                 * the shadow to walk into. */
                presenter->shadows_blocked_room++;
            }
        }
        ShadowDrawBox drawn;
        const ShadowBackground background = {
            .color_index = presenter->bg_color_index,
            .attr_priority = presenter->bg_attr_priority,
            .stride_pixels = presenter->stride_pixels,
            .objects_win_over_bg = (hw->lcdc & LCDC_BG_ENABLE) == 0u,
        };
        sprite_shadow_render(&presenter->shadows, presenter->pixels, presenter->stride_pixels,
                             presenter->height, (int32_t)game_left, room_left,
                             shadow_room_right, &background, &drawn);
        if (drawn.pixels > 0u) {
            presenter->shadow_pixels_last_frame = drawn.pixels;
            presenter->shadow_box_x0 = drawn.x0;
            presenter->shadow_box_x1 = drawn.x1;
            presenter->shadow_box_y0 = drawn.y0;
            presenter->shadow_box_y1 = drawn.y1;
            presenter->shadow_box_frame = presenter->lines_rendered / presenter->height;
        }
    }

    if (!cutscene) {
        render_margin_sprites(presenter, hw, cgb, game_left, picture_width, hud_left, scene_map);
    }

    presenter->lines_rendered += presenter->height;
    remember_frame_sprites(presenter, hw);
    return true;
}
/* ---------------------------------------------------------------------------
 * Object layer.
 *
 * Objects are where a whole-frame renderer most easily disagrees with a PPU,
 * because three hardware rules are per-scanline and the snapshot only gives us
 * the end of the frame:
 *
 *  - at most ten objects per line, chosen in OAM order, the rest dropped;
 *  - within a line the first object in that order with a non-zero colour wins;
 *  - on CGB an object still loses to a background pixel whose colour index is
 *    non-zero, unless LCDC bit 0 clears - that bit selects object priority over
 *    the background and is NOT a console mode flag.
 *
 * Object tiles always use 0x8000 addressing regardless of LCDC bit 4: the
 * signed-index window applies to the background only. Reading sprites through
 * the background addressing mode pulls plausible-looking tiles from the wrong
 * place and renders scenery where objects should be.
 * ------------------------------------------------------------------------ */


typedef struct ScanlineObject {
    int32_t screen_x;
    uint32_t bank;
    uint8_t palette;
    bool behind_bg;
    bool flip_x;
    uint8_t lo;
    uint8_t hi;
} ScanlineObject;

/* The object layer, in the margin only.
 *
 * The picture keeps the PPU's own compositing. The framebuffer already has its
 * objects and redrawing them from OAM over an exact region could only lose:
 * the hardware drops objects past ten a line and orders the rest by x, neither
 * of which is OAM order. This pass exists for the objects the framebuffer does
 * *not* have - the ones the guest placed outside its own 160 columns. They are
 * real, they are still in OAM, and they belong in the margin.
 *
 * Nothing here can touch the game's own columns, so the 160px picture is
 * byte-identical whether this runs or not.
 */
static void render_margin_sprites(
    NativePresenter* presenter,
    const GBPresentationHardwareSnapshot* hw,
    const bool cgb,
    const uint32_t game_left,
    const uint32_t picture_width,
    const uint32_t hud_left,
    const uint32_t scene_map) {
    if (presenter->pixels == NULL) {
        return;
    }
    if ((hw->lcdc & LCDC_LCD_ENABLE) == 0u || (hw->lcdc & LCDC_OBJ_ENABLE) == 0u) {
        return;
    }

    const bool objects_win_over_bg = (hw->lcdc & LCDC_BG_ENABLE) == 0u;

    for (uint32_t y = 0; y < presenter->height; ++y) {
        if (hud_left < presenter->width && !row_is_scene_map(hw, y, scene_map)) {
            continue;
        }
        ScanlineObject line[NATIVE_MAX_SPRITES_PER_LINE];
        int32_t count = 0;

        /* Selection: OAM order, first ten that cover this scanline. */
        for (uint32_t index = 0; index < GB_PRESENTATION_SPRITE_COUNT; ++index) {
            const GBPresentationSprite* sprite = &hw->sprites[index];
            if (sprite->raw_y == 0u) {
                continue; /* hardware convention for an unused OAM slot */
            }
            const int32_t top = (int32_t)sprite->screen_y;
            const int32_t object_height = sprite->height != 0 ? (int32_t)sprite->height : 8;
            if ((int32_t)y < top || (int32_t)y >= top + object_height) {
                continue;
            }
            if (count >= NATIVE_MAX_SPRITES_PER_LINE) {
                presenter->sprites_over_budget++;
                continue;
            }

            int32_t row = (int32_t)y - top;
            if ((sprite->attributes & OAM_OBJ_FLIP_Y) != 0u) {
                row = object_height - 1 - row;
            }
            uint32_t tile = sprite->tile;
            if (object_height == 16) {
                tile &= 0xFEu; /* 8x16 objects always pair tiles from an even index */
            }
            const uint32_t offset = tile * TILE_BYTES + (uint32_t)row * 2u;
            if (offset + 1u >= TILE_AREA) {
                continue;
            }
            const uint32_t bank = (cgb && (sprite->attributes & OAM_CGB_BANK) != 0u) ? 1u : 0u;

            presenter->sprites_considered++;
            /* Objects are world things the game happened to place on its own
             * 160px screen, so in the wider view they travel with the picture:
             * the game's view sits at game_left, so a sprite at screen x = 20
             * belongs 20 columns into the picture, and one at -2 belongs two
             * columns into the margin on the left.
             *
             * No modular arithmetic, and this is measured rather than assumed.
             * The guest parks unused objects at raw x 252-255 with a live y -
             * 40 of them in one 1000-frame outdoor run, sitting still, so their
             * previous x equals their current one. Masking the sum with 0xFF
             * folded those 87-pixels-off-the-right-edge objects back into the
             * left margin and drew them there as ghosts. Off the surface is off
             * the surface: the x loop's own bounds clip them. */
            line[count++] = (ScanlineObject){
                .screen_x = (int32_t)sprite->screen_x + (int32_t)game_left,
                .bank = bank,
                .palette = cgb ? (uint8_t)(sprite->attributes & OAM_CGB_PALETTE)
                               : (uint8_t)((sprite->attributes & OAM_OBJ_PALETTE) ? 1u : 0u),
                .behind_bg = (sprite->attributes & OAM_BG_PRIORITY) != 0u,
                .flip_x = (sprite->attributes & OAM_OBJ_FLIP_X) != 0u,
                .lo = hw->tile_data[bank][offset],
                .hi = hw->tile_data[bank][offset + 1u],
            };
        }

        uint32_t* row_out = presenter->pixels + (size_t)y * presenter->stride_pixels;

        for (uint32_t x = 0; x < presenter->width; ++x) {
            /* The picture is the PPU's; the status strip is the HUD's. What is
             * left is the margin, which is the only place an object the guest
             * placed outside its own playfield columns can belong. */
            if (x >= game_left && x < game_left + picture_width) {
                continue;
            }
            if (x >= hud_left) {
                continue;
            }
            const ScanlineObject* chosen = NULL;
            uint8_t color = 0u;

            for (int32_t i = 0; i < count; ++i) {
                const int32_t local = (int32_t)x - line[i].screen_x;
                if (local < 0 || local >= 8) {
                    continue;
                }
                /* Unflipped objects put their leftmost pixel at bit 7. */
                const uint8_t bit = line[i].flip_x ? (uint8_t)local : (uint8_t)(7 - local);
                const uint8_t candidate =
                    (uint8_t)(((line[i].lo >> bit) & 1u) | (((line[i].hi >> bit) & 1u) << 1));
                if (candidate == 0u) {
                    continue; /* colour 0 is transparent for objects */
                }
                chosen = &line[i];
                color = candidate;
                break;
            }
            if (chosen == NULL) {
                continue;
            }

            if (presenter->bg_color_index != NULL) {
                const size_t bg_index = (size_t)y * presenter->width + x;
                if (presenter->bg_color_index[bg_index] != 0u) {
                    const uint8_t bg_priority = presenter->bg_attr_priority[bg_index];
                    if (!objects_win_over_bg && (bg_priority != 0u || chosen->behind_bg)) {
                        presenter->sprite_pixels_hidden_by_bg++;
                        continue;
                    }
                }
            }

            if (cgb) {
                row_out[x] = read_palette(hw->object_palettes, chosen->palette, color);
            } else {
                row_out[x] = k_dmg_object_palette[color];
            }
            presenter->sprite_pixels_drawn++;
        }
    }
}
