/* See room_compositor.h for why the camera is chosen from the guest's own map. */

#include "room_compositor.h"

#include <string.h>

#define MAP_DIM 32u
#define SCREEN_WIDTH GB_PRESENTATION_ACCURATE_WIDTH
#define SCREEN_HEIGHT GB_PRESENTATION_ACCURATE_HEIGHT

void room_compositor_init(RoomCompositor* compositor) {
    if (compositor == NULL) {
        return;
    }
    memset(compositor, 0, sizeof(*compositor));
}

bool room_compositor_set_room(RoomCompositor* compositor,
                              const uint8_t* rom,
                              size_t rom_size,
                              uint8_t switchable_bank,
                              uint8_t area,
                              uint8_t room) {
    if (compositor == NULL) {
        return false;
    }
    room_compositor_init(compositor);
    if (!room_map_read(rom, rom_size, switchable_bank, area, room, &compositor->map)) {
        return false;
    }
    compositor->link_count =
        room_links_read(rom, rom_size, area, room, compositor->links);
    compositor->has_room = true;
    return true;
}

static RoomLinkGeometry geometry_from_map(const RoomMap* map) {
    RoomLinkGeometry geometry;
    geometry.start_x = map->start_x;
    geometry.start_y = map->start_y;
    geometry.origin_x = map->origin_x;
    geometry.origin_y = map->origin_y;
    geometry.cols = (int32_t)map->cols;
    geometry.rows = (int32_t)map->rows;
    return geometry;
}

/* Place the neighbour already decoded in `compositor->neighbour` against
 * `edge`, and decide whether it actually reaches the band the presenter draws.
 *
 * `link` is the cartridge's door when there is one; the runtime graph has no
 * door record and passes NULL, because the edge is what places the room and the
 * door's page tile only ever proved that a door existed on the source side. */
static bool place_decoded_neighbour(RoomCompositor* compositor,
                                    const RoomLink* link,
                                    RoomLinkEdge edge,
                                    int32_t margin_left,
                                    int32_t margin_right) {
    const RoomLinkGeometry current = geometry_from_map(&compositor->map);
    const RoomLinkGeometry neighbour = geometry_from_map(&compositor->neighbour);
    RoomLinkPlacement placement;
    const bool placed = (link != NULL) ? room_link_place(link, &current, &neighbour, edge, &placement)
                                       : room_link_place_edge(&current, &neighbour, edge, &placement);
    if (!placed) {
        return false;
    }
    const int32_t band_left = compositor->camera_x - margin_left;
    const int32_t band_right = compositor->camera_x + (int32_t)SCREEN_WIDTH + margin_right;
    /* The neighbour has to reach into the band the presenter will draw on the
     * edge it is placed against. A placement wholly outside it draws nothing;
     * comparing against the room's grid instead accepted a door whose
     * destination sat a whole page away, counted it as placed, and left the
     * margin black. The two edges have different bands: the left one ends at
     * the camera, the right one starts at the camera plus the picture. */
    const int32_t neighbour_left = placement.origin_x + neighbour.origin_x;
    const int32_t neighbour_right = neighbour_left + neighbour.cols * ROOM_MAP_CELL_PX;
    const int32_t edge_band_left = (edge == ROOM_LINK_EDGE_RIGHT)
                                       ? compositor->camera_x + (int32_t)SCREEN_WIDTH
                                       : band_left;
    const int32_t edge_band_right =
        (edge == ROOM_LINK_EDGE_RIGHT) ? band_right : compositor->camera_x;
    const int32_t room_left = compositor->map.origin_x;
    const int32_t room_right =
        room_left + (int32_t)compositor->map.cols * ROOM_MAP_CELL_PX;
    const bool beyond_grid = (edge == ROOM_LINK_EDGE_RIGHT) ? (neighbour_right > room_right)
                                                           : (neighbour_left < room_left);
    if (!beyond_grid || neighbour_left >= edge_band_right || neighbour_right <= edge_band_left) {
        return false;
    }

    compositor->neighbour_place = placement;
    compositor->neighbour_ready = true;
    compositor->neighbour_placements++;
    return true;
}

static bool try_link(RoomCompositor* compositor,
                     const uint8_t* rom,
                     size_t rom_size,
                     uint8_t switchable_bank,
                     const RoomLink* link,
                     const RoomLinkGeometry* current,
                     int32_t margin_left,
                     int32_t margin_right,
                     bool left_over,
                     bool right_over) {
    if (link->src_x < 0 || link->src_y < 0 || link->dst_area >= 67u || link->dst_room >= 16u) {
        return false;
    }
    const RoomLinkEdge edge = room_link_edge(link, current->start_x, current->start_y);
    if ((edge == ROOM_LINK_EDGE_LEFT && !left_over) ||
        (edge == ROOM_LINK_EDGE_RIGHT && !right_over)) {
        return false;
    }
    if (edge != ROOM_LINK_EDGE_LEFT && edge != ROOM_LINK_EDGE_RIGHT) {
        return false;
    }

    if (!compositor->neighbour_decoded || compositor->neighbour.area != link->dst_area ||
        compositor->neighbour.room != link->dst_room) {
        if (!room_map_read(rom, rom_size, switchable_bank, link->dst_area, link->dst_room,
                           &compositor->neighbour)) {
            compositor->neighbour_decoded = false;
            return false;
        }
        compositor->neighbour_decoded = true;
    }

    return place_decoded_neighbour(compositor, link, edge, margin_left, margin_right);
}

bool room_compositor_set_neighbour(RoomCompositor* compositor,
                                   const uint8_t* rom,
                                   size_t rom_size,
                                   uint8_t switchable_bank,
                                   int32_t margin_left,
                                   int32_t margin_right,
                                   uint8_t source_node) {
    if (compositor == NULL || rom == NULL) {
        return false;
    }
    compositor->neighbour_ready = false;
    if (!compositor->has_room || !compositor->camera_valid || compositor->link_count == 0u) {
        return false;
    }

    const int32_t left = compositor->map.origin_x;
    const int32_t right =
        compositor->map.origin_x + (int32_t)compositor->map.cols * ROOM_MAP_CELL_PX;
    const int32_t band_left = compositor->camera_x - margin_left;
    const int32_t band_right =
        compositor->camera_x + (int32_t)SCREEN_WIDTH + margin_right;
    const bool left_over = margin_left > 0 && band_left < left;
    const bool right_over = margin_right > 0 && band_right > right;
    if (!left_over && !right_over) {
        return false;
    }

    const RoomLinkGeometry current = geometry_from_map(&compositor->map);
    /* The guest's own door first. $CBC2 is the index of the tile under the
     * player, so it is the transition the player is about to take; the table
     * can hold several doors on the same edge, each leading somewhere else. */
    const RoomLink* preferred =
        source_node < compositor->link_count ? &compositor->links[source_node] : NULL;
    if (preferred != NULL &&
        try_link(compositor, rom, rom_size, switchable_bank, preferred, &current, margin_left,
                 margin_right, left_over, right_over)) {
        return true;
    }
    for (uint32_t i = 0u; i < compositor->link_count; i++) {
        if (&compositor->links[i] == preferred) {
            continue;
        }
        if (try_link(compositor, rom, rom_size, switchable_bank, &compositor->links[i], &current,
                     margin_left, margin_right, left_over, right_over)) {
            return true;
        }
    }
    return false;
}

bool room_compositor_set_neighbour_room(RoomCompositor* compositor,
                                        const uint8_t* rom,
                                        size_t rom_size,
                                        uint8_t switchable_bank,
                                        uint8_t dst_area,
                                        uint8_t dst_room,
                                        RoomLinkEdge edge,
                                        int32_t margin_left,
                                        int32_t margin_right) {
    if (compositor == NULL || rom == NULL) {
        return false;
    }
    compositor->neighbour_ready = false;
    if (!compositor->has_room || !compositor->camera_valid) {
        return false;
    }
    if (edge != ROOM_LINK_EDGE_LEFT && edge != ROOM_LINK_EDGE_RIGHT) {
        return false;
    }
    /* The band has to leave the room's own grid on that edge, exactly as the
     * cartridge path asks. A room the guest was seen entering is still not
     * drawn over the scenery this room already has. */
    const int32_t left = compositor->map.origin_x;
    const int32_t right = left + (int32_t)compositor->map.cols * ROOM_MAP_CELL_PX;
    const int32_t band_left = compositor->camera_x - margin_left;
    const int32_t band_right = compositor->camera_x + (int32_t)SCREEN_WIDTH + margin_right;
    const bool over = (edge == ROOM_LINK_EDGE_LEFT) ? (margin_left > 0 && band_left < left)
                                                    : (margin_right > 0 && band_right > right);
    if (!over) {
        return false;
    }
    if (dst_area >= 67u || dst_room >= 16u) {
        return false;
    }
    if (!compositor->neighbour_decoded || compositor->neighbour.area != dst_area ||
        compositor->neighbour.room != dst_room) {
        if (!room_map_read(rom, rom_size, switchable_bank, dst_area, dst_room,
                           &compositor->neighbour)) {
            compositor->neighbour_decoded = false;
            return false;
        }
        compositor->neighbour_decoded = true;
    }
    return place_decoded_neighbour(compositor, NULL, edge, margin_left, margin_right);
}

/* The map a scanline was drawn from. Ghost Babel switches LCDC bit 3 mid-frame
 * for the HUD band, so the frame-end value describes the room rows and not the
 * band's; the snapshot records the register per row for exactly this. */
static uint32_t row_map_index(const GBPresentationHardwareSnapshot* hw, int32_t screen_y) {
    uint8_t lcdc = hw->lcdc;
    if (screen_y >= 0 && screen_y < (int32_t)SCREEN_HEIGHT) {
        const uint8_t recorded = hw->scanline_lcdc[screen_y];
        if ((recorded & 0x80u) != 0u) {
            lcdc = recorded;
        }
    }
    return (lcdc & 0x08u) != 0u ? 1u : 0u;
}

/* The guest's own tile for a world pixel, read the way the PPU reads it: the
 * background map is a 256x256-pixel window, so the position wraps. */
static bool guest_tile(const GBPresentationHardwareSnapshot* hw,
                       uint32_t map_index,
                       int32_t world_x,
                       int32_t world_y,
                       uint8_t* out_id,
                       uint8_t* out_attribute) {
    const uint32_t position =
        (uint32_t)(((uint32_t)world_y & 0xFFu) >> 3) * MAP_DIM + (((uint32_t)world_x & 0xFFu) >> 3);
    if (position >= GB_PRESENTATION_TILE_MAP_BYTES) {
        return false;
    }
    *out_id = hw->tile_ids[map_index][position];
    *out_attribute = hw->tile_attributes[map_index][position];
    return true;
}

/* How many probes a camera position explains. A tile counts when both the id
 * and the attribute agree, because a camera 256 pixels away would still land on
 * a plausible tile id every so often -- the attribute is what makes a wrong
 * page visible. */
static uint32_t score_camera(const RoomCompositor* compositor,
                             const GBPresentationHardwareSnapshot* hw,
                             int32_t camera_x,
                             int32_t camera_y,
                             uint32_t* out_probes) {
    uint32_t matches = 0u;
    uint32_t probes = 0u;
    for (int32_t screen_y = 0; screen_y < (int32_t)SCREEN_HEIGHT; screen_y += ROOM_COMPOSITOR_PROBE_STEP) {
        for (int32_t screen_x = 0; screen_x < (int32_t)SCREEN_WIDTH; screen_x += ROOM_COMPOSITOR_PROBE_STEP) {
            const int32_t world_x = camera_x + screen_x;
            const int32_t world_y = camera_y + screen_y;
            uint8_t room_id = 0u;
            uint8_t room_attr = 0u;
            if (!room_map_tile(&compositor->map, world_x, world_y, &room_id, &room_attr)) {
                continue;
            }
            uint8_t guest_id = 0u;
            uint8_t guest_attr = 0u;
            if (!guest_tile(hw, row_map_index(hw, screen_y), world_x, world_y, &guest_id, &guest_attr)) {
                continue;
            }
            probes++;
            if (room_id == guest_id && room_attr == guest_attr) {
                matches++;
            }
        }
    }
    if (out_probes != NULL) {
        *out_probes = probes;
    }
    return matches;
}

/* The camera the guest itself is holding, in world pixels. Nothing here is
 * inferred: remastmain reads $FFB0/$FFB1 (and $FFB2 for Y), which is the same
 * value the guest copies into SCX/SCY. That is exact where SCX alone is a
 * byte, and it makes the search below a fallback rather than the path. */
bool room_compositor_place(RoomCompositor* compositor, int32_t camera_x, int32_t camera_y) {
    if (compositor == NULL || !compositor->has_room || camera_x < 0 || camera_y < 0) {
        return false;
    }
    compositor->camera_x = camera_x;
    compositor->camera_y = camera_y;
    compositor->camera_valid = true;
    compositor->camera_from_guest = true;
    return true;
}

bool room_compositor_verify(RoomCompositor* compositor,
                            const GBPresentationHardwareSnapshot* hw) {
    if (compositor == NULL || hw == NULL || !compositor->has_room || !compositor->camera_valid) {
        return false;
    }
    uint32_t probes = 0u;
    const uint32_t matches =
        score_camera(compositor, hw, compositor->camera_x, compositor->camera_y, &probes);
    compositor->probed_tiles += probes;
    compositor->probe_mismatches += probes - matches;
    return probes >= ROOM_COMPOSITOR_PROBE_MIN_MATCHES && matches == probes;
}

bool room_compositor_align(RoomCompositor* compositor,
                           const GBPresentationHardwareSnapshot* hw) {
    if (compositor == NULL || hw == NULL || !compositor->has_room) {
        return false;
    }
    compositor->camera_valid = false;

    /* The scroll register names the camera modulo 256; the room's own grid says
     * which world positions are possible. That is at most a handful. The grid,
     * not the record: the record is the walkable box and the grid is the drawn
     * room, and the camera may sit over part of the room the player cannot
     * walk to. */
    uint32_t best_matches = 0u;
    int32_t best_x = 0;
    int32_t best_y = 0;
    bool best_found = false;

    /* Step by 256, not by one: only the positions congruent to the scroll
     * register are candidates, and a room is at most 4095 pixels across. A grid
     * narrower than the screen admits exactly one camera, its origin. */
    const int32_t limit_min_x = compositor->map.origin_x;
    const int32_t limit_min_y = compositor->map.origin_y;
    const int32_t grid_max_x = compositor->map.origin_x +
                               (int32_t)compositor->map.cols * ROOM_MAP_CELL_PX - SCREEN_WIDTH;
    const int32_t grid_max_y = compositor->map.origin_y +
                               (int32_t)compositor->map.rows * ROOM_MAP_CELL_PX - SCREEN_HEIGHT;
    const int32_t limit_max_x = grid_max_x > limit_min_x ? grid_max_x : limit_min_x;
    const int32_t limit_max_y = grid_max_y > limit_min_y ? grid_max_y : limit_min_y;
    const int32_t first_x =
        limit_min_x + ((((int32_t)hw->scx - limit_min_x) % 256) + 256) % 256;
    const int32_t first_y =
        limit_min_y + ((((int32_t)hw->scy - limit_min_y) % 256) + 256) % 256;
    for (int32_t camera_y = first_y; camera_y <= limit_max_y; camera_y += 256) {
        for (int32_t camera_x = first_x; camera_x <= limit_max_x; camera_x += 256) {
            uint32_t probes = 0u;
            const uint32_t matches = score_camera(compositor, hw, camera_x, camera_y, &probes);
            if (probes < ROOM_COMPOSITOR_PROBE_MIN_MATCHES) {
                continue;
            }
            /* A tie is not evidence. Keeping the first is fine only because the
             * candidates are 256 pixels apart and a room that promoted two of
             * them equally would be drawing the same picture twice. */
            if (!best_found || matches > best_matches) {
                best_found = true;
                best_matches = matches;
                best_x = camera_x;
                best_y = camera_y;
            }
        }
    }

    if (!best_found) {
        compositor->unaligned_frames++;
        return false;
    }
    compositor->camera_x = best_x;
    compositor->camera_y = best_y;
    compositor->camera_valid = true;
    compositor->aligned_frames++;
    return true;
}

/* The cell at a world pixel, or false when the pixel is outside the grid. */
static bool cell_blank_at(const RoomMap* map, int32_t world_x, int32_t world_y) {
    if (map == NULL || !map->valid) {
        return false;
    }
    const int32_t local_x = world_x - map->origin_x;
    const int32_t local_y = world_y - map->origin_y;
    if (local_x < 0 || local_y < 0) {
        return false;
    }
    const int32_t cx = local_x / ROOM_MAP_CELL_PX;
    const int32_t cy = local_y / ROOM_MAP_CELL_PX;
    if (cx > 0xFFFF || cy > 0xFFFF) {
        return false;
    }
    return room_map_cell_blank(map, (uint16_t)cx, (uint16_t)cy);
}

RoomTileSource room_compositor_tile_from(RoomCompositor* compositor,
                                         int32_t screen_x,
                                         int32_t screen_y,
                                         uint8_t* out_id,
                                         uint8_t* out_attribute) {
    if (compositor == NULL || !compositor->has_room || !compositor->camera_valid ||
        out_id == NULL || out_attribute == NULL) {
        return ROOM_TILE_SOURCE_NONE;
    }
    const int32_t world_x = compositor->camera_x + screen_x;
    const int32_t world_y = compositor->camera_y + screen_y;
    const bool room_blank = cell_blank_at(&compositor->map, world_x, world_y);
    if (!room_blank && room_map_tile(&compositor->map, world_x, world_y, out_id, out_attribute)) {
        compositor->tiles_resolved++;
        return ROOM_TILE_SOURCE_ROOM;
    }
    if (compositor->neighbour_ready) {
        const int32_t neighbour_x = world_x - compositor->neighbour_place.origin_x;
        const int32_t neighbour_y = world_y - compositor->neighbour_place.origin_y;
        if (!cell_blank_at(&compositor->neighbour, neighbour_x, neighbour_y) &&
            room_map_tile(&compositor->neighbour, neighbour_x, neighbour_y, out_id, out_attribute)) {
            compositor->neighbour_tiles++;
            return ROOM_TILE_SOURCE_NEIGHBOUR;
        }
        compositor->neighbour_misses++;
    }
    compositor->tiles_missing++;
    /* The cell exists but is padding, and no neighbour reaches it: the room
     * ends here. Saying so is what lets the presenter letterbox instead of
     * drawing a flat band the room does not contain. */
    return room_blank ? ROOM_TILE_SOURCE_BLANK : ROOM_TILE_SOURCE_NONE;
}

bool room_compositor_tile(RoomCompositor* compositor,
                          int32_t screen_x,
                          int32_t screen_y,
                          uint8_t* out_id,
                          uint8_t* out_attribute) {
    const RoomTileSource source =
        room_compositor_tile_from(compositor, screen_x, screen_y, out_id, out_attribute);
    return source == ROOM_TILE_SOURCE_ROOM || source == ROOM_TILE_SOURCE_NEIGHBOUR;
}
