/* The room's tile map, read out of the cartridge.
 *
 * The atlas was the last thing standing between "we know where the room's data
 * is" and "we can draw the part of it the guest never draws". It is not a tile
 * bitmap. It is the room's cell -> tile map, and this module turns it into a
 * lookup the presenter can ask by world pixel.
 *
 * The chain, measured rather than inferred (docs/remaster-findings.md, the
 * 2026-10-19 addendum):
 *
 *   visual layer   one 16-bit word a cell, decompressed from the cartridge at
 *                  room load; the low 12 bits index the atlas
 *   atlas entry    8 bytes, four (tile id, attribute) pairs of a 2x2 metatile,
 *                  in the order the guest's own map writer (22:4001) lays them
 *                  down:
 *
 *                      [id00, id10, attr00, attr10, id01, id11, attr01, attr11]
 *
 *   geometry       index = cell_y * pitch + cell_x, cells relative to a fixed
 *                  origin of (256, 256) -- 00:18A6's dec h / dec d subtract
 *                  exactly 256 from each axis, unconditionally, so the layer's
 *                  first cell is world (256, 256) whatever the room's record
 *                  says. Its arguments are hl = Y and de = X, the reverse of
 *                  how they were first read.
 *
 *   pitch          the room header's byte 8 (`$CBC3`) selects it:
 *                  pitch = 16 << CBC3. 00:18A6 shifts the rounded Y by
 *                  `CBC3 + 1` but enters the loop at its `dec c`, so the
 *                  effective count is CBC3. Measured across the cartridge the
 *                  pitches are 16, 32 and 64, and every layer's cell count
 *                  divides its pitch exactly: 16x16, 16x32, 16x64, 32x8,
 *                  32x16, 32x32, 64x8 and 64x16, and nothing ragged.
 *
 *                  This is the answer to what the design notes called "the
 *                  $CBC1 paging question": there is no paging. A room is one
 *                  rectangular grid whose width is the pitch, and a room that
 *                  looked wider than 512 pixels was being read at the wrong
 *                  pitch. The record's four fields are the *walkable* box, not
 *                  the drawn room; the layer is the drawn room.
 *
 * Verified against the guest's live VRAM tile map for area 2 room 4: 198 of 198
 * cells with content match byte-for-byte in order, at two camera positions, and
 * the map is stable as the camera moves (570 of 570 world tiles keep their
 * (id, attribute) pair). tools/room_map.py is the reference and
 * tools/test_room_map.py drives both over the real cartridge.
 *
 * The tile id and the attribute are exactly what a CGB background wants: the id
 * selects a pattern, attribute bit 3 the VRAM bank, bits 0-2 the palette, bits 5
 * and 6 the flips, bit 7 the BG priority. So a renderer reads the pixels from
 * the VRAM patterns the guest already has resident and the *map* from here.
 *
 * The grid is the drawn room, and it is the honest extent: a caller asking for
 * a pixel outside it is asking for a pixel the layer does not have, and
 * room_map_cell says so instead of wrapping into the next row. Across the
 * cartridge 730 of the 737 rooms hold their whole camera reach inside their own
 * grid; the seven that do not are shorter than one screen, where the guest
 * itself is drawing past its layer.
 *
 * Observation only. Nothing here writes to the guest, and the ROM is read,
 * never written.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The two tables the loader uses, both read through the loader's bank. */
#define ROOM_MAP_TABLE_BANK 0x29u
#define ROOM_MAP_LAYER_TABLE_CPU 0x440Cu
#define ROOM_MAP_RECORD_TABLE_CPU 0x4492u

/* The per-room header table at $4366, read through the loader's bank: 16 bytes
 * a room, and the loader's own entry point (29:4001) reads it before the layer.
 * Byte 8 is the geometry selector ($CBC3). */
#define ROOM_MAP_HEADER_TABLE_CPU 0x4366u
#define ROOM_MAP_HEADER_BYTES 16u
#define ROOM_MAP_HEADER_GEOMETRY 8u
#define ROOM_MAP_HEADER_START_X 0u
#define ROOM_MAP_HEADER_START_Y 2u

/* The layer's cell grid. A visual layer is at most 0x800 bytes, so it holds at
 * most 1024 cells, and the width is 16 << CBC3. */
#define ROOM_MAP_CELLS 1024u
#define ROOM_MAP_CELL_PITCH_BASE 16u
#define ROOM_MAP_MAX_GEOMETRY 2u
#define ROOM_MAP_ENTRY_BYTES 8u
#define ROOM_MAP_ATLAS_MASK 0x0FFFu

/* The accurate window, used only to turn the record's camera limits into a cell
 * grid; the presenter may draw wider than this. Kept local so this module has
 * no dependency on the presentation ABI. */
#define ROOM_MAP_SCREEN_WIDTH 160
#define ROOM_MAP_SCREEN_HEIGHT 144

#define ROOM_MAP_CELL_PX 16

typedef struct RoomMap {
    bool valid;
    uint8_t area;
    uint8_t room;

    /* The layer's origin in world pixels, which is always (256, 256): the
     * guest's own cell arithmetic subtracts exactly that. The record's four
     * fields are the walkable box (X max, X min, Y min, Y max) and are kept for
     * the camera search, not for the geometry. */
    int32_t origin_x;
    int32_t origin_y;
    int32_t limit_min_x;
    int32_t limit_max_x;
    int32_t limit_min_y;
    int32_t limit_max_y;

    /* The drawn room: `cols` cells across (the pitch, 16 << CBC3) and `rows`
     * down (cell_count / pitch). This is the layer's own shape, not the
     * walkable box's. */
    uint16_t cols;
    uint16_t rows;

    /* The camera the header starts the room at, from its first two words. The
     * guest copies it straight into $FFB0..$FFB3 on entry. */
    int32_t start_x;
    int32_t start_y;

    /* The visual layer: one word a cell, low 12 bits an atlas index. */
    uint16_t cell_count;
    uint16_t cells[ROOM_MAP_CELLS];

    /* Where the atlas blob begins in the cartridge and how much of the image is
     * left from there. The blob has no length of its own; an index is in range
     * when its entry ends before the image does. */
    size_t atlas_offset;
    size_t atlas_bytes;
    uint16_t atlas_ptr;
    uint8_t atlas_bank;
    const uint8_t* rom;
    size_t rom_size;
} RoomMap;

/* Resolve and decode one room's map. Fails whole: a room whose record or visual
 * layer does not resolve leaves `out` invalid rather than half-filled, so a
 * caller cannot mistake a partial map for an answer. */
bool room_map_read(const uint8_t* rom,
                   size_t rom_size,
                   uint8_t switchable_bank,
                   uint8_t area,
                   uint8_t room,
                   RoomMap* out);

/* The eight atlas bytes of one cell, in the writer's order. `cx`/`cy` are cell
 * coordinates relative to (256, 256). Returns false outside the room's grid. */
bool room_map_cell(const RoomMap* map, uint16_t cx, uint16_t cy, uint8_t out[ROOM_MAP_ENTRY_BYTES]);

/* True when the cell exists but names no tile: every one of its four ids is 0.
 * That is the layer's padding, not a drawn cell - the grid is a rectangle and
 * the room inside it is not. Measured on area 1 room 1, whose authored content
 * stops at world x 640 while the grid runs to 767: every cell past it is all
 * zero, and painting one shows a flat band the room does not contain (a solid
 * ochre strip beside the jungle, 28 columns wide at $FFB0=478). This only
 * decides the revealed margin: the picture itself is the guest's own pixels,
 * where tile 0 is ordinary content. */
bool room_map_cell_blank(const RoomMap* map, uint16_t cx, uint16_t cy);

/* The (tile id, attribute) at a world pixel, by its 8x8 tile. Returns false
 * outside the room's grid, which is the caller's cue to fall back. */
bool room_map_tile(
    const RoomMap* map, int32_t world_x, int32_t world_y, uint8_t* out_id, uint8_t* out_attribute);

/* Overwrite the room's decoded visual cells from the guest's live WRAM bank 5
 * ($D000) copy so dynamic map changes (e.g. opened doors) appear in margins. */
void room_map_sync_live_cells(RoomMap* map, const uint8_t* wram_visual, size_t wram_bytes);
