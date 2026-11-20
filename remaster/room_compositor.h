/* Placing the room's map on the screen, so the margin can be drawn from it.
 *
 * room_map.h answers "what tile is at this world pixel". The presenter needs a
 * different question answered: "what tile is at this *screen* pixel". The
 * difference is the camera, and the snapshot only carries its low byte -- SCX
 * and SCY name a position in a 256-pixel background window, not a world
 * position.
 *
 * The room record bounds the camera, so the candidates are the positions inside
 * the record's limits whose low byte is the scroll register. A room taller or
 * wider than 256 pixels admits more than one, and nothing in the snapshot
 * chooses between them. So the choice is made from evidence: for each candidate,
 * compare the tiles the room map gives for the game's own 160x144 window against
 * the tiles the guest actually wrote into its background map. Only the camera
 * the guest is using explains the guest's own map.
 *
 * That also makes the alignment a check. A room whose map does not explain the
 * live map -- a wrong area, a stale room, a record that is not a rectangle -- is
 * refused rather than drawn, and the presenter keeps its old path.
 *
 * Measured on the two savestates of area 2 room 4: the alignment picks
 * (505, 600) and (434, 600), and every probed tile agrees with the guest's map.
 * The candidates that are 256 pixels away do not explain it, so the choice is
 * not a tie.
 *
 * Observation only. Nothing here writes to the guest.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gbrt_presentation.h"
#include "room_links.h"
#include "room_map.h"

/* How many screen positions the alignment probes, spread over the game's own
 * window. The probe compares tile ids and attributes, not pixels: it is asking
 * which camera the guest is using, and the guest's own map is the answer. */
#define ROOM_COMPOSITOR_PROBE_STEP 16
#define ROOM_COMPOSITOR_PROBE_MIN_MATCHES 8u

typedef struct RoomCompositor {
    bool has_room;
    RoomMap map;

    /* The room beyond the edge the camera is leaving, when the cartridge's
     * transition table names one. A margin that leaves this room's own layer is
     * not blank: it is the room behind the door on that edge. The layer is
     * decoded once per destination and placed against the edge, so a frame that
     * keeps the same neighbour pays only the lookup. */
    RoomLink links[ROOM_LINK_COUNT];
    uint32_t link_count;
    RoomMap neighbour;
    bool neighbour_decoded;
    bool neighbour_ready;
    RoomLinkPlacement neighbour_place;

    /* The world position of screen pixel (0,0). */
    int32_t camera_x;
    int32_t camera_y;
    bool camera_valid;
    /* True when the camera came from the guest's own variables ($FFB0/$FFB1,
     * $FFB2) rather than from the search. The guest's value is exact, so it is
     * the path; the search below only runs for callers that cannot supply it. */
    bool camera_from_guest;

    /* Diagnostics: how often the map explained the guest's own window, and how
     * often a requested tile fell outside the room. The presenter reports these
     * rather than letting a wrong margin look like a drawn one. */
    uint32_t aligned_frames;
    uint32_t unaligned_frames;
    uint32_t tiles_resolved;
    uint32_t tiles_missing;
    uint32_t neighbour_tiles;
    uint32_t neighbour_misses;
    uint32_t neighbour_placements;
    /* Verification: the placed camera's probes, and how many of them disagreed
     * with the guest's own map. A non-zero mismatch means the room's data and
     * the guest disagree -- our bug, not an ambiguous camera. */
    uint32_t probed_tiles;
    uint32_t probe_mismatches;
} RoomCompositor;

void room_compositor_init(RoomCompositor* compositor);

/* Decode the room's map. Returns false when the cartridge does not describe
 * this room; the compositor is then not in use, and the caller keeps whatever
 * margin path it had. */
bool room_compositor_set_room(RoomCompositor* compositor,
                              const uint8_t* rom,
                              size_t rom_size,
                              uint8_t switchable_bank,
                              uint8_t area,
                              uint8_t room);

/* Place the camera at the guest's own camera, in world pixels. This is the
 * path: $FFB0/$FFB1 hold the camera, and its low byte is what the guest copies
 * into SCX, so it agrees with the picture by construction. Returns false when
 * the compositor has no room. */
bool room_compositor_place(RoomCompositor* compositor, int32_t camera_x, int32_t camera_y);

/* Check the placed camera against the guest's own background map. Diagnostic
 * rather than structural: with the guest's camera there is nothing to choose,
 * so a mismatch is reported and counted instead of being allowed to move the
 * picture. Returns true when every probe agreed. */
bool room_compositor_verify(RoomCompositor* compositor,
                            const GBPresentationHardwareSnapshot* hw);

/* Fallback for a caller that cannot supply the camera: pick it from the scroll
 * registers and the guest's own map. SCX/SCY name the camera only modulo 256,
 * so a room wider or taller than 256 pixels can admit more than one candidate;
 * whichever the map explains best wins. Only used when camera_from_guest is
 * false. */
bool room_compositor_align(RoomCompositor* compositor,
                           const GBPresentationHardwareSnapshot* hw);

/* The (tile id, attribute) the room puts at a screen pixel. Returns false
 * outside the room (the camera stops at the record's limits), or when the
 * compositor has no camera. */
bool room_compositor_tile(RoomCompositor* compositor,
                          int32_t screen_x,
                          int32_t screen_y,
                          uint8_t* out_id,
                          uint8_t* out_attribute);

/* Which layer answered room_compositor_tile_from. */
typedef enum RoomTileSource {
    ROOM_TILE_SOURCE_NONE = 0,
    ROOM_TILE_SOURCE_ROOM,
    ROOM_TILE_SOURCE_NEIGHBOUR,
    /* Inside the grid, but the cell names no tile: the layer's padding. The
     * room is over on this side, so the caller letterboxes rather than paints
     * the flat band a sample of tile 0 gives. */
    ROOM_TILE_SOURCE_BLANK,
} RoomTileSource;

/* The same lookup, saying which room answered. A neighbour tile is still the
 * cartridge's own room -- it is the room behind the door on this edge -- but it
 * is not the current room, and the report keeps the two counts apart. */
RoomTileSource room_compositor_tile_from(RoomCompositor* compositor,
                                         int32_t screen_x,
                                         int32_t screen_y,
                                         uint8_t* out_id,
                                         uint8_t* out_attribute);

/* Choose and decode the room behind the edge the surface margin is leaving.
 *
 * `margin_left`/`margin_right` are the surface band the presenter can fill on
 * each side (36 px in the widescreen), not the room's own travel. The distinction
 * matters at the camera's limit: the travel is zero there, while the band still
 * leaves the grid and the annexe is still visible. The check runs against the
 * camera the compositor was placed at: a band that is still inside the room's
 * own grid needs no neighbour and does not decode one. When the band does leave
 * the grid, the transition the guest itself
 * keeps in $CBC2 -- the door under the player -- is preferred when it sits on
 * that edge; otherwise the first transition whose source node sits on that
 * edge, whose destination resolves, and whose layer reaches back into the
 * margin is used. A room can have several doors on one edge, and the first one
 * is not necessarily the one the player is about to take. `source_node` is
 * $CBC2, or 0xFF when the caller has no value. Returns false, leaving the old
 * margin path in place, when there is no such transition. */
bool room_compositor_set_neighbour(RoomCompositor* compositor,
                                   const uint8_t* rom,
                                   size_t rom_size,
                                   uint8_t switchable_bank,
                                   int32_t margin_left,
                                   int32_t margin_right,
                                   uint8_t source_node);

/* Place an explicit room against `edge`, with no door record.
 *
 * The cartridge's transition table stops at area 14 (learned_links.h), so for
 * the rooms past it the only source of a neighbour is what the guest was seen
 * doing: it loaded the destination room while the player crossed this edge. The
 * band, the decode, and the reach into the margin are the same rules the
 * cartridge path uses; only the question "which room" comes from elsewhere. */
bool room_compositor_set_neighbour_room(RoomCompositor* compositor,
                                        const uint8_t* rom,
                                        size_t rom_size,
                                        uint8_t switchable_bank,
                                        uint8_t dst_area,
                                        uint8_t dst_room,
                                        RoomLinkEdge edge,
                                        int32_t margin_left,
                                        int32_t margin_right);
