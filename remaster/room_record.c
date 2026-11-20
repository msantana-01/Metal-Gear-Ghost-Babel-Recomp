#include "room_record.h"

#include <string.h>

/* Banked window and the size of one bank. Spelled out rather than taken from the
 * engine so this module stays a leaf: it is address arithmetic over a byte
 * image and can be compiled and measured without the emulator. */
#define ROOM_BANK_SIZE 0x4000u

static bool rom_read_u16(const uint8_t* rom, size_t rom_size, size_t offset, uint16_t* out) {
    if (offset > rom_size || rom_size - offset < 2u) {
        return false;
    }
    *out = (uint16_t)((uint16_t)rom[offset] | ((uint16_t)rom[offset + 1u] << 8));
    return true;
}

bool room_record_cpu_to_rom(uint16_t cpu_addr,
                            uint8_t switchable_bank,
                            const uint8_t* rom,
                            size_t rom_size,
                            size_t* out_offset) {
    if (rom == NULL || out_offset == NULL) {
        return false;
    }
    size_t offset;
    if (cpu_addr < (uint16_t)ROOM_BANK_SIZE) {
        /* Fixed bank: no bank register involved. */
        offset = (size_t)cpu_addr;
    } else if (cpu_addr < 0x8000u) {
        const size_t base = (size_t)switchable_bank * (size_t)ROOM_BANK_SIZE;
        const size_t window = (size_t)cpu_addr - (size_t)ROOM_BANK_SIZE;
        /* The addition is checked rather than assumed: switchable_bank is 8 bits
         * here because that is what the loader's bank register holds for this
         * title, and a cartridge large enough to overflow needs a wider one. */
        if (base > (size_t)-1 - window) {
            return false;
        }
        offset = base + window;
    } else {
        /* $8000 and up is not cartridge. A guest read there lands in RAM, so
         * reading it as ROM would answer a different question than the one the
         * loader asked. Same rule the engine's own semantic reader applies. */
        return false;
    }
    if (offset > rom_size || rom_size - offset < 1u) {
        return false;
    }
    *out_offset = offset;
    return true;
}

bool room_table_entry_read(const uint8_t* rom,
                           size_t rom_size,
                           uint8_t switchable_bank,
                           uint16_t table_cpu_addr,
                           uint8_t area,
                           uint8_t room,
                           uint16_t stride,
                           uint8_t* out,
                           size_t out_len,
                           uint16_t* out_cpu_addr) {
    if (rom == NULL || out == NULL || out_len == 0u || stride < out_len) {
        return false;
    }

    /* 00:0551: a pointer table with a stride of two, indexed by the area with its
     * top bit masked off. */
    const uint32_t index = (uint32_t)(area & 0x7Fu) * 2u;
    const uint32_t entry_addr = (uint32_t)table_cpu_addr + index;
    if (entry_addr > 0xFFFFu) {
        return false;
    }

    size_t entry_offset = 0u;
    if (!room_record_cpu_to_rom((uint16_t)entry_addr, switchable_bank, rom, rom_size,
                                &entry_offset)) {
        return false;
    }
    uint16_t table_base = 0u;
    if (!rom_read_u16(rom, rom_size, entry_offset, &table_base)) {
        return false;
    }

    /* 00:0534 for the record table (stride 8), `add hl,hl` x4 for the header
     * table (stride 16): the base plus the room index times the entry size. */
    const uint32_t addr = (uint32_t)table_base + (uint32_t)room * (uint32_t)stride;
    if (addr > 0xFFFFu) {
        return false;
    }

    size_t offset = 0u;
    if (!room_record_cpu_to_rom((uint16_t)addr, switchable_bank, rom, rom_size, &offset)) {
        return false;
    }
    /* The whole entry has to be there: an entry whose last byte is off the end
     * of the cartridge is a failed resolve, and reporting part of it would be a
     * plausible lie. */
    if (offset > rom_size || rom_size - offset < out_len) {
        return false;
    }
    memcpy(out, rom + offset, out_len);
    if (out_cpu_addr != NULL) {
        *out_cpu_addr = (uint16_t)addr;
    }
    return true;
}

bool room_record_read(const uint8_t* rom,
                      size_t rom_size,
                      uint8_t switchable_bank,
                      uint16_t table_cpu_addr,
                      uint8_t area,
                      uint8_t room,
                      RoomRecord* out) {
    if (rom == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->area = area;
    out->room = room;

    if (!room_table_entry_read(rom,
                               rom_size,
                               switchable_bank,
                               table_cpu_addr,
                               area,
                               room,
                               ROOM_RECORD_BYTES,
                               out->raw,
                               ROOM_RECORD_BYTES,
                               &out->cpu_addr)) {
        return false;
    }
    for (uint32_t i = 0u; i < ROOM_RECORD_FIELDS; ++i) {
        out->field[i] = (uint16_t)((uint16_t)out->raw[i * 2u] |
                                   ((uint16_t)out->raw[i * 2u + 1u] << 8));
    }
    out->valid = true;
    return true;
}
