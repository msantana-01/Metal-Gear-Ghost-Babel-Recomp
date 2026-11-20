/* See room_map.h for what this reads and how it was measured. */

#include "room_map.h"

#include <string.h>

#include "room_layers.h"
#include "room_record.h"

/* The layer's origin is not the record's minimum corner: 00:18A6 subtracts 256
 * from each axis unconditionally, so cell (0,0) is world (256,256) for every
 * room. Rooms whose walkable box starts later simply carry padding at the top
 * left, which is the room's own data and draws like any other cell. */
#define ROOM_MAP_ORIGIN 256

/* $CBC3 selects the grid's width: 00:18A6 shifts the rounded Y by CBC3+1 but
 * enters the loop at its `dec c`, so the effective count is CBC3 and the width
 * is 16 << CBC3. */
static bool cell_pitch_from_header(uint8_t geometry, uint16_t* out) {
    if (geometry > ROOM_MAP_MAX_GEOMETRY) {
        return false;
    }
    *out = (uint16_t)(ROOM_MAP_CELL_PITCH_BASE << geometry);
    return true;
}

bool room_map_read(const uint8_t* rom,
                   size_t rom_size,
                   uint8_t switchable_bank,
                   uint8_t area,
                   uint8_t room,
                   RoomMap* out) {
    if (rom == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    RoomRecord record;
    if (!room_record_read(rom,
                          rom_size,
                          switchable_bank,
                          ROOM_MAP_RECORD_TABLE_CPU,
                          area,
                          room,
                          &record) ||
        !record.valid) {
        return false;
    }

    /* A record that is not a rectangle is not a room this module can place. The
     * field limit rejects the erased 0xFFFF entries the table also contains. */
    const int32_t max_x = (int32_t)record.field[ROOM_FIELD_CAMERA_MAX_X];
    const int32_t min_x = (int32_t)record.field[ROOM_FIELD_CAMERA_MIN_X];
    const int32_t min_y = (int32_t)record.field[ROOM_FIELD_CAMERA_MIN_Y];
    const int32_t max_y = (int32_t)record.field[ROOM_FIELD_CAMERA_MAX_Y];
    if (max_x > (int32_t)0x0FFF || min_x > (int32_t)0x0FFF || min_y > (int32_t)0x0FFF ||
        max_y > (int32_t)0x0FFF || max_x < min_x || max_y < min_y) {
        return false;
    }

    uint8_t header[ROOM_MAP_HEADER_BYTES];
    if (!room_table_entry_read(rom,
                               rom_size,
                               switchable_bank,
                               ROOM_MAP_HEADER_TABLE_CPU,
                               area,
                               room,
                               ROOM_MAP_HEADER_BYTES,
                               header,
                               ROOM_MAP_HEADER_GEOMETRY + 2u,
                               NULL)) {
        return false;
    }
    uint16_t pitch = 0u;
    if (!cell_pitch_from_header(header[ROOM_MAP_HEADER_GEOMETRY], &pitch)) {
        return false;
    }

    RoomLayerBlock visual;
    if (!room_layer_block_read(rom,
                               rom_size,
                               switchable_bank,
                               ROOM_MAP_LAYER_TABLE_CPU,
                               area,
                               room,
                               ROOM_LAYER_SLOT_VISUAL,
                               &visual)) {
        return false;
    }

    size_t stream_offset = 0u;
    if (!room_record_cpu_to_rom(visual.ptr, visual.bank, rom, rom_size, &stream_offset)) {
        return false;
    }

    /* Decompress into bytes and assemble the words by hand: the layer is a
     * little-endian page in the guest's address space, and reading it through a
     * cast would make the result depend on the host's endianness and alias the
     * buffer. */
    uint8_t page[ROOM_LAYER_MAX_BYTES];
    size_t length = 0u;
    if (room_layer_decompress(rom,
                              rom_size,
                              stream_offset,
                              ROOM_LAYER_PAGE_VISUAL,
                              page,
                              sizeof(page),
                              &length) != ROOM_DECOMPRESS_OK) {
        return false;
    }
    if (length < 2u || (length % 2u) != 0u || length > sizeof(page)) {
        return false;
    }

    RoomLayerBlock atlas;
    if (!room_layer_block_read(rom,
                               rom_size,
                               switchable_bank,
                               ROOM_MAP_LAYER_TABLE_CPU,
                               area,
                               room,
                               ROOM_LAYER_SLOT_ATLAS,
                               &atlas)) {
        return false;
    }
    size_t atlas_offset = 0u;
    if (!room_record_cpu_to_rom(atlas.ptr, atlas.bank, rom, rom_size, &atlas_offset)) {
        return false;
    }

    /* The layer's cell count has to divide its pitch, or the geometry byte and
     * the layer disagree and neither can be trusted. Across the cartridge every
     * room divides exactly; refusing here keeps a ragged room from drawing a
     * grid that is actually two rows shorter than it claims. */
    const size_t cell_count = length / 2u;
    if (cell_count == 0u || cell_count > ROOM_MAP_CELLS || (cell_count % pitch) != 0u) {
        return false;
    }
    const size_t rows = cell_count / pitch;
    if (rows == 0u || rows > 0xFFFFu) {
        return false;
    }

    out->cell_count = (uint16_t)cell_count;
    for (uint16_t i = 0u; i < out->cell_count; i++) {
        out->cells[i] = (uint16_t)((uint16_t)page[2u * i] | ((uint16_t)page[2u * i + 1u] << 8));
    }
    out->atlas_offset = atlas_offset;
    out->atlas_bytes = rom_size - atlas_offset;
    out->atlas_ptr = atlas.ptr;
    out->atlas_bank = atlas.bank;
    out->rom = rom;
    out->rom_size = rom_size;
    out->origin_x = ROOM_MAP_ORIGIN;
    out->origin_y = ROOM_MAP_ORIGIN;
    out->limit_min_x = min_x;
    out->limit_max_x = max_x;
    out->limit_min_y = min_y;
    out->limit_max_y = max_y;
    out->cols = pitch;
    out->rows = (uint16_t)rows;
    out->start_x = (int32_t)((uint16_t)header[ROOM_MAP_HEADER_START_X] |
                             ((uint16_t)header[ROOM_MAP_HEADER_START_X + 1u] << 8));
    out->start_y = (int32_t)((uint16_t)header[ROOM_MAP_HEADER_START_Y] |
                             ((uint16_t)header[ROOM_MAP_HEADER_START_Y + 1u] << 8));
    out->area = area;
    out->room = room;
    out->valid = true;
    return true;
}

bool room_map_cell(const RoomMap* map, uint16_t cx, uint16_t cy, uint8_t out[ROOM_MAP_ENTRY_BYTES]) {
    if (map == NULL || !map->valid || out == NULL) {
        return false;
    }
    /* Inside the room's grid, which is the drawn room. A caller asking past it
     * is asking for a pixel the layer does not have; wrapping into the next row
     * would answer with a different part of the room. */
    if (cx >= map->cols || cy >= map->rows) {
        return false;
    }
    const uint32_t index = (uint32_t)cy * (uint32_t)map->cols + (uint32_t)cx;
    if (index >= (uint32_t)map->cell_count) {
        return false;
    }
    const size_t start =
        map->atlas_offset + (size_t)(map->cells[index] & ROOM_MAP_ATLAS_MASK) * ROOM_MAP_ENTRY_BYTES;
    if (map->atlas_bytes < ROOM_MAP_ENTRY_BYTES || start > map->rom_size - ROOM_MAP_ENTRY_BYTES) {
        return false;
    }
    memcpy(out, &map->rom[start], ROOM_MAP_ENTRY_BYTES);
    return true;
}

bool room_map_tile(
    const RoomMap* map, int32_t world_x, int32_t world_y, uint8_t* out_id, uint8_t* out_attribute) {
    if (map == NULL || !map->valid || out_id == NULL || out_attribute == NULL) {
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

    uint8_t entry[ROOM_MAP_ENTRY_BYTES];
    if (!room_map_cell(map, (uint16_t)cx, (uint16_t)cy, entry)) {
        return false;
    }

    /* Row-major inside the cell: (0,0) (1,0) (0,1) (1,1), with the ids in the
     * first and third pair and the attributes in the second and fourth. */
    const uint32_t tile = ((uint32_t)(local_y % ROOM_MAP_CELL_PX) / 8u) * 2u +
                          ((uint32_t)(local_x % ROOM_MAP_CELL_PX) / 8u);
    *out_id = entry[(tile / 2u) * 4u + (tile % 2u)];
    *out_attribute = entry[(tile / 2u) * 4u + 2u + (tile % 2u)];
    return true;
}

bool room_map_cell_blank(const RoomMap* map, uint16_t cx, uint16_t cy) {
    uint8_t entry[ROOM_MAP_ENTRY_BYTES];
    if (!room_map_cell(map, cx, cy, entry)) {
        return false;
    }
    /* The whole entry, not just the four ids. A cell of tile id 0 is a tile the
     * room draws, not a hole: on CGB id 0 is colour 0 of the attribute's
     * palette, and this is how the cartridge paints flat surfaces. Area 1
     * room 1's tan wall is 00 00 04 04 00 00 04 04 behind the panel, and
     * reading the ids alone letterboxed 24 columns of wall into a black stripe
     * (ADR-011). Only the padding is all zero. */
    for (size_t i = 0; i < ROOM_MAP_ENTRY_BYTES; ++i) {
        if (entry[i] != 0u) {
            return false;
        }
    }
    return true;
}

void room_map_sync_live_cells(RoomMap* map, const uint8_t* wram_visual, size_t wram_bytes) {
    if (map == NULL || !map->valid || wram_visual == NULL ||
        wram_bytes < (size_t)map->cell_count * 2u) {
        return;
    }
    for (uint16_t i = 0u; i < map->cell_count; ++i) {
        map->cells[i] =
            (uint16_t)((uint16_t)wram_visual[2u * i] | ((uint16_t)wram_visual[2u * i + 1u] << 8));
    }
}
