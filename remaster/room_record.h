/* Reading the room's own record out of the cartridge, the way the guest does.
 *
 * The widescreen margin is the weakest thing about the presenter, and the reason
 * is that it has to invent it: the guest draws 160 columns and the margin is
 * reconstructed from what the guest left behind in VRAM or from what the PPU
 * already drew. Both sources are extrapolations, and the three defects that
 * brought this file into being are what extrapolating costs.
 *
 * The room itself is not an extrapolation. Ghost Babel keeps a table of rooms in
 * the cartridge, indexed by area and room, and the loader writes four 16-bit
 * fields out of it on every room change. Two of those are the camera limits the
 * presenter already trusts; the other two have never been read. If the room's
 * record turns out to describe the room rather than just its camera, the margin
 * stops being a guess and becomes data - which is the whole question this module
 * exists to answer.
 *
 * The addressing, from the disassembly (asm/full/bank029.asm), is:
 *
 *   29:42C5  push bc
 *           ld a, [$CBC0]      ; area
 *           and $7F
 *           call $0551         ; hl = read16(hl + area * 2)
 *           ld a, [$CBC1]      ; room
 *           call $0534         ; hl = hl + room * 8
 *           pop bc
 *           add hl, bc         ; + the field offset the caller asked for
 *           ld a, [hl+]
 *           ld h, [hl]
 *           ld l, a
 *           ret
 *
 *   00:0551  ld b, $00 / add a,a / jr nc,+3 / inc b / ld c,a / add hl,bc
 *           ld a,[hl+] / ld h,[hl] / ld l,a      -> pointer table, stride 2
 *   00:0534  ld b,h / ld c,l / ld l,a / ld h,$00 / add hl,hl x3 / add hl,bc
 *                                              -> base + index * 8
 *
 * The loader at 29:410a asks for offsets 0, 2, 4 and 6 and writes the four words
 * to $CC0B/$CC0C, $CC0D/$CC0E, $CC0F/$CC10 and $CC11/$CC12. All four are the
 * camera's limits - the rectangle the camera may move in, in world pixels:
 *
 *   offset 0 -> $CC0B  camera's right limit in X
 *   offset 2 -> $CC0D  camera's left limit in X
 *   offset 4 -> $CC0F  camera's top limit in Y
 *   offset 6 -> $CC11  camera's bottom limit in Y
 *
 * The X pair was read first and is confirmed by the guest's own scroll code.
 * The Y pair is confirmed the same way: 08:4839 far-calls 29:42B7 (which returns
 * the word at $CC0F) and, with the camera in $FFB2/$FFB3, calls 22:43D8 -
 * the handler that decrements $FFB2. 08:486A far-calls 29:42BE ($CC11) and calls
 * 22:4412, which increments it. A limit the camera is allowed to walk down to
 * cannot be the far end, so $CC0F is the top and $CC11 the bottom.
 *
 * The same three bytes are the room's extent: with the camera range [min, max],
 * the room's world is [min_x, max_x + 159] x [min_y, max_y + 143]. Every room in
 * the cartridge was swept with tools/room_table_report.py: 747 rooms over 67
 * areas, X spans to 632 pixels and Y spans to 640, so a room is up to five
 * screens each way and the guest's 160x144 window shows a corner of it.
 *
 * Observation only. Nothing here writes to the guest, and the ROM is read, never
 * written.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Four 16-bit fields. */
#define ROOM_RECORD_FIELDS 4u
#define ROOM_RECORD_BYTES (ROOM_RECORD_FIELDS * 2u)

/* Field indices, as the loader numbers them by byte offset. */
#define ROOM_FIELD_CAMERA_MAX_X 0u
#define ROOM_FIELD_CAMERA_MIN_X 1u
#define ROOM_FIELD_CAMERA_MIN_Y 2u
#define ROOM_FIELD_CAMERA_MAX_Y 3u

typedef struct RoomRecord {
    bool valid;
    uint8_t area;
    uint8_t room;
    /* The guest CPU address the record resolved to, before it is translated to
     * a cartridge offset. Kept because it is the number to check against the
     * disassembly when the fields do not make sense. */
    uint16_t cpu_addr;
    uint8_t raw[ROOM_RECORD_BYTES];
    uint16_t field[ROOM_RECORD_FIELDS];
} RoomRecord;

/* A guest CPU address to a cartridge offset, or false when it is outside.
 *
 * A CPU address below 0x4000 is the fixed bank and needs no bank register; from
 * 0x4000 up it is whatever the bank register is holding, which for the room
 * table is the loader's own bank. Rejecting an out-of-range offset instead of
 * reading past the image is deliberate: a wrong area index must read as "not
 * resolved", never as a plausible number from the wrong place. */
bool room_record_cpu_to_rom(uint16_t cpu_addr,
                            uint8_t switchable_bank,
                            const uint8_t* rom,
                            size_t rom_size,
                            size_t* out_offset);

/* Resolve one room's entry out of any per-room table shaped like the loader's:
 * a 16-bit pointer table indexed by area (00:0551), then a run of fixed-size
 * entries indexed by room. The record table is one such table (stride 8); the
 * room header table at $4366 is another (stride 16). Keeping the addressing in
 * one place means the next table found in the loader costs a call, not a second
 * derivation.
 *
 * `out` receives `out_len` bytes of the entry; `out_cpu_addr` (optional) the
 * guest address they came from. Fails whole, like the record reader: a
 * partially read entry is a plausible lie. */
bool room_table_entry_read(const uint8_t* rom,
                           size_t rom_size,
                           uint8_t switchable_bank,
                           uint16_t table_cpu_addr,
                           uint8_t area,
                           uint8_t room,
                           uint16_t stride,
                           uint8_t* out,
                           size_t out_len,
                           uint16_t* out_cpu_addr);

/* Resolve one room's record. `table_cpu_addr` is where the loader keeps its
 * pointer table ($4492 in bank $29); `switchable_bank` is the bank that address
 * is read through. Returns false when any step lands outside the cartridge. */
bool room_record_read(const uint8_t* rom,
                      size_t rom_size,
                      uint8_t switchable_bank,
                      uint16_t table_cpu_addr,
                      uint8_t area,
                      uint8_t room,
                      RoomRecord* out);
