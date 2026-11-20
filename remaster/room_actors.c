/* Read-only metasprite decoder and widescreen margin renderer for room actors. */

#include "room_actors.h"

#include <string.h>

#define LCDC_BG_ENABLE 0x01u
#define LCDC_OBJ_ENABLE 0x02u
#define LCDC_BG_MAP 0x08u
#define LCDC_LCD_ENABLE 0x80u

#define OAM_PALETTE_MASK 0x07u
#define OAM_VRAM_BANK 0x08u
#define OAM_FLIP_X 0x20u
#define OAM_FLIP_Y 0x40u
#define OAM_BG_PRIORITY 0x80u

#define TILE_BYTES 16u
#define MAX_DRAWN_ACTORS 16u
#define MAX_METASPRITE_PIECES 32u

static const uint16_t k_dmg_actor_palette[4] = {0x7FFFu, 0x5294u, 0x294Au, 0x0000u};

typedef enum ActorRendererKind {
    ACTOR_KIND_NONE = 0,
    ACTOR_KIND_BANK_0E,
    ACTOR_KIND_BANK_60,
    ACTOR_KIND_BANK_61,
    ACTOR_KIND_BANK_62,
    ACTOR_KIND_BANK_64,
} ActorRendererKind;

typedef struct ActorSlotRange {
    ActorRendererKind kind;
    uint8_t first_slot;
    uint8_t slot_count;
} ActorSlotRange;

typedef struct MetaspritePiece {
    int8_t dy;
    int8_t dx;
    uint8_t tile;
    uint8_t attr;
    bool dynamic_rom_tile;
    uint8_t rom_bank;
    uint16_t rom_addr;
} MetaspritePiece;

typedef struct QueuedActor {
    int32_t world_y;
    int32_t screen_x;
    int32_t screen_y;
    uint8_t culled;
    bool has_shadow;
    uint8_t piece_count;
    MetaspritePiece pieces[MAX_METASPRITE_PIECES];
} QueuedActor;

static bool rom_read8_bank(const uint8_t* rom,
                           size_t rom_size,
                           uint8_t bank,
                           uint16_t addr,
                           uint8_t* out) {
    if (rom == NULL || out == NULL) {
        return false;
    }
    size_t offset = 0u;
    if (bank == 0u) {
        if (addr >= 0x4000u) {
            return false;
        }
        offset = addr;
    } else {
        if (addr < 0x4000u || addr >= 0x8000u) {
            return false;
        }
        offset = (size_t)bank * 0x4000u + (size_t)(addr - 0x4000u);
    }
    if (offset >= rom_size) {
        return false;
    }
    *out = rom[offset];
    return true;
}

static bool rom_read16_bank(const uint8_t* rom,
                            size_t rom_size,
                            uint8_t bank,
                            uint16_t addr,
                            uint16_t* out) {
    uint8_t lo = 0u;
    uint8_t hi = 0u;
    if (!rom_read8_bank(rom, rom_size, bank, addr, &lo) ||
        !rom_read8_bank(rom, rom_size, bank, (uint16_t)(addr + 1u), &hi)) {
        return false;
    }
    *out = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
    return true;
}

static ActorSlotRange dispatch_bank_selector(const uint8_t* rom,
                                             size_t rom_size,
                                             uint8_t selector) {
    ActorSlotRange none = {ACTOR_KIND_NONE, 0u, 0u};
    if (selector >= 96u) {
        return none;
    }
    uint16_t stub = 0u;
    if (!rom_read16_bank(rom, rom_size, 0x06u, (uint16_t)(0x589Fu + (uint16_t)selector * 2u),
                         &stub)) {
        return none;
    }
    switch (stub) {
        case 0x595Fu:
            return (ActorSlotRange){ACTOR_KIND_BANK_0E, 0u, 8u};
        case 0x5966u:
            return (ActorSlotRange){ACTOR_KIND_BANK_0E, 0u, 16u};
        case 0x596Du:
            return (ActorSlotRange){ACTOR_KIND_BANK_60, 0u, 12u};
        case 0x5974u:
            return (ActorSlotRange){ACTOR_KIND_BANK_60, 0u, 12u};
        case 0x597Bu:
            return (ActorSlotRange){ACTOR_KIND_BANK_61, 0u, 10u};
        case 0x5982u:
            return (ActorSlotRange){ACTOR_KIND_BANK_61, 1u, 5u};
        case 0x5989u:
            return (ActorSlotRange){ACTOR_KIND_BANK_62, 0u, 12u};
        case 0x5990u:
            return (ActorSlotRange){ACTOR_KIND_BANK_64, 0u, 12u};
        default:
            return none;
    }
}

static uint16_t read_obj_palette(const uint8_t* palette_ram, uint8_t palette, uint8_t index) {
    const size_t offset = (size_t)palette * 8u + (size_t)index * 2u;
    return (uint16_t)((uint16_t)palette_ram[offset] | ((uint16_t)palette_ram[offset + 1u] << 8));
}

static uint8_t resolve_row_lcdc(const GBPresentationHardwareSnapshot* hw, uint32_t y) {
    if (y >= GB_PRESENTATION_ACCURATE_HEIGHT) {
        return hw->lcdc;
    }
    const uint8_t recorded = hw->scanline_lcdc[y];
    return ((recorded & LCDC_LCD_ENABLE) != 0u) ? recorded : hw->lcdc;
}

static bool pixel_visible_in_margin(const RoomActorRenderArgs* args,
                                    int32_t px,
                                    int32_t py,
                                    bool allow_scene_margin) {
    if (py < 0 || py >= (int32_t)args->height) {
        return false;
    }
    if (px < (int32_t)args->scene_left || px >= (int32_t)args->scene_right ||
        px >= (int32_t)args->hud_left || px < 0 || px >= (int32_t)args->width) {
        return false;
    }
    const uint8_t lcdc = resolve_row_lcdc(args->hw, (uint32_t)py);
    const uint32_t row_map = ((lcdc & LCDC_BG_MAP) != 0u) ? 1u : 0u;
    if (row_map == args->scene_map) {
        if (!allow_scene_margin) {
            return false;
        }
        return px < (int32_t)args->game_left ||
               px >= (int32_t)(args->game_left + args->picture_width);
    }
    const bool centered = (uint32_t)py < args->height / 2u ||
                          (args->bottom_non_scene_rows > 16u && (uint32_t)py + 8u < args->height);
    if (!centered) {
        return false;
    }
    return px < (int32_t)args->overlay_left ||
           px >= (int32_t)(args->overlay_left + args->picture_width);
}

static bool decode_actor_pieces(const RoomActorSnapshot* snap,
                                const RoomActorRenderArgs* args,
                                ActorRendererKind kind,
                                uint8_t slot,
                                const uint8_t* actor,
                                QueuedActor* out) {
    const uint8_t anim_id = actor[0x33];
    const uint8_t anim_override = actor[0x34];
    const uint8_t anim_frame = actor[0x35];
    const uint8_t raw_dir = actor[0x16];
    const uint8_t attr_override = actor[0x38];
    const uint8_t flags = actor[0x39];

    uint8_t frame_bank = 0u;
    uint16_t frame_table = 0u;
    uint8_t meta_bank = 0u;
    uint16_t meta_table = 0u;
    uint8_t eff_anim = 0u;
    uint8_t meta_anim_idx = 0u;
    uint8_t dir_frame = raw_dir;
    uint8_t dir_meta = raw_dir;
    uint8_t tile_offset = actor[0x37];
    bool use_dyn = false;
    bool has_shadow = false;

    if (kind == ACTOR_KIND_BANK_0E) {
        eff_anim = (anim_override < 0xF0u) ? anim_override : anim_id;
        if (eff_anim >= 0x60u) {
            return false;
        }
        frame_bank = 0x0Eu;
        frame_table = 0x47B4u;
        dir_frame = (uint8_t)(raw_dir & 7u);
        dir_meta = dir_frame;
        const uint8_t group = (uint8_t)(eff_anim >> 3);
        uint8_t dyn_flag = 0u;
        if (anim_id < 0x60u) {
            (void)rom_read8_bank(args->rom, args->rom_size, 0x0Eu, (uint16_t)(0x4694u + anim_id),
                                 &dyn_flag);
            uint8_t sh_flag = 0u;
            if (snap->shadow_disabled == 0u &&
                rom_read8_bank(args->rom, args->rom_size, 0x0Eu, (uint16_t)(0x46F4u + anim_id),
                               &sh_flag)) {
                has_shadow = (sh_flag & 1u) != 0u;
            }
        }
        if (group <= 2u) {
            meta_bank = 0x25u;
            meta_table = 0x5A29u;
            meta_anim_idx = eff_anim;
            use_dyn = (dyn_flag != 0u);
        } else if (group == 3u) {
            meta_bank = 0x26u;
            meta_table = 0x4AE7u;
            meta_anim_idx = (uint8_t)(eff_anim - 0x18u);
            use_dyn = (dyn_flag != 0u);
        } else if (group <= 6u) {
            meta_bank = 0x0Eu;
            meta_table = 0x5A64u;
            meta_anim_idx = (uint8_t)(eff_anim - 0x20u);
        } else {
            meta_bank = 0x26u;
            meta_table = 0x4B67u;
            meta_anim_idx = (uint8_t)(eff_anim - 0x38u);
            use_dyn = (dyn_flag != 0u);
        }
        if (use_dyn && slot < 8u) {
            tile_offset = (uint8_t)(0x60u + 6u * slot);
        }
    } else {
        if ((flags & 0x04u) != 0u) {
            return false;
        }
        eff_anim = (kind == ACTOR_KIND_BANK_64 || anim_override < 0xF0u) ? anim_override : anim_id;
        meta_anim_idx = eff_anim;
        dir_frame = (uint8_t)(raw_dir & 7u);
        dir_meta = dir_frame;
        switch (kind) {
            case ACTOR_KIND_BANK_60: {
                frame_bank = 0x60u;
                frame_table = 0x4519u;
                meta_bank = 0x60u;
                meta_table = 0x4C41u;
                uint8_t sh = 0u;
                if (snap->shadow_disabled == 0u &&
                    rom_read8_bank(args->rom, args->rom_size, 0x60u,
                                   (uint16_t)(0x51C5u + anim_id), &sh)) {
                    has_shadow = (sh != 0u);
                }
                break;
            }
            case ACTOR_KIND_BANK_61: {
                frame_bank = 0x61u;
                frame_table = 0x456Eu;
                meta_bank = 0x61u;
                meta_table = 0x4BFEu;
                const uint8_t group = (uint8_t)(eff_anim >> 3);
                uint8_t sh = 0u;
                if (snap->shadow_disabled == 0u && group != 5u && group != 7u &&
                    rom_read8_bank(args->rom, args->rom_size, 0x61u,
                                   (uint16_t)(0x50B6u + anim_id), &sh)) {
                    has_shadow = (sh != 0u);
                }
                break;
            }
            case ACTOR_KIND_BANK_62: {
                frame_bank = 0x62u;
                frame_table = 0x4428u;
                meta_bank = 0x62u;
                meta_table = 0x4E28u;
                uint8_t sh = 0u;
                if (snap->shadow_disabled == 0u &&
                    rom_read8_bank(args->rom, args->rom_size, 0x62u,
                                   (uint16_t)(0x5268u + anim_id), &sh)) {
                    has_shadow = (sh != 0u);
                }
                break;
            }
            case ACTOR_KIND_BANK_64:
                frame_bank = 0x64u;
                frame_table = 0x4313u;
                meta_bank = 0x64u;
                meta_table = 0x4483u;
                dir_meta = (uint8_t)(raw_dir & 7u);
                break;
            default:
                return false;
        }
    }

    uint16_t fl_ptr = 0u;
    if (!rom_read16_bank(args->rom, args->rom_size, frame_bank,
                         (uint16_t)(frame_table + (uint16_t)eff_anim * 16u + (uint16_t)dir_frame * 2u),
                         &fl_ptr)) {
        return false;
    }
    uint8_t pose = 0u;
    if (!rom_read8_bank(args->rom, args->rom_size, frame_bank,
                        (uint16_t)(fl_ptr + (uint16_t)anim_frame * 2u), &pose)) {
        return false;
    }

    uint16_t dir_ptr = 0u;
    if (!rom_read16_bank(
            args->rom, args->rom_size, meta_bank,
            (uint16_t)(meta_table + (uint16_t)meta_anim_idx * 16u + (uint16_t)dir_meta * 2u),
            &dir_ptr)) {
        return false;
    }
    uint16_t ms_ptr = 0u;
    if (!rom_read16_bank(args->rom, args->rom_size, meta_bank,
                         (uint16_t)(dir_ptr + (uint16_t)pose * 2u), &ms_ptr)) {
        return false;
    }

    uint8_t dyn_src_bank = 0u;
    uint16_t dyn_src_addr = 0u;
    bool dyn_src_valid = false;
    if (use_dyn && slot < 8u) {
        const uint8_t dyn_anim = (anim_override < 0x60u) ? anim_override : anim_id;
        if (dyn_anim < 0x60u) {
            const uint8_t b1 = actor[0x01];
            const uint16_t src_table =
                (b1 == 0x04u) ? 0x55E4u : ((b1 == 0x02u) ? 0x5824u : 0x4D74u);
            const uint16_t entry =
                (uint16_t)(src_table + (uint16_t)dyn_anim * 24u + (uint16_t)dir_frame * 3u);
            if (rom_read16_bank(args->rom, args->rom_size, 0x0Eu, entry, &dyn_src_addr) &&
                rom_read8_bank(args->rom, args->rom_size, 0x0Eu, (uint16_t)(entry + 2u),
                               &dyn_src_bank)) {
                dyn_src_valid = true;
            }
        }
    }

    out->has_shadow = has_shadow;
    out->piece_count = 0u;
    for (uint32_t i = 0u; i < MAX_METASPRITE_PIECES; ++i) {
        uint8_t b0 = 0u, b1 = 0u, b2 = 0u, b3 = 0u;
        const uint16_t pa = (uint16_t)(ms_ptr + i * 4u);
        if (!rom_read8_bank(args->rom, args->rom_size, meta_bank, pa, &b0)) {
            break;
        }
        if (b0 == 0x80u) {
            break;
        }
        if (!rom_read8_bank(args->rom, args->rom_size, meta_bank, (uint16_t)(pa + 1u), &b1) ||
            !rom_read8_bank(args->rom, args->rom_size, meta_bank, (uint16_t)(pa + 2u), &b2) ||
            !rom_read8_bank(args->rom, args->rom_size, meta_bank, (uint16_t)(pa + 3u), &b3)) {
            break;
        }
        MetaspritePiece* piece = &out->pieces[out->piece_count++];
        piece->dy = (int8_t)b0;
        piece->dx = (int8_t)b1;
        piece->tile = (uint8_t)(b2 + tile_offset);
        if (attr_override != 0u) {
            piece->attr = (uint8_t)(((b3 & 0xF8u) ^ (attr_override & 0xF0u)) |
                                    ((attr_override + b3) & OAM_PALETTE_MASK));
        } else {
            piece->attr = b3;
        }
        piece->dynamic_rom_tile =
            dyn_src_valid && b2 < 6u && (piece->attr & OAM_VRAM_BANK) == 0u;
        if (piece->dynamic_rom_tile) {
            piece->rom_bank = dyn_src_bank;
            piece->rom_addr =
                (uint16_t)(dyn_src_addr + (uint16_t)pose * 0x60u + (uint16_t)b2 * TILE_BYTES);
        } else {
            piece->rom_bank = 0u;
            piece->rom_addr = 0u;
        }
    }
    return out->piece_count > 0u;
}

static bool draw_tile_8x8(const RoomActorRenderArgs* args,
                          const uint8_t tile_bytes[TILE_BYTES],
                          int32_t canvas_x,
                          int32_t canvas_y,
                          uint8_t attr,
                          bool objects_win_over_bg,
                          bool allow_scene_margin,
                          RoomActorRenderStats* stats) {
    const bool flip_x = (attr & OAM_FLIP_X) != 0u;
    const bool flip_y = (attr & OAM_FLIP_Y) != 0u;
    const bool behind_bg = (attr & OAM_BG_PRIORITY) != 0u;
    const uint8_t palette =
        args->cgb ? (uint8_t)(attr & OAM_PALETTE_MASK) : (uint8_t)((attr & 0x10u) ? 1u : 0u);
    uint32_t drawn_here = 0u;

    for (uint32_t ty = 0u; ty < 8u; ++ty) {
        const int32_t py = canvas_y + (int32_t)ty;
        if (py < 0 || py >= (int32_t)args->height) {
            continue;
        }
        const uint32_t row_in_tile = flip_y ? (7u - ty) : ty;
        const uint8_t lo = tile_bytes[row_in_tile * 2u];
        const uint8_t hi = tile_bytes[row_in_tile * 2u + 1u];
        uint32_t* row_out = args->pixels + (size_t)(uint32_t)py * args->stride_pixels;

        for (uint32_t tx = 0u; tx < 8u; ++tx) {
            const int32_t px = canvas_x + (int32_t)tx;
            if (!pixel_visible_in_margin(args, px, py, allow_scene_margin)) {
                continue;
            }
            const uint8_t bit = flip_x ? (uint8_t)tx : (uint8_t)(7u - tx);
            const uint8_t ci = (uint8_t)(((lo >> bit) & 1u) | (((hi >> bit) & 1u) << 1));
            if (ci == 0u) {
                continue;
            }
            if (args->bg_color_index != NULL && args->bg_attr_priority != NULL) {
                const size_t bg_idx = (size_t)(uint32_t)py * args->width + (size_t)(uint32_t)px;
                if (args->bg_color_index[bg_idx] != 0u && !objects_win_over_bg &&
                    (args->bg_attr_priority[bg_idx] != 0u || behind_bg)) {
                    continue;
                }
            }
            row_out[px] = args->cgb ? read_obj_palette(args->hw->object_palettes, palette, ci)
                                    : k_dmg_actor_palette[ci];
            stats->pixels_drawn++;
            drawn_here++;
        }
    }
    if (drawn_here > 0u) {
        stats->pieces_drawn++;
        return true;
    }
    return false;
}

void room_actors_reset(RoomActorSnapshot* snap) {
    if (snap == NULL) {
        return;
    }
    memset(snap, 0, sizeof(*snap));
}

void room_actors_capture(const uint8_t* wram, size_t wram_size, RoomActorSnapshot* out) {
    if (out == NULL) {
        return;
    }
    room_actors_reset(out);
    if (wram == NULL || wram_size < ROOM_ACTOR_TABLE_ADDR + ROOM_ACTOR_TABLE_BYTES) {
        return;
    }
    out->valid = true;
    out->bank_selector = wram[ROOM_ACTOR_BANK_SEL_ADDR];
    out->shadow_disabled = wram[ROOM_ACTOR_SHADOW_DISABLE_ADDR];
    memcpy(out->table, wram + ROOM_ACTOR_TABLE_ADDR, ROOM_ACTOR_TABLE_BYTES);
}

void room_actors_render(const RoomActorSnapshot* snap,
                        const RoomActorRenderArgs* args,
                        RoomActorRenderStats* stats_out) {
    RoomActorRenderStats stats = {0u, 0u, 0u};
    if (stats_out != NULL) {
        *stats_out = stats;
    }
    if (snap == NULL || !snap->valid || args == NULL || args->rom == NULL || args->hw == NULL ||
        args->pixels == NULL) {
        return;
    }
    if ((args->hw->lcdc & LCDC_LCD_ENABLE) == 0u || (args->hw->lcdc & LCDC_OBJ_ENABLE) == 0u) {
        return;
    }

    const ActorSlotRange range =
        dispatch_bank_selector(args->rom, args->rom_size, snap->bank_selector);
    if (range.kind == ACTOR_KIND_NONE) {
        return;
    }

    QueuedActor queue[MAX_DRAWN_ACTORS];
    uint32_t queued = 0u;
    const uint8_t end_slot = (uint8_t)(range.first_slot + range.slot_count);

    for (uint8_t slot = range.first_slot; slot < end_slot && queued < MAX_DRAWN_ACTORS; ++slot) {
        const uint8_t raw_idx = (slot < 8u) ? slot : (uint8_t)(slot + 1u);
        if (raw_idx >= ROOM_ACTOR_RAW_SLOTS) {
            continue;
        }
        const uint8_t* actor = snap->table + (size_t)raw_idx * ROOM_ACTOR_STRIDE;
        if ((actor[0x00] & 1u) == 0u) {
            continue;
        }
        const uint8_t culled = actor[0x3A];
        if (culled != 0xFFu && culled != 0x00u) {
            continue;
        }

        const int32_t world_x = (int32_t)(uint16_t)(actor[0x12] | ((uint16_t)actor[0x13] << 8));
        const int32_t world_y = (int32_t)(uint16_t)(actor[0x14] | ((uint16_t)actor[0x15] << 8));
        const int32_t screen_x = world_x - args->camera_x;
        const int32_t screen_y = world_y - args->camera_y;
        const int32_t canvas_x = (int32_t)args->game_left + screen_x;

        if (canvas_x < -48 || canvas_x >= (int32_t)args->hud_left + 48 || screen_y < -48 ||
            screen_y >= (int32_t)args->height + 48) {
            continue;
        }

        QueuedActor item;
        memset(&item, 0, sizeof(item));
        item.world_y = world_y;
        item.screen_x = screen_x;
        item.screen_y = screen_y;
        item.culled = culled;

        if (decode_actor_pieces(snap, args, range.kind, slot, actor, &item)) {
            queue[queued++] = item;
        }
    }

    /* Stable insertion sort by world_y ascending (matching 00:0804 Y-sort). */
    for (uint32_t i = 1u; i < queued; ++i) {
        const QueuedActor key = queue[i];
        uint32_t j = i;
        while (j > 0u && queue[j - 1u].world_y > key.world_y) {
            queue[j] = queue[j - 1u];
            --j;
        }
        queue[j] = key;
    }

    const bool objects_win_over_bg = (args->hw->lcdc & LCDC_BG_ENABLE) == 0u;

    for (uint32_t a = 0u; a < queued; ++a) {
        const QueuedActor* item = &queue[a];
        bool drew_this_actor = false;

        if (item->has_shadow) {
            const int32_t piece_sx = item->screen_x - 4;
            const bool allow_scene = (item->culled == 0xFFu) || (piece_sx < -7 || piece_sx >= 160);
            const uint8_t* shadow_tile = &args->hw->tile_data[0][0x06u * TILE_BYTES];
            if (draw_tile_8x8(args, shadow_tile, (int32_t)args->game_left + piece_sx,
                              item->screen_y + 1, 0x06u, objects_win_over_bg, allow_scene,
                              &stats)) {
                drew_this_actor = true;
            }
        }

        /* Draw back-to-front so earlier OAM entries win opaque pixels. */
        for (int32_t p = (int32_t)item->piece_count - 1; p >= 0; --p) {
            const MetaspritePiece* piece = &item->pieces[p];
            const int32_t piece_sx = item->screen_x + (int32_t)piece->dx;
            const bool allow_scene = (item->culled == 0xFFu) || (piece_sx < -7 || piece_sx >= 160);
            uint8_t rom_tile[TILE_BYTES];
            const uint8_t* tile_ptr = NULL;
            if (piece->dynamic_rom_tile) {
                bool ok = true;
                for (uint16_t b = 0u; b < TILE_BYTES; ++b) {
                    if (!rom_read8_bank(args->rom, args->rom_size, piece->rom_bank,
                                        (uint16_t)(piece->rom_addr + b), &rom_tile[b])) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    tile_ptr = rom_tile;
                }
            }
            if (tile_ptr == NULL) {
                const uint32_t bank =
                    (args->cgb && (piece->attr & OAM_VRAM_BANK) != 0u) ? 1u : 0u;
                tile_ptr = &args->hw->tile_data[bank][(size_t)piece->tile * TILE_BYTES];
            }
            if (draw_tile_8x8(args, tile_ptr, (int32_t)args->game_left + piece_sx,
                              item->screen_y + (int32_t)piece->dy, piece->attr, objects_win_over_bg,
                              allow_scene, &stats)) {
                drew_this_actor = true;
            }
        }

        if (drew_this_actor) {
            stats.actors_drawn++;
        }
    }

    if (stats_out != NULL) {
        *stats_out = stats;
    }
}
