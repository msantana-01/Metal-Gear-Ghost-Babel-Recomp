/* Reading a room's own layers out of the cartridge, the way the guest does.
 *
 * room_record.h explained why this project needs the room's own data: the
 * margin is either data or it is an invention, and inventing it is what the
 * three defects (a column cache carrying a frozen copy, a guard that fades and
 * pops, a codec inheriting the previous room) are made of. The record gave the
 * camera's rectangle. This module goes after the room itself.
 *
 * At every room change the guest decompresses the room out of the cartridge
 * into three WRAM pages, and the map the streamer later walks column by column
 * is exactly that decompressed data - not something it derives as it walks.
 * The chain, from the disassembly (asm/full/bank029.asm unless marked):
 *
 *   29:410A  the room loader
 *   29:42C5  record resolver: $0551(area) -> base, then base + room*8 + field
 *   29:42EC  block resolver: the same shape with a stride of 12, returning a
 *            3-byte record - a pointer, then the ROM bank to read it through
 *   29:430C  layer 0, decompressed into $D000 with WRAM bank 5 selected
 *   29:4356  layer 2 -> $D000 with bank 4    (behaviour: a 16-bit word a cell)
 *   29:435E  layer 3 -> $D800 with bank 5    (attributes: four nibbles a cell)
 *   slot +3  the atlas pointer, which the loader stores at $CBFF untouched -
 *            it is a literal 3-byte pointer, not a stream
 *   29:41E8  the expander that copies a decompressed block into a layer page
 *   00:16C0  the bank-switching wrapper
 *   00:0E29  the decompressor
 *
 * The decompressor is a marker-byte LZ. Its header is a 16-bit output length
 * followed by the byte that marks a match; then:
 *
 *   token != marker   one literal byte
 *   token == marker   delta, control
 *                     run    = (control & 0x0F) + 4
 *                     offset = ((control >> 4) & 0x0F) * 256 + delta
 *                     source = the destination start + offset
 *
 * A run of 0x13 is the escape: one more byte follows, added to 0x13. The source
 * must land inside [start, start+length); at or past the end of that range the
 * guest rewinds it by 0x1000 - a WRAM page - and where that lands before the
 * start it writes a zero byte instead of reading (00:0E98, re-run for every
 * copied byte, which is what makes overlapping runs work).
 *
 * A source below the start is the guest reading its own workspace, and this
 * module refuses it rather than inventing bytes: past that point the layer is
 * no longer a function of the cartridge alone.
 *
 * Verified, not assumed. For the room both savestates sit in (area 2, room 4)
 * all three layers decode to 2048 bytes and equal the live WRAM page byte for
 * byte, and the atlas record equals the live $CBFF triple. tools/room_layers.py
 * is the same decoder with a --wram switch that repeats that check, and its
 * --sweep covers the cartridge: every one of the 737 rooms whose record is a
 * usable rectangle decodes.
 *
 * Observation only. Nothing here writes to the guest, and the ROM is read,
 * never written.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The loader resolves each layer with 00:16C0(bank, CPU address). The bank is
 * the loader's own, as it is for the record table. */
#define ROOM_LAYER_TABLE_BANK 0x29u

/* The block list the loader reads: 29:440C, stride 12, 3-byte records. It is
 * not the same table as the record's ($4492, stride 8): two views of the room,
 * each with its own entry size. */
#define ROOM_LAYER_TABLE_CPU 0x440Cu
#define ROOM_LAYER_ENTRY_BYTES 12u
#define ROOM_LAYER_RECORD_BYTES 3u

/* Slots inside a room's 12-byte entry, in the order the loader uses them. */
#define ROOM_LAYER_SLOT_VISUAL 0u
#define ROOM_LAYER_SLOT_ATLAS 3u
#define ROOM_LAYER_SLOT_BEHAVIOUR 6u
#define ROOM_LAYER_SLOT_ATTRIBUTES 9u

/* The three layers this module decodes, in the loader's order. */
#define ROOM_LAYER_VISUAL 0u
#define ROOM_LAYER_BEHAVIOUR 1u
#define ROOM_LAYER_ATTRIBUTES 2u
#define ROOM_LAYER_KINDS 3u

/* A layer is a power-of-two cell grid. The three sizes the cartridge uses are
 * 512, 1024 and 2048 bytes, so a page of 0x800 bytes is the largest. */
#define ROOM_LAYER_MAX_BYTES 0x800u

/* The decompressor's page: the destination the guest passes in hl. The visual
 * and behaviour layers take the bottom of a WRAM page, the attributes the top
 * half of bank 5's. */
#define ROOM_LAYER_PAGE_VISUAL 0xD000u
#define ROOM_LAYER_PAGE_BEHAVIOUR 0xD000u
#define ROOM_LAYER_PAGE_ATTRIBUTES 0xD800u

/* What a decode did. Named so the presenter can tell "this room's layers are
 * data" from "these bytes are not a layer", which is the difference between an
 * answered question and a plausible-looking failure. */
typedef enum RoomDecompressStatus {
    ROOM_DECOMPRESS_OK = 0,
    /* The header, or a token the header called for, is past the image. */
    ROOM_DECOMPRESS_TRUNCATED,
    /* Not a layer: a zero length, or one larger than any layer the guest loads. */
    ROOM_DECOMPRESS_IMPLAUSIBLE,
    /* A match that would read outside [start, start+length). The guest would
     * read its own workspace there, so the result is not the room's data. */
    ROOM_DECOMPRESS_OUTSIDE_PAGE,
} RoomDecompressStatus;

/* One 3-byte block record: a pointer and the bank to read it through. */
typedef struct RoomLayerBlock {
    bool valid;
    uint16_t cpu_addr; /* where the record itself was read from */
    uint16_t ptr;      /* the pointer the record holds */
    uint8_t bank;      /* the ROM bank that pointer is read through */
} RoomLayerBlock;

/* A room's three layers, decoded. */
typedef struct RoomLayers {
    bool valid;
    uint8_t area;
    uint8_t room;
    /* The atlas triple, as the loader stores it at $CBFF: a pointer and a bank,
     * not a stream. */
    uint16_t atlas_ptr;
    uint8_t atlas_bank;
    /* Decoded size of each layer, in bytes. Visual and attributes match in
     * every room swept; behaviour is the one that can differ. */
    uint16_t bytes[ROOM_LAYER_KINDS];
    uint8_t page[ROOM_LAYER_KINDS][ROOM_LAYER_MAX_BYTES];
} RoomLayers;

/* The guest's 00:0E29, ported.
 *
 * `offset` is where the stream starts in the cartridge, `start` the absolute
 * destination address the guest passes in hl, and `out`/`out_capacity` the
 * buffer standing for that page. On success `out_length` receives the number of
 * bytes written, which is the length the stream declared. */
RoomDecompressStatus room_layer_decompress(const uint8_t* rom,
                                           size_t rom_size,
                                           size_t offset,
                                           uint16_t start,
                                           uint8_t* out,
                                           size_t out_capacity,
                                           size_t* out_length);

/* Resolve one 3-byte block record: `table_cpu_addr` + area*2 is the area's
 * entry, and its value plus room*12 plus `slot` is the record. */
bool room_layer_block_read(const uint8_t* rom,
                           size_t rom_size,
                           uint8_t switchable_bank,
                           uint16_t table_cpu_addr,
                           uint8_t area,
                           uint8_t room,
                           uint8_t slot,
                           RoomLayerBlock* out);

/* Resolve and decode a room's three layers. The atlas slot is looked up too and
 * stored as the triple it is.
 *
 * Fails whole: a room whose atlas record or any one of the three layers does
 * not resolve leaves `out` invalid rather than half-filled. A caller that gets
 * false has to fall back, and a half-filled RoomLayers would make that call
 * look like it had succeeded. */
bool room_layers_read(const uint8_t* rom,
                      size_t rom_size,
                      uint8_t switchable_bank,
                      uint16_t table_cpu_addr,
                      uint8_t area,
                      uint8_t room,
                      RoomLayers* out);

/* The status of the layer that failed, for a caller that wants to say why. Only
 * meaningful after room_layers_read returned false with a valid-looking room. */
const char* room_decompress_status_name(RoomDecompressStatus status);
