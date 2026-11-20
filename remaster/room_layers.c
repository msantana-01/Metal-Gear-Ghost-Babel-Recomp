#include "room_layers.h"

#include <string.h>

#include "room_record.h"

/* The layer slots, in the order the loader resolves them, paired with the page
 * each one is decompressed into. The atlas slot shares the table but is a
 * pointer, so it is read on its own below. */
static const uint8_t k_slot[ROOM_LAYER_KINDS] = {
    ROOM_LAYER_SLOT_VISUAL,
    ROOM_LAYER_SLOT_BEHAVIOUR,
    ROOM_LAYER_SLOT_ATTRIBUTES,
};

static const uint16_t k_page_start[ROOM_LAYER_KINDS] = {
    ROOM_LAYER_PAGE_VISUAL,
    ROOM_LAYER_PAGE_BEHAVIOUR,
    ROOM_LAYER_PAGE_ATTRIBUTES,
};

const char* room_decompress_status_name(RoomDecompressStatus status) {
    switch (status) {
    case ROOM_DECOMPRESS_OK:
        return "ok";
    case ROOM_DECOMPRESS_TRUNCATED:
        return "truncated";
    case ROOM_DECOMPRESS_IMPLAUSIBLE:
        return "implausible";
    case ROOM_DECOMPRESS_OUTSIDE_PAGE:
        return "outside-page";
    }
    return "unknown";
}

/* 00:0E98..0ECA: keep a match's source inside [start, start+length).
 *
 * The guest compares the high byte first and only then the low one, and on the
 * way back reserves one case for writing a zero byte rather than reading. The
 * result is a source that is either inside the page or explicitly a zero, never
 * a read of memory the layer does not own. */
static bool room_layer_window(uint32_t* source, uint16_t start, uint16_t end, bool* zero) {
    if (*source >= (uint32_t)end) {
        *source = (*source - 0x1000u) & 0xFFFFu;
        if (*source < (uint32_t)start) {
            *source = (*source + 0x1000u) & 0xFFFFu;
            *zero = true;
            return true;
        }
    }
    *zero = false;
    return *source >= (uint32_t)start;
}

RoomDecompressStatus room_layer_decompress(const uint8_t* rom,
                                           size_t rom_size,
                                           size_t offset,
                                           uint16_t start,
                                           uint8_t* out,
                                           size_t out_capacity,
                                           size_t* out_length) {
    if (rom == NULL || out == NULL || out_length == NULL || out_capacity == 0u) {
        return ROOM_DECOMPRESS_TRUNCATED;
    }
    *out_length = 0u;
    if (offset > rom_size || rom_size - offset < 3u) {
        return ROOM_DECOMPRESS_TRUNCATED;
    }

    const size_t length = (size_t)rom[offset] | ((size_t)rom[offset + 1u] << 8);
    const uint8_t marker = rom[offset + 2u];
    size_t pos = offset + 3u;

    /* A layer is at most half a WRAM page. Anything larger is a record read
     * from the wrong place, and decoding it would fill the caller's buffer with
     * the shape of a mistake. */
    if (length == 0u || length > out_capacity || length > ROOM_LAYER_MAX_BYTES) {
        return ROOM_DECOMPRESS_IMPLAUSIBLE;
    }

    const uint16_t end = (uint16_t)((uint32_t)start + length);
    size_t written = 0u;
    size_t remaining = length;

    for (;;) {
        if (pos >= rom_size) {
            return ROOM_DECOMPRESS_TRUNCATED;
        }
        const uint8_t token = rom[pos++];
        if (token != marker) {
            out[written++] = token;
            if (--remaining == 0u) {
                *out_length = length;
                return ROOM_DECOMPRESS_OK;
            }
            continue;
        }

        if (rom_size - pos < 2u) {
            return ROOM_DECOMPRESS_TRUNCATED;
        }
        const uint8_t delta = rom[pos];
        const uint8_t control = rom[pos + 1u];
        pos += 2u;

        size_t run = (size_t)(control & 0x0Fu) + 4u;
        if (run == 0x13u) {
            if (pos >= rom_size) {
                return ROOM_DECOMPRESS_TRUNCATED;
            }
            run = (size_t)rom[pos] + 0x13u;
            ++pos;
        }

        uint32_t source =
            ((uint32_t)start + (uint32_t)((control >> 4) & 0x0Fu) * 256u + (uint32_t)delta) &
            0xFFFFu;

        for (;;) {
            bool zero = false;
            if (!room_layer_window(&source, start, end, &zero)) {
                /* Below the layer: the guest reads $Cxxx, its own workspace.
                 * Nothing about the cartridge predicts that byte. */
                return ROOM_DECOMPRESS_OUTSIDE_PAGE;
            }
            uint8_t byte = 0u;
            if (!zero) {
                const uint32_t index = (source - (uint32_t)start) & 0xFFFFu;
                if (index >= length) {
                    return ROOM_DECOMPRESS_OUTSIDE_PAGE;
                }
                byte = out[index];
            }
            out[written++] = byte;
            if (--remaining == 0u) {
                *out_length = length;
                return ROOM_DECOMPRESS_OK;
            }
            source = (source + 1u) & 0xFFFFu;
            if (--run == 0u) {
                break;
            }
        }
    }
}

bool room_layer_block_read(const uint8_t* rom,
                           size_t rom_size,
                           uint8_t switchable_bank,
                           uint16_t table_cpu_addr,
                           uint8_t area,
                           uint8_t room,
                           uint8_t slot,
                           RoomLayerBlock* out) {
    if (rom == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    /* 00:0551: a pointer table with a stride of two, indexed by the area. */
    const uint32_t entry_addr = (uint32_t)table_cpu_addr + (uint32_t)(area & 0x7Fu) * 2u;
    if (entry_addr > 0xFFFFu) {
        return false;
    }
    size_t entry_offset = 0u;
    if (!room_record_cpu_to_rom((uint16_t)entry_addr, switchable_bank, rom, rom_size,
                                &entry_offset) ||
        rom_size - entry_offset < 2u) {
        return false;
    }
    const uint16_t base =
        (uint16_t)((uint16_t)rom[entry_offset] | ((uint16_t)rom[entry_offset + 1u] << 8));

    /* 29:42EC: base + room * 12, then the caller's own offset into the record. */
    const uint32_t record_addr =
        (uint32_t)base + (uint32_t)room * ROOM_LAYER_ENTRY_BYTES + (uint32_t)slot;
    if (record_addr > 0xFFFFu) {
        return false;
    }
    size_t record_offset = 0u;
    if (!room_record_cpu_to_rom((uint16_t)record_addr, switchable_bank, rom, rom_size,
                                &record_offset) ||
        rom_size - record_offset < ROOM_LAYER_RECORD_BYTES) {
        return false;
    }

    out->cpu_addr = (uint16_t)record_addr;
    out->ptr = (uint16_t)((uint16_t)rom[record_offset] |
                          ((uint16_t)rom[record_offset + 1u] << 8));
    out->bank = rom[record_offset + 2u];
    out->valid = true;
    return true;
}

bool room_layers_read(const uint8_t* rom,
                      size_t rom_size,
                      uint8_t switchable_bank,
                      uint16_t table_cpu_addr,
                      uint8_t area,
                      uint8_t room,
                      RoomLayers* out) {
    if (rom == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->area = area;
    out->room = room;

    RoomLayerBlock block;
    if (!room_layer_block_read(rom, rom_size, switchable_bank, table_cpu_addr, area, room,
                               ROOM_LAYER_SLOT_ATLAS, &block)) {
        return false;
    }
    out->atlas_ptr = block.ptr;
    out->atlas_bank = block.bank;

    for (uint32_t i = 0u; i < ROOM_LAYER_KINDS; ++i) {
        if (!room_layer_block_read(rom, rom_size, switchable_bank, table_cpu_addr, area, room,
                                   k_slot[i], &block)) {
            return false;
        }
        size_t stream_offset = 0u;
        if (!room_record_cpu_to_rom(block.ptr, block.bank, rom, rom_size, &stream_offset)) {
            return false;
        }
        size_t length = 0u;
        const RoomDecompressStatus status =
            room_layer_decompress(rom, rom_size, stream_offset, k_page_start[i], out->page[i],
                                  ROOM_LAYER_MAX_BYTES, &length);
        if (status != ROOM_DECOMPRESS_OK) {
            return false;
        }
        out->bytes[i] = (uint16_t)length;
    }

    out->valid = true;
    return true;
}
