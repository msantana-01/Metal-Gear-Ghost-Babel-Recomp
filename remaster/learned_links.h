/* What the cartridge does not say: which room is behind an edge.
 *
 * The door table in bank $1F has 16 area pointers, covering areas 0..14, and ends
 * at $4CD3 where area 0's room table begins. Area 15's pointer names the door
 * resolver's own code and areas 16..66 read past the array. In those areas the
 * guest changes room with scripted code (4F:46CE picks area 36, 48 or area+1;
 * 4F:46E7 dispatches by area from 32 up), so no static read can name the room
 * across a given edge of a given room. See docs/remaster-findings.md, "A tabela
 * de portas acaba na área 14".
 *
 * The guest can. It crosses the edge, writes $CBC0/$CBC1, and the picture
 * becomes the other room; watching that happen once is the only source that
 * exists for those areas. This module keeps what was seen, for the session
 * only: nothing is written to disk, so two runs over the same frames draw the
 * same pixels, and the first visit to a room still shows the room's own edge.
 *
 * The assumption, stated because it is one: a player who leaves a room by that
 * room's own edge has the next room beside it. That is the same rule the
 * cartridge's own placements already use for the 128 sides it can name
 * (origin_x = current.right - neighbour.start_x); here it is applied to an edge
 * the table cannot name. A scripted teleport that fires while the camera
 * happens to sit on a room's edge would be learned as an adjacency and drawn as
 * one. `conflicts` and the caller's own counters are what make that visible,
 * and the graph never overrides a cartridge entry - it is only consulted when
 * the door table has nothing.
 *
 * Observation only. Nothing here writes to the guest.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "room_links.h"

/* Every room the cartridge has, on both edges: 737 rooms x 2 = 1474 possible
 * keys, and 1536 holds all of them. The first size (512) filled after ~256
 * transitions, which a playthrough passes, and then learning stopped - the one
 * thing the table exists to avoid. Fixed so the whole graph is inside the
 * presenter and a frame can never allocate. */
#define LEARNED_LINKS_CAPACITY 1536u

typedef struct LearnedLink {
    uint8_t area;
    uint8_t room;
    uint8_t edge; /* ROOM_LINK_EDGE_LEFT or ROOM_LINK_EDGE_RIGHT */
    uint8_t to_area;
    uint8_t to_room;
} LearnedLink;

typedef struct LearnedLinks {
    LearnedLink entries[LEARNED_LINKS_CAPACITY];
    uint32_t count;
    /* Entries written, mirror included. */
    uint32_t recorded;
    /* The same edge later led somewhere else. The first answer is kept: a
     * margin that flips between two rooms is worse than one that stays with
     * what it first saw. */
    uint32_t conflicts;
    /* A change the graph had no room left for. Counted, not silently dropped. */
    uint32_t full;
} LearnedLinks;

void learned_links_init(LearnedLinks* links);

/* Which of the room's own edges the player left by.
 *
 * The camera stops at the room's walkable limits, so a camera sitting on one of
 * them is the only evidence a room change carries about *where* the player was.
 * A camera in the middle of a wide room means a door, and which way a door
 * leads is not visible from the camera - that answer is refused rather than
 * guessed. A room exactly one screen wide sits on both limits at once and is
 * refused for the same reason.
 *
 * `tolerance` is in world pixels and exists because the presenter places the
 * camera once per frame: the last frame inside the room can be a few pixels
 * short of the limit. */
#define LEARNED_LINK_EDGE_TOLERANCE 16

bool learned_links_exit_edge(int32_t camera_x,
                             int32_t limit_min_x,
                             int32_t limit_max_x,
                             uint8_t* out_edge);

/* Record that `area`/`room` was left by `edge` and the guest loaded
 * `to_area`/`to_room`, and the mirror: the room just left is on the opposite
 * edge of the room just entered. Returns the number of entries written (0, 1
 * or 2). A reload of the same room writes nothing.
 *
 * If the edge was seen before and led somewhere else, the first answer stays
 * and `conflicts` counts the disagreement; the mirror of a contradicted
 * observation is not stored either, because it is the same observation pointing
 * the other way. */
uint32_t learned_links_record(LearnedLinks* links,
                              uint8_t area,
                              uint8_t room,
                              uint8_t edge,
                              uint8_t to_area,
                              uint8_t to_room);

/* The room behind `edge` of `area`/`room`, when it was seen. */
bool learned_links_lookup(const LearnedLinks* links,
                          uint8_t area,
                          uint8_t room,
                          uint8_t edge,
                          uint8_t* out_area,
                          uint8_t* out_room);
