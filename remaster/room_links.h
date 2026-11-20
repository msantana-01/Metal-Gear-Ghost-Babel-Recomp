/* The room-transition table, read out of the cartridge.
 *
 * A widescreen margin that leaves the room's own layer is not blank space: it
 * is the room the game reaches through the door on that edge. Ghost Babel keeps
 * the answer in two per-room tables in bank $1F, and the guest reads them on
 * every door.
 *
 *   1F:5D13  $CBC0 -> $4CB3[area] -> [room]  ; transition base
 *            $CBC2 * 4                      ; four bytes an entry
 *            -> destination area, room, node, kind
 *
 *   1F:5D60  $CBC0 -> $4001[area] -> [room]  ; node base
 *            $CBC2 * 8                      ; eight bytes a node
 *            node[0..1] = page tile x, y
 *
 * `$CBC2` is the door index, written by the interaction handler at 06:46B2 as
 * `(tile_low_nibble - 4)`, so 0..11 are the reachable indices. The tables are
 * shared between rooms -- a room's pointer can land in the middle of another
 * room's run -- so this module reads exactly those 12 entries. That is what the
 * guest reads, not a guessed room boundary.
 *
 * The node position is a *page tile* (0..31), not a world pixel. The guest
 * checks the tile under the player, and the background map is a 256-pixel page,
 * so the same node names a different world position for every camera page. The
 * placement below resolves it through each room's header start camera, which is
 * the view the guest itself uses when it enters the room.
 *
 * Observation only. The cartridge is read, never written.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROOM_LINK_BANK 0x1Fu
#define ROOM_LINK_NODE_TABLE_CPU 0x4001u
#define ROOM_LINK_TRANSITION_TABLE_CPU 0x4CB3u
#define ROOM_LINK_NODE_BYTES 8u
#define ROOM_LINK_ENTRY_BYTES 4u
#define ROOM_LINK_COUNT 12u
#define ROOM_LINK_PAGE_TILES 32
#define ROOM_LINK_SCREEN_WIDTH 160
#define ROOM_LINK_SCREEN_HEIGHT 144

typedef enum RoomLinkEdge {
    ROOM_LINK_EDGE_NONE = 0,
    ROOM_LINK_EDGE_LEFT,
    ROOM_LINK_EDGE_RIGHT,
    ROOM_LINK_EDGE_TOP,
    ROOM_LINK_EDGE_BOTTOM,
} RoomLinkEdge;

/* One reachable door. `src_x`/`src_y` are page tiles (0..31); -1 means the
 * guest does not place this node (the `$FF` sentinel, or a stale shared-table
 * entry above the PPU's 32-tile page). `dst_*` is resolved the same way in the
 * destination room and is -1 when the destination does not name a real room or
 * node. */
typedef struct RoomLink {
    uint8_t node;
    uint8_t dst_area;
    uint8_t dst_room;
    uint8_t dst_node;
    uint8_t kind;
    int16_t src_x;
    int16_t src_y;
    int16_t dst_x;
    int16_t dst_y;
} RoomLink;

/* A room's view geometry, in world pixels. The layer origin is always
 * (256,256), and the header's first two words are the start camera. */
typedef struct RoomLinkGeometry {
    int32_t start_x;
    int32_t start_y;
    int32_t origin_x;
    int32_t origin_y;
    int32_t cols;
    int32_t rows;
} RoomLinkGeometry;

typedef struct RoomLinkPlacement {
    RoomLinkEdge edge;
    /* World position of the neighbour's local (0,0). Draw a neighbour pixel at
     * local world (wx,wy) at world (wx + origin_x, wy + origin_y). */
    int32_t origin_x;
    int32_t origin_y;
} RoomLinkPlacement;

/* Read the 12 reachable transitions. Returns the number written (always
 * ROOM_LINK_COUNT when the room's tables resolve); a room whose pointers fall
 * outside bank $1F returns 0 rather than half-filled links. */
uint32_t room_links_read(const uint8_t* rom,
                         size_t rom_size,
                         uint8_t area,
                         uint8_t room,
                         RoomLink out[ROOM_LINK_COUNT]);

/* Where a page tile lands on the 160x144 view for a camera position. The
 * background map is a 256-pixel page, so the answer is modulo 256; 160..255 is
 * the right margin (and 144..255 the bottom), not an invalid tile. */
int32_t room_link_page_screen(int32_t page_tile, int32_t camera);

/* The screen edge the source door is nearest, resolved at the room's start
 * camera. Returns NONE when the guest does not place the node. */
RoomLinkEdge room_link_edge(const RoomLink* link,
                            int32_t start_x,
                            int32_t start_y);

/* Place the neighbour so its start view meets the current room along `edge`.
 * Only LEFT and RIGHT are placements this projection extends; TOP and BOTTOM
 * return false. The guest cuts to the destination room's own header camera
 * (observed 2:4 -> 2:3: y 600 -> 632), so the two start views line up on the
 * axis the edge does not consume. The door and entry are decoded and validated,
 * but do not move the picture: aligning them pushed the destination's view out
 * of the margin. The result is snapped to the 8-pixel grid so the presenter's
 * flip lookup is identical in both rooms' frames. */
bool room_link_place(const RoomLink* link,
                     const RoomLinkGeometry* current,
                     const RoomLinkGeometry* neighbour,
                     RoomLinkEdge edge,
                     RoomLinkPlacement* out);

/* The same placement with the edge known and no door record, for the runtime
 * graph (learned_links.h). The formula does not use the door's page tile - that
 * tile only proves a door exists on the source side - so a room the guest was
 * seen entering can be placed by the edge it was entered through. */
bool room_link_place_edge(const RoomLinkGeometry* current,
                          const RoomLinkGeometry* neighbour,
                          RoomLinkEdge edge,
                          RoomLinkPlacement* out);
