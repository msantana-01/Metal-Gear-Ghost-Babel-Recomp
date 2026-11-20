/* See learned_links.h: the room behind an edge the cartridge does not name. */

#include "learned_links.h"

#include <stddef.h>

void learned_links_init(LearnedLinks* links) {
    if (links == NULL) {
        return;
    }
    links->count = 0u;
    links->recorded = 0u;
    links->conflicts = 0u;
    links->full = 0u;
}

bool learned_links_exit_edge(int32_t camera_x,
                             int32_t limit_min_x,
                             int32_t limit_max_x,
                             uint8_t* out_edge) {
    if (out_edge == NULL) {
        return false;
    }
    *out_edge = (uint8_t)ROOM_LINK_EDGE_NONE;
    if (limit_max_x <= limit_min_x) {
        /* One screen wide or narrower: the camera is on both limits at once, so
         * the room change says nothing about the side. */
        return false;
    }
    const bool at_max = camera_x >= limit_max_x - LEARNED_LINK_EDGE_TOLERANCE;
    const bool at_min = camera_x <= limit_min_x + LEARNED_LINK_EDGE_TOLERANCE;
    if (at_max == at_min) {
        /* In the middle of a wide room (a door), or so far outside the limits
         * that the record and the camera disagree. Neither names an edge. */
        return false;
    }
    *out_edge = at_max ? (uint8_t)ROOM_LINK_EDGE_RIGHT : (uint8_t)ROOM_LINK_EDGE_LEFT;
    return true;
}

static uint8_t opposite_edge(uint8_t edge) {
    return edge == (uint8_t)ROOM_LINK_EDGE_LEFT ? (uint8_t)ROOM_LINK_EDGE_RIGHT
                                                : (uint8_t)ROOM_LINK_EDGE_LEFT;
}

typedef enum WriteResult {
    WRITE_NONE = 0,
    WRITE_STORED,
    WRITE_CONFLICT,
    WRITE_FULL,
} WriteResult;

static WriteResult write_one(LearnedLinks* links,
                             uint8_t area,
                             uint8_t room,
                             uint8_t edge,
                             uint8_t to_area,
                             uint8_t to_room) {
    for (uint32_t i = 0u; i < links->count; ++i) {
        LearnedLink* entry = &links->entries[i];
        if (entry->area != area || entry->room != room || entry->edge != edge) {
            continue;
        }
        if (entry->to_area != to_area || entry->to_room != to_room) {
            /* Seen before, and it led somewhere else. Keep the first answer and
             * say so; a margin that flips between two rooms reads as a defect. */
            links->conflicts++;
            return WRITE_CONFLICT;
        }
        return WRITE_NONE;
    }
    if (links->count >= LEARNED_LINKS_CAPACITY) {
        links->full++;
        return WRITE_FULL;
    }
    LearnedLink* entry = &links->entries[links->count++];
    entry->area = area;
    entry->room = room;
    entry->edge = edge;
    entry->to_area = to_area;
    entry->to_room = to_room;
    links->recorded++;
    return WRITE_STORED;
}

uint32_t learned_links_record(LearnedLinks* links,
                              uint8_t area,
                              uint8_t room,
                              uint8_t edge,
                              uint8_t to_area,
                              uint8_t to_room) {
    if (links == NULL) {
        return 0u;
    }
    if (edge != (uint8_t)ROOM_LINK_EDGE_LEFT && edge != (uint8_t)ROOM_LINK_EDGE_RIGHT) {
        return 0u;
    }
    if (area == to_area && room == to_room) {
        /* The same room reloaded (a checkpoint, a cutscene inside the room).
         * That is not a hand-off and says nothing about a neighbour. */
        return 0u;
    }
    const WriteResult crossed =
        write_one(links, area, room, edge, to_area, to_room);
    if (crossed == WRITE_CONFLICT || crossed == WRITE_FULL) {
        /* The observation is contradicted, or there is no room for it. Its
         * mirror is the same observation pointing the other way, so it is not
         * worth more trust than the half that just failed. A full table is not
         * counted twice either. */
        return 0u;
    }
    const WriteResult mirror = write_one(links, to_area, to_room, opposite_edge(edge), area, room);
    return (uint32_t)crossed + (mirror == WRITE_STORED ? 1u : 0u);
}

bool learned_links_lookup(const LearnedLinks* links,
                          uint8_t area,
                          uint8_t room,
                          uint8_t edge,
                          uint8_t* out_area,
                          uint8_t* out_room) {
    if (links == NULL || out_area == NULL || out_room == NULL) {
        return false;
    }
    for (uint32_t i = 0u; i < links->count; ++i) {
        const LearnedLink* entry = &links->entries[i];
        if (entry->area == area && entry->room == room && entry->edge == edge) {
            *out_area = entry->to_area;
            *out_room = entry->to_room;
            return true;
        }
    }
    return false;
}
