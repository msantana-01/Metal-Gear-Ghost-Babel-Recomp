/* See room_links.h for the table layout and why each coordinate is a page tile. */

#include "room_links.h"

#include <string.h>

#include "room_record.h"

static bool read_u16(const uint8_t* rom, size_t rom_size, uint8_t bank, uint16_t cpu_addr,
                     uint16_t* out) {
    size_t offset = 0u;
    if (!room_record_cpu_to_rom(cpu_addr, bank, rom, rom_size, &offset)) {
        return false;
    }
    if (rom_size - offset < 2u) {
        return false;
    }
    *out = (uint16_t)(rom[offset] | ((uint16_t)rom[offset + 1u] << 8));
    return true;
}

static bool read_u8(const uint8_t* rom, size_t rom_size, uint8_t bank, uint16_t cpu_addr,
                    uint8_t* out) {
    size_t offset = 0u;
    if (!room_record_cpu_to_rom(cpu_addr, bank, rom, rom_size, &offset)) {
        return false;
    }
    if (rom_size - offset < 1u) {
        return false;
    }
    *out = rom[offset];
    return true;
}

/* 00:0551 applied twice: area table -> room table -> the room's own array.
 * Both pointers have to name the switchable window; a word that lands in the
 * fixed bank is a stale table entry, not a second table. */
static bool table_pointer(const uint8_t* rom,
                          size_t rom_size,
                          uint16_t table_cpu,
                          uint8_t area,
                          uint8_t room,
                          uint16_t* out) {
    uint16_t area_table = 0u;
    if (!read_u16(rom, rom_size, ROOM_LINK_BANK, (uint16_t)(table_cpu + (uint16_t)area * 2u),
                  &area_table)) {
        return false;
    }
    if (area_table < 0x4000u || area_table >= 0x8000u) {
        return false;
    }
    const uint32_t room_addr = (uint32_t)area_table + (uint32_t)room * 2u;
    if (room_addr > 0x7FFFu) {
        /* The room hop would run past the switchable window; the guest's 16-bit
         * add wraps into the fixed bank, so the reference refuses rather than
         * reads a different table than the one the cartridge laid out. */
        return false;
    }
    uint16_t room_table = 0u;
    if (!read_u16(rom, rom_size, ROOM_LINK_BANK, (uint16_t)room_addr, &room_table)) {
        return false;
    }
    if (room_table < 0x4000u || room_table >= 0x8000u) {
        return false;
    }
    *out = room_table;
    return true;
}

/* How many areas the door table in $4CB3 describes.
 *
 * The leading run of in-window pointers is not the answer: the room tables for
 * the first areas follow immediately and their pointers are in the window too,
 * so the run keeps going (it counted 26 - 16 areas plus ten room pointers of
 * area 0). The table cannot point into itself, and this cartridge puts the first
 * area's room table exactly at its end, so area 0's pointer names the end and the
 * span between them is the count. Every entry is then required to be a pointer in
 * the window, or this is not an area table. */
static uint8_t transition_area_count(const uint8_t* rom, size_t rom_size) {
    uint16_t first = 0u;
    if (!read_u16(rom, rom_size, ROOM_LINK_BANK, ROOM_LINK_TRANSITION_TABLE_CPU, &first)) {
        return 0u;
    }
    if (first < ROOM_LINK_TRANSITION_TABLE_CPU + 2u) {
        return 0u;
    }
    const uint32_t span = (uint32_t)first - ROOM_LINK_TRANSITION_TABLE_CPU;
    if ((span % 2u) != 0u || span > 256u) {
        return 0u;
    }
    const uint32_t count = span / 2u;
    for (uint32_t i = 0u; i < count; i++) {
        uint16_t pointer = 0u;
        if (!read_u16(rom, rom_size, ROOM_LINK_BANK,
                      (uint16_t)(ROOM_LINK_TRANSITION_TABLE_CPU + 2u * i), &pointer) ||
            pointer < 0x4000u || pointer >= 0x8000u) {
            return 0u;
        }
    }
    return (uint8_t)count;
}

/* How many rooms an area's room table names.
 *
 * Same derivation one level down: the run of in-window room pointers is followed
 * by the entries themselves, whose bytes are code or transition data, not
 * pointers. This cartridge has areas with 10 and with 13 rooms, so a room past
 * the count is "no entry", not "a degenerate pointer". A zero means the area's
 * pointer does not name a room table at all - area 15's names the door
 * resolver's own code. */
static uint8_t transition_room_count(const uint8_t* rom, size_t rom_size, uint8_t area) {
    uint16_t area_table = 0u;
    if (!read_u16(rom, rom_size, ROOM_LINK_BANK,
                  (uint16_t)(ROOM_LINK_TRANSITION_TABLE_CPU + 2u * area), &area_table)) {
        return 0u;
    }
    if (area_table < 0x4000u || area_table >= 0x8000u) {
        return 0u;
    }
    uint8_t rooms = 0u;
    while (rooms < 16u) {
        const uint32_t addr = (uint32_t)area_table + 2u * rooms;
        if (addr >= 0x8000u) {
            break;
        }
        uint16_t pointer = 0u;
        if (!read_u16(rom, rom_size, ROOM_LINK_BANK, (uint16_t)addr, &pointer) ||
            pointer < 0x4000u || pointer >= 0x8000u) {
            break;
        }
        rooms++;
    }
    return rooms;
}

static bool node_read(const uint8_t* rom,
                      size_t rom_size,
                      uint8_t area,
                      uint8_t room,
                      uint8_t index,
                      uint8_t out[ROOM_LINK_NODE_BYTES]) {
    uint16_t base = 0u;
    if (!table_pointer(rom, rom_size, ROOM_LINK_NODE_TABLE_CPU, area, room, &base)) {
        return false;
    }
    const uint32_t addr = (uint32_t)base + (uint32_t)index * ROOM_LINK_NODE_BYTES;
    if (addr > 0x7FFFu) {
        return false;
    }
    for (uint8_t i = 0u; i < ROOM_LINK_NODE_BYTES; i++) {
        if (!read_u8(rom, rom_size, ROOM_LINK_BANK, (uint16_t)(addr + i), &out[i])) {
            return false;
        }
    }
    return true;
}

static int16_t coordinate(uint8_t value) {
    /* A page tile is 0..31. $FF is the explicit unset marker, and a value above
     * 31 is a shared-table entry that is not a position the PPU can address. */
    return value <= 31u ? (int16_t)value : (int16_t)-1;
}

uint32_t room_links_read(const uint8_t* rom,
                         size_t rom_size,
                         uint8_t area,
                         uint8_t room,
                         RoomLink out[ROOM_LINK_COUNT]) {
    if (rom == NULL || out == NULL) {
        return 0u;
    }
    memset(out, 0, sizeof(RoomLink) * ROOM_LINK_COUNT);

    /* An area the table does not describe, and a room the area does not have,
     * are refused before the hop. Without this the guest's 16-bit add would read
     * whatever follows the table and call it a transition. */
    if (area >= transition_area_count(rom, rom_size) ||
        room >= transition_room_count(rom, rom_size, area)) {
        return 0u;
    }
    uint16_t transition_base = 0u;
    if (!table_pointer(rom, rom_size, ROOM_LINK_TRANSITION_TABLE_CPU, area, room,
                       &transition_base)) {
        return 0u;
    }

    for (uint8_t index = 0u; index < ROOM_LINK_COUNT; index++) {
        const uint32_t entry = (uint32_t)transition_base + (uint32_t)index * ROOM_LINK_ENTRY_BYTES;
        if (entry > 0x7FFFu) {
            memset(out, 0, sizeof(RoomLink) * ROOM_LINK_COUNT);
            return 0u;
        }
        uint8_t raw[ROOM_LINK_ENTRY_BYTES] = {0u, 0u, 0u, 0u};
        for (uint8_t i = 0u; i < ROOM_LINK_ENTRY_BYTES; i++) {
            if (!read_u8(rom, rom_size, ROOM_LINK_BANK, (uint16_t)(entry + i), &raw[i])) {
                memset(out, 0, sizeof(RoomLink) * ROOM_LINK_COUNT);
                return 0u;
            }
        }
        uint8_t source[ROOM_LINK_NODE_BYTES] = {0u};
        if (!node_read(rom, rom_size, area, room, index, source)) {
            memset(out, 0, sizeof(RoomLink) * ROOM_LINK_COUNT);
            return 0u;
        }

        out[index].node = index;
        out[index].dst_area = raw[0];
        out[index].dst_room = raw[1];
        out[index].dst_node = raw[2];
        out[index].kind = raw[3];
        out[index].src_x = coordinate(source[0]);
        out[index].src_y = coordinate(source[1]);
        out[index].dst_x = -1;
        out[index].dst_y = -1;

        /* The shared tables run past the last room. A destination that does not
         * name a real area/room/node has no position; leave it unplaced rather
         * than reading a record from another bank. */
        if (out[index].dst_area < 67u && out[index].dst_room < 16u &&
            out[index].dst_node < ROOM_LINK_COUNT) {
            uint8_t target[ROOM_LINK_NODE_BYTES] = {0u};
            if (node_read(rom, rom_size, out[index].dst_area, out[index].dst_room,
                          out[index].dst_node, target)) {
                out[index].dst_x = coordinate(target[0]);
                out[index].dst_y = coordinate(target[1]);
            }
        }
    }
    return ROOM_LINK_COUNT;
}

int32_t room_link_page_screen(int32_t page_tile, int32_t camera) {
    const int32_t low = camera & 0xFF;
    return ((page_tile * 8 - low) & 0xFF);
}

RoomLinkEdge room_link_edge(const RoomLink* link, int32_t start_x, int32_t start_y) {
    if (link == NULL || link->src_x < 0 || link->src_y < 0) {
        return ROOM_LINK_EDGE_NONE;
    }
    const int32_t sx = room_link_page_screen(link->src_x, start_x);
    const int32_t sy = room_link_page_screen(link->src_y, start_y);
    const int32_t dl = sx < 0 ? -sx : sx;
    const int32_t dr = sx > ROOM_LINK_SCREEN_WIDTH ? sx - ROOM_LINK_SCREEN_WIDTH
                                                   : ROOM_LINK_SCREEN_WIDTH - sx;
    const int32_t dt = sy < 0 ? -sy : sy;
    const int32_t db = sy > ROOM_LINK_SCREEN_HEIGHT ? sy - ROOM_LINK_SCREEN_HEIGHT
                                                    : ROOM_LINK_SCREEN_HEIGHT - sy;
    int32_t best = dl;
    RoomLinkEdge edge = ROOM_LINK_EDGE_LEFT;
    if (dr < best) {
        best = dr;
        edge = ROOM_LINK_EDGE_RIGHT;
    }
    if (dt < best) {
        best = dt;
        edge = ROOM_LINK_EDGE_TOP;
    }
    if (db < best) {
        best = db;
        edge = ROOM_LINK_EDGE_BOTTOM;
    }
    /* A door more than a screen away from every edge is an interior transition;
     * it cannot fill a margin. */
    if (best > ROOM_LINK_SCREEN_WIDTH) {
        return ROOM_LINK_EDGE_NONE;
    }
    return edge;
}

bool room_link_place(const RoomLink* link,
                     const RoomLinkGeometry* current,
                     const RoomLinkGeometry* neighbour,
                     RoomLinkEdge edge,
                     RoomLinkPlacement* out) {
    if (link == NULL) {
        return false;
    }
    if (link->src_x < 0 || link->src_y < 0) {
        return false;
    }
    return room_link_place_edge(current, neighbour, edge, out);
}

bool room_link_place_edge(const RoomLinkGeometry* current,
                          const RoomLinkGeometry* neighbour,
                          RoomLinkEdge edge,
                          RoomLinkPlacement* out) {
    if (current == NULL || neighbour == NULL || out == NULL) {
        return false;
    }
    if (edge != ROOM_LINK_EDGE_LEFT && edge != ROOM_LINK_EDGE_RIGHT) {
        return false;
    }

    if (edge == ROOM_LINK_EDGE_RIGHT) {
        out->origin_x = current->origin_x + current->cols * 16 - neighbour->start_x;
    } else {
        out->origin_x =
            current->origin_x - neighbour->start_x - ROOM_LINK_PLAYFIELD_WIDTH;
    }
    /* The guest does not scroll from one room into the next: when the player
     * crosses the limit it cuts to the destination room's own header camera
     * (observed 2:4 -> 2:3: camera y 600 -> 632, the header's 632, not the
     * destination node's 848). Aligning the two start views is therefore the
     * scene the player is about to see; aligning the door with the node entry
     * would shift the destination's view out of the margin. */
    out->origin_y = current->start_y - neighbour->start_y;
    out->edge = edge;

    /* The presenter mirrors tiles with the world position's low three bits.
     * Keeping the placement on the 8-pixel grid keeps those bits identical to
     * the neighbour's own frame. */
    out->origin_x &= ~7;
    out->origin_y &= ~7;
    return true;
}
