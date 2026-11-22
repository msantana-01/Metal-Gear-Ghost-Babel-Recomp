/* ghost-babel-remaster: native presentation host.
 *
 * The second artifact described in ADR-006. It runs the same recompiled guest
 * as the authentic build and, at every completed frame, captures the runtime's
 * presentation snapshot and renders it itself instead of trusting the SDL
 * host's blit.
 *
 * This file deliberately does NOT compare. An earlier version counted matching
 * pixels in C and also painted a side-by-side image; the two disagreed about
 * the same instant, the same buffer and the same frame - the counter said 2
 * mismatches, the written image said 4289 - and reading the source did not say
 * which one was wrong. So the comparison moved out: this host runs and dumps
 * raw frames, and tools/audit_presenter_divergence.py does all of the counting
 * with numpy, reading the same bytes any third party would.
 *
 * One writer, one reader, no second opinion.
 *
 * What stays here is the renderer's own structural facts - pixels drawn,
 * objects dropped by the ten-per-line budget - which describe the renderer and
 * not its agreement with anybody.
 *
 * guest_writeback_allowed = false: this host only reads the snapshot.
 */

#include "baserom.h"
#include "gbrt.h"
#include "gbrt_hash.h"
#include "gbrt_port.h"
#include "gbrt_presentation.h"
#ifdef GB_HAS_SDL2
#include <SDL.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <time.h>
#endif

#include "audio_stats.h"
#include "camera_extent.h"
#include "native_presenter.h"
#include "platform_sdl.h"
#include "room_actors.h"
#include "room_layers.h"
#include "room_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_WIDTH GB_PRESENTATION_ACCURATE_WIDTH
#define SCREEN_HEIGHT GB_PRESENTATION_ACCURATE_HEIGHT
#ifdef _WIN32
#define SWEEP_STATE_PATH "remaster-sweep-state.bin"
#else
#define SWEEP_STATE_PATH "/tmp/remaster-sweep-state.bin"
#endif
#define SWEEP_MAX_DRAINS 16u
#define WRAM_TOTAL (0x1000u * 8u)
#define OAM_BYTES 160u
/* Scene revealed on each side of the centred game view. */
#define WIDESCREEN_MARGIN_PX 48u
/* The widest one band gets: the surface left after the HUD strip, minus the
 * active 136px playfield - 256 - 24 - 136. The picture slides toward the side
 * that is empty (ADR-011), so a band can be 96 columns wide. */
#define WIDESCREEN_BAND_MAX_PX 96u
#define TRACE_RECORD_BYTES (WRAM_TOTAL + OAM_BYTES)

/* The room's camera limits and the camera itself, where the guest keeps them.
 *
 * Bank 41's room loader (29:410a) writes the room record's first two 16-bit
 * fields into WRAM bank 0 on every room change: $CC0B/$CC0C is the camera's
 * right limit and $CC0D/$CC0E its left. $FFB0/$FFB1 is the camera position in
 * world pixels, and its low byte is what the STAT handler copies into SCX.
 * Both are read-only observation: nothing here writes them back. */
#define GUEST_ROOM_MAX_X 0x0C0Bu
#define GUEST_ROOM_MIN_X 0x0C0Du
#define GUEST_CAMERA_X 0x0030u
#define GUEST_CAMERA_Y 0x0032u /* $FFB2; 08:4839 clamps it against $CC0F */

/* Which room the guest is in, and where its record lives in the cartridge.
 *
 * 29:42C5 resolves the record: it indexes a pointer table at $4492 with the area
 * in $CBC0 (masked $7F, stride 2), walks room*8 bytes into the entry it lands on,
 * and reads a 16-bit field at the offset the caller asked for. 29:410a asks for
 * offsets 0, 2, 4 and 6 and writes the four words to $CC0B/$CC0C,
 * $CC0D/$CC0E, $CC0F/$CC10 and $CC11/$CC12.
 *
 * The first two are the camera limits read above. The other two have never been
 * read, and they are the whole reason this exists: if the record describes the
 * room and not only its camera, the widescreen margin stops being a
 * reconstruction of what the guest happened to leave in VRAM. See
 * room_record.h. */
#define GUEST_ROOM_AREA 0x0BC0u
#define GUEST_ROOM_ROOM 0x0BC1u
#define GUEST_ROOM_SOURCE_NODE 0x0BC2u /* $CBC2, the door tile under the player */
#define GUEST_ROOM_FIELD2 0x0C0Fu
#define GUEST_ROOM_FIELD3 0x0C11u
#define GUEST_ROOM_TABLE_BANK 0x29u
#define GUEST_ROOM_TABLE_ADDR 0x4492u

static CameraBounds read_camera_bounds(const GBContext* ctx) {
    CameraBounds bounds;
    bounds.max_x = (uint16_t)(ctx->wram[GUEST_ROOM_MAX_X] |
                             ((uint16_t)ctx->wram[GUEST_ROOM_MAX_X + 1u] << 8));
    bounds.min_x = (uint16_t)(ctx->wram[GUEST_ROOM_MIN_X] |
                              ((uint16_t)ctx->wram[GUEST_ROOM_MIN_X + 1u] << 8));
    return bounds;
}

/* The camera in world pixels. SCX is its low byte, so this agrees with the
 * picture the PPU drew; the high byte is what the margin needs, because the
 * room's limits are 16-bit and a room can sit past the map's own wrap. */
static uint16_t read_camera_x(const GBContext* ctx) {
    return (uint16_t)(ctx->hram[GUEST_CAMERA_X] |
                      ((uint16_t)ctx->hram[GUEST_CAMERA_X + 1u] << 8));
}

/* The camera's Y, from $FFB2. The guest clamps it against $CC0F, so it is the
 * same value the picture was drawn at, not a guess. */
static uint16_t read_camera_y(const GBContext* ctx) {
    return (uint16_t)(ctx->hram[GUEST_CAMERA_Y] |
                      ((uint16_t)ctx->hram[GUEST_CAMERA_Y + 1u] << 8));
}

/* The room the guest says it is heading into, and the record the cartridge holds
 * for it, next to the record the loader has already written to WRAM.
 *
 * One row per change of identity and one per change of agreement, rather than one
 * per frame: a session visits a handful of rooms and a row per frame would bury
 * them. The agreement column is the point of the exercise. This is the acceptance
 * test for reading a room out of the cartridge at all: if the record's four
 * fields do not reproduce $CC0B, $CC0D, $CC0F and $CC11, the addressing is wrong
 * and nothing else about the record means anything.
 *
 * All four are compared, not the first two. The record was read for its camera
 * limits, and the X pair was the one the widescreen needed; the Y pair is the
 * same loader writing the same record, and comparing it costs nothing while
 * settling whether the record is four limits or two limits and two unknowns.
 *
 * y_in_room is the test of what the Y pair means. If $CC0F/$CC11 is the camera's
 * vertical range, the camera in $FFB2 - the word 08:4839 clamps - is inside it on
 * every frame of the room, including the first. A wrong reading of the field
 * order still passes an interval test, so the disassembly, not this column, is
 * what fixes which end is which; this column is what would have caught the pair
 * being something else entirely, such as a size or a pointer.
 *
 * The two are not always the same instant. $CBC0/$CBC1 is the pair the game sets
 * when it decides to change rooms; the loader runs afterwards and only then
 * writes $CC0B..$CC12. So a row taken in between shows the pair leading WRAM by
 * one room, and the row that follows when WRAM catches up is the one that proves
 * the record was read correctly. Logging the flip rather than hiding it is
 * deliberate: the interval is exactly the window where a semantic renderer would
 * be reading the wrong room. */
static const char ROOM_HEADER[] =
    "frame,area,room,resolved,cpu_addr,w0,w1,w2,w3,cc0b,cc0d,cc0f,cc11,limits_agree,"
    "camera_y,y_in_room\n";

/* Agreement between the resolved record and the loader's copy in WRAM. */
enum {
    ROOM_AGREE_UNRESOLVED = 0,
    ROOM_AGREE_DIFFERS = 1,
    ROOM_AGREE_SAME = 2,
};

typedef struct RoomObservation {
    uint8_t area;
    uint8_t room;
    bool resolved;
    uint16_t cpu_addr;
    uint16_t field[ROOM_RECORD_FIELDS];
    uint16_t live[ROOM_RECORD_FIELDS];
    uint16_t camera_y;
    bool camera_in_room;
    int agreement;
} RoomObservation;

static RoomObservation observe_room(const GBContext* ctx) {
    RoomObservation obs;
    memset(&obs, 0, sizeof(obs));
    obs.area = ctx->wram[GUEST_ROOM_AREA];
    obs.room = ctx->wram[GUEST_ROOM_ROOM];

    RoomRecord rec;
    obs.resolved = room_record_read(ctx->rom, ctx->rom_size, GUEST_ROOM_TABLE_BANK,
                                    GUEST_ROOM_TABLE_ADDR, obs.area, obs.room, &rec);
    obs.cpu_addr = rec.cpu_addr;
    memcpy(obs.field, rec.field, sizeof(obs.field));

    static const uint16_t live_addr[ROOM_RECORD_FIELDS] = {
        GUEST_ROOM_MAX_X, GUEST_ROOM_MIN_X, GUEST_ROOM_FIELD2, GUEST_ROOM_FIELD3};
    for (uint32_t i = 0u; i < ROOM_RECORD_FIELDS; ++i) {
        obs.live[i] = (uint16_t)(ctx->wram[live_addr[i]] |
                                 ((uint16_t)ctx->wram[live_addr[i] + 1u] << 8));
    }

    if (!obs.resolved) {
        obs.agreement = ROOM_AGREE_UNRESOLVED;
    } else {
        bool same = true;
        for (uint32_t i = 0u; i < ROOM_RECORD_FIELDS; ++i) {
            same = same && obs.field[i] == obs.live[i];
        }
        obs.agreement = same ? ROOM_AGREE_SAME : ROOM_AGREE_DIFFERS;
    }

    /* The camera's world row, and whether the record's Y pair contains it. */
    obs.camera_y = (uint16_t)(ctx->hram[GUEST_CAMERA_Y] |
                              ((uint16_t)ctx->hram[GUEST_CAMERA_Y + 1u] << 8));
    if (obs.resolved) {
        const uint16_t min_y = obs.field[ROOM_FIELD_CAMERA_MIN_Y];
        const uint16_t max_y = obs.field[ROOM_FIELD_CAMERA_MAX_Y];
        obs.camera_in_room = obs.camera_y >= min_y && obs.camera_y <= max_y;
    }
    return obs;
}

static void write_room_row(FILE* out, uint32_t frame, const RoomObservation* obs) {
    fprintf(out, "%u,%u,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u,%d\n", frame,
            (unsigned)obs->area, (unsigned)obs->room, obs->resolved ? 1 : 0,
            (unsigned)obs->cpu_addr, (unsigned)obs->field[0], (unsigned)obs->field[1],
            (unsigned)obs->field[2], (unsigned)obs->field[3], (unsigned)obs->live[0],
            (unsigned)obs->live[1], (unsigned)obs->live[2], (unsigned)obs->live[3],
            obs->agreement == ROOM_AGREE_SAME ? 1 : 0, (unsigned)obs->camera_y,
            obs->camera_in_room ? 1 : 0);
}

static uint32_t fnv1a32(const uint8_t* data, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0u; i < n; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

/* The room's tile-index table, hashed.
 *
 * 22:42F5 and its three siblings compute a position in the world, $183F turns
 * that into a tile number by reading a 16-bit word from $D000 with WRAM bank 5
 * selected, and copies 8 bytes of that tile out of the atlas at $CBFF. So the
 * question that decides whether the margin can be drawn without the camera ever
 * having been there is not "is the layout in the ROM" - the atlas is - but
 * whether this table is a window that moves with the camera or a room-sized
 * structure that stands still. Both are consistent with what has been read so
 * far, and only measurement separates them.
 *
 * A hash per frame is enough: the table is 1 KiB and a dump of it per frame
 * would be unreadable, while a hash that never changes over a walk says exactly
 * the thing that needs saying. */
#define GUEST_ROOM_INDEX_TABLE 0x5000u /* $D000, WRAM bank 5 */
#define GUEST_ROOM_INDEX_BYTES 0x0400u
#define GUEST_ROOM_ATLAS 0x0BFFu /* $CBFF, WRAM bank 0 */

typedef struct SyncedGuestState {
    bool valid;
    uint64_t total_cycles;
    GBPresentationFrame frame;
    RoomObservation room_obs;
    CameraBounds bounds;
    uint16_t camera_x;
    uint16_t camera_y;
    uint8_t source_node;
    uint16_t live_atlas_ptr;
    uint8_t live_atlas_bank;
    uint8_t wram_visual[ROOM_LAYER_MAX_BYTES];
    RoomActorSnapshot actors;
} SyncedGuestState;

static void capture_guest_state(const GBContext* ctx,
                                const GBPresentationFrame* frame,
                                bool copy_visual,
                                SyncedGuestState* out) {
    out->valid = true;
    out->total_cycles = ctx->total_cycles;
    out->frame = *frame;
    out->room_obs = observe_room(ctx);
    out->bounds = read_camera_bounds(ctx);
    out->camera_x = read_camera_x(ctx);
    out->camera_y = read_camera_y(ctx);
    out->source_node = ctx->wram[GUEST_ROOM_SOURCE_NODE];
    out->live_atlas_ptr = (uint16_t)(ctx->wram[GUEST_ROOM_ATLAS] |
                                     ((uint16_t)ctx->wram[GUEST_ROOM_ATLAS + 1u] << 8));
    out->live_atlas_bank = ctx->wram[GUEST_ROOM_ATLAS + 2u];
    room_actors_capture(ctx->wram, WRAM_TOTAL, &out->actors);
    if (copy_visual) {
        memcpy(out->wram_visual, ctx->wram + GUEST_ROOM_INDEX_TABLE, ROOM_LAYER_MAX_BYTES);
    }
}

/* Pair accurate_pixels/scanline_lcdc from frame N (just drawn by the PPU)
 * with the pre-frame post-VBlank state (registers, tilemaps, OAM, camera, live
 * cells) whenever the LCD ran continuously without a state jump. That removes
 * the 1-frame / 1-px lag between the PPU playfield and the margin/bounds. */
static void build_render_state(const SyncedGuestState* prev,
                               const SyncedGuestState* post,
                               uint64_t pre_cycles,
                               uint32_t drain_cycles,
                               SyncedGuestState* out) {
    const uint8_t line0_lcdc = post->frame.hardware.scanline_lcdc[0];
    const bool use_prev = drain_cycles != 0u && prev->valid && prev->total_cycles == pre_cycles &&
                          (prev->frame.hardware.lcdc & 0x80u) != 0u &&
                          (line0_lcdc & 0x80u) != 0u && prev->frame.hardware.lcdc == line0_lcdc;
    if (use_prev) {
        *out = *prev;
        memcpy(out->frame.hardware.accurate_pixels, post->frame.hardware.accurate_pixels,
               sizeof(out->frame.hardware.accurate_pixels));
        memcpy(out->frame.hardware.scanline_lcdc, post->frame.hardware.scanline_lcdc,
               sizeof(out->frame.hardware.scanline_lcdc));
        out->frame.timing = post->frame.timing;
        out->total_cycles = post->total_cycles;
    } else {
        *out = *post;
    }
}

/* Count scanlines assembled from the secondary BG map, split between upper-half
 * dialogue overlays and lower-half status strips. */
static void count_overlay_rows(const GBPresentationHardwareSnapshot* hw,
                               uint32_t* out_top_overlay_rows,
                               uint32_t* out_bottom_status_rows) {
    uint32_t row_map[SCREEN_HEIGHT];
    uint32_t zero = 0u;
    for (uint32_t y = 0u; y < SCREEN_HEIGHT; ++y) {
        const uint8_t recorded = hw->scanline_lcdc[y];
        const uint8_t lcdc = ((recorded & 0x80u) != 0u) ? recorded : hw->lcdc;
        row_map[y] = (lcdc & 0x08u) != 0u ? 1u : 0u;
        if (row_map[y] == 0u) {
            zero++;
        }
    }
    const uint32_t scene_map = (zero * 2u > SCREEN_HEIGHT) ? 0u : 1u;
    uint32_t top = 0u;
    uint32_t bottom = 0u;
    for (uint32_t y = 0u; y < SCREEN_HEIGHT; ++y) {
        if (row_map[y] != scene_map) {
            if (y < SCREEN_HEIGHT / 2u) {
                top++;
            } else {
                bottom++;
            }
        }
    }
    *out_top_overlay_rows = top;
    *out_bottom_status_rows = bottom;
}

typedef struct PresenterRoomSync {
    bool known;
    uint8_t area;
    uint8_t room;
    uint32_t refusals;
    uint32_t placements;
} PresenterRoomSync;

/* Synchronise the presenter's cartridge room map, camera coordinates and live
 * WRAM bank 5 ($D000) cell table with the guest. Waiting for ROOM_AGREE_SAME
 * avoids switching rooms during the transition window when $CBC0/$CBC1 has
 * changed before WRAM limits and tiles catch up. */
static void sync_presenter_room(NativePresenter* presenter,
                                const uint8_t* rom,
                                size_t rom_size,
                                const SyncedGuestState* gs,
                                PresenterRoomSync* state) {
    const RoomObservation* obs = &gs->room_obs;
    const bool identity_differs =
        !state->known || obs->area != state->area || obs->room != state->room;
    if (identity_differs && obs->agreement == ROOM_AGREE_SAME) {
        if (state->known) {
            native_presenter_learn_transition(presenter, obs->area, obs->room);
        }
        native_presenter_clear_column_cache(presenter);
        state->known = true;
        state->area = obs->area;
        state->room = obs->room;
        if (!native_presenter_set_room(presenter, rom, rom_size, GUEST_ROOM_TABLE_BANK, obs->area,
                                       obs->room)) {
            state->refusals++;
        }
    }
    if (state->known && obs->live[0] > obs->live[1]) {
        if (native_presenter_place_camera(presenter, (int32_t)gs->camera_x,
                                          (int32_t)gs->camera_y)) {
            state->placements++;
        }
        if (!identity_differs && obs->agreement == ROOM_AGREE_SAME) {
            native_presenter_sync_room_cells(presenter, gs->wram_visual, ROOM_LAYER_MAX_BYTES,
                                             gs->live_atlas_ptr, gs->live_atlas_bank);
        }
    }
}

static void room_map_row(FILE* out, const GBContext* ctx, uint32_t frame) {
    const uint8_t* table = ctx->wram + GUEST_ROOM_INDEX_TABLE;
    uint32_t nonzero = 0u;
    for (size_t i = 0u; i < GUEST_ROOM_INDEX_BYTES; ++i) {
        if (table[i] != 0u) {
            nonzero++;
        }
    }
    fprintf(out, "%u,%u,%u,%u,%u,%08x,%u,%02x%02x%02x\n", frame,
            (unsigned)ctx->wram[GUEST_ROOM_AREA], (unsigned)ctx->wram[GUEST_ROOM_ROOM],
            (unsigned)read_camera_x(ctx),
            (unsigned)(ctx->hram[GUEST_CAMERA_Y] | ((uint16_t)ctx->hram[GUEST_CAMERA_Y + 1u] << 8)),
            fnv1a32(table, GUEST_ROOM_INDEX_BYTES), nonzero,
            (unsigned)ctx->wram[GUEST_ROOM_ATLAS], (unsigned)ctx->wram[GUEST_ROOM_ATLAS + 1u],
            (unsigned)ctx->wram[GUEST_ROOM_ATLAS + 2u]);
}

#define META_HEADER \
    "frame,scx,scy,lcdc,wy,wx,oam_live,bg_map_unique,bg_map_bank,tile_data_nonzero,map1_unique,map1_visible_nonzero,tile0_pattern,tile1_nonzero,pal0_c0_c1,attr_nonzero,tid_min,tid_max,tid_ge128,visible_blank,margin_left,margin_right,status_rows,top_overlay_rows\n"

typedef struct StructuralCounters {
    uint32_t frames_captured;
    uint32_t frames_traced;
    uint32_t captures_refused;
    uint32_t sprite_pixels_drawn;
    uint32_t sprite_pixels_hidden_by_bg;
    uint32_t sprites_over_budget;
    uint32_t sprite_line_placements;
    uint32_t shadows_admitted;
    uint32_t shadows_refused;
    uint32_t sprites_exiting;
    uint32_t sprites_exiting_left;
    uint32_t shadows_blocked_panel;
    uint32_t shadows_blocked_picture;
    uint32_t shadows_blocked_room;
    uint32_t shadows_last_frame;
    int32_t shadows_last_x;
    int32_t shadows_last_y;
    uint32_t shadows_last_width;
    uint32_t shadow_pixels_last_frame;
    int32_t shadow_box_x0;
    int32_t shadow_box_x1;
    int32_t shadow_box_y0;
    int32_t shadow_box_y1;
    uint32_t shadow_box_frame;
} StructuralCounters;

static const char* status_name(GBPresentationStatus status) {
    switch (status) {
        case GB_PRESENTATION_OK: return "OK";
        case GB_PRESENTATION_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case GB_PRESENTATION_ABI_MISMATCH: return "ABI_MISMATCH";
        case GB_PRESENTATION_ROM_MISMATCH: return "ROM_MISMATCH";
        case GB_PRESENTATION_NOT_AT_FRAME_BOUNDARY: return "NOT_AT_FRAME_BOUNDARY";
        case GB_PRESENTATION_INVALID_SCENE: return "INVALID_SCENE";
        case GB_PRESENTATION_INVALID_UI: return "INVALID_UI";
        case GB_PRESENTATION_INVALID_ASSET: return "INVALID_ASSET";
        default: return "UNKNOWN";
    }
}


/* ── Performance baseline ──────────────────────────────────────────────────
 *
 * The overlay answers "what does the platform think of the audio", which is one
 * of the two questions worth asking. The other is whether the loop keeps up,
 * and from inside the two failures look identical: a remaster too heavy to hold
 * 60fps and one that is comfortably fast but whose audio arrives late both look
 * like "something feels wrong". Only a number separates them.
 *
 * So: count frames against the wall clock, count CPU against the frame budget,
 * and print the audio counters next to both. A baseline is only useful if the
 * same command produces the same numbers next week, which is why this lives in
 * the binary instead of in a stopwatch around it.
 *
 * Reporting is per wall-clock frame, not per emulated frame: a run that cannot
 * hold 60fps and a run that can both render the same number of guest frames,
 * and only the clock tells them apart.
 */
#define PERF_FRAME_BUDGET_MS (1000.0 / 59.727)
#define PERF_REPORT_EVERY_FRAMES 600u

typedef struct PerfCounters {
#ifdef _WIN32
    LARGE_INTEGER wall_start;
    LARGE_INTEGER frame_start;
    LARGE_INTEGER freq;
    uint64_t cpu_start_100ns;
    uint64_t cpu_mark_100ns;
#else
    struct timespec wall_start;
    struct timespec frame_start;
    struct rusage cpu_start;
    struct rusage cpu_mark;
#endif
    uint32_t frames;
    uint32_t reported_at;
    double worst_frame_ms;
    double worst_frame_number;
    double worst_frame_cpu_ms;
    double first_frame_ms;
} PerfCounters;

static bool g_perf_report = false;

#ifdef _WIN32
static double perf_now_ms(const LARGE_INTEGER* a, const LARGE_INTEGER* b, LARGE_INTEGER freq) {
    if (freq.QuadPart <= 0) {
        return 0.0;
    }
    return (double)(b->QuadPart - a->QuadPart) * 1000.0 / (double)freq.QuadPart;
}

static uint64_t perf_cpu_100ns(void) {
    FILETIME create_t, exit_t, kernel_t, user_t;
    if (!GetProcessTimes(GetCurrentProcess(), &create_t, &exit_t, &kernel_t, &user_t)) {
        return 0u;
    }
    const uint64_t k = ((uint64_t)kernel_t.dwHighDateTime << 32) | kernel_t.dwLowDateTime;
    const uint64_t u = ((uint64_t)user_t.dwHighDateTime << 32) | user_t.dwLowDateTime;
    return k + u;
}

static double perf_cpu_ms(uint64_t a, uint64_t b) {
    return (double)(b - a) / 10000.0;
}
#else
static double perf_now_ms(const struct timespec* a, const struct timespec* b) {
    return (double)(b->tv_sec - a->tv_sec) * 1000.0 + (double)(b->tv_nsec - a->tv_nsec) / 1.0e6;
}

static double perf_cpu_ms(const struct rusage* a, const struct rusage* b) {
    const double ua = (double)a->ru_utime.tv_sec * 1000.0 + (double)a->ru_utime.tv_usec / 1000.0;
    const double sa = (double)a->ru_stime.tv_sec * 1000.0 + (double)a->ru_stime.tv_usec / 1000.0;
    const double ub = (double)b->ru_utime.tv_sec * 1000.0 + (double)b->ru_utime.tv_usec / 1000.0;
    const double sb = (double)b->ru_stime.tv_sec * 1000.0 + (double)b->ru_stime.tv_usec / 1000.0;
    return (ub + sb) - (ua + sa);
}
#endif

/* The panel effect the window is actually showing. The platform owns the
 * list; the report needs a name, not a number. */
static const char* screen_effect_name(GBScreenEffect effect) {
    switch (effect) {
        case GB_SCREEN_EFFECT_SCANLINES:
            return "scanlines";
        case GB_SCREEN_EFFECT_LCD_GRID:
            return "lcd grid";
        case GB_SCREEN_EFFECT_CRT:
            return "crt";
        case GB_SCREEN_EFFECT_SHADER:
            return "crt-trinitron";
        case GB_SCREEN_EFFECT_CRT_CURVED:
            return "crt-curved";
        case GB_SCREEN_EFFECT_GBC_LCD:
            return "gbc-lcd";
        case GB_SCREEN_EFFECT_OFF:
        default:
            return "off";
    }
}

static void perf_init(PerfCounters* perf) {
#ifdef _WIN32
    QueryPerformanceFrequency(&perf->freq);
    QueryPerformanceCounter(&perf->wall_start);
    perf->frame_start = perf->wall_start;
    perf->cpu_start_100ns = perf_cpu_100ns();
    perf->cpu_mark_100ns = perf->cpu_start_100ns;
#else
    clock_gettime(CLOCK_MONOTONIC, &perf->wall_start);
    clock_gettime(CLOCK_MONOTONIC, &perf->frame_start);
    getrusage(RUSAGE_SELF, &perf->cpu_start);
    getrusage(RUSAGE_SELF, &perf->cpu_mark);
#endif
    perf->frames = 0u;
    perf->reported_at = 0u;
    perf->worst_frame_ms = 0.0;
    perf->worst_frame_number = 0.0;
    perf->worst_frame_cpu_ms = 0.0;
    perf->first_frame_ms = 0.0;
}

static void perf_report(const PerfCounters* perf, const char* tag) {
#ifdef _WIN32
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint64_t cpu_now = perf_cpu_100ns();
    const double wall_ms = perf_now_ms(&perf->wall_start, &now, perf->freq);
    const double cpu_ms = perf_cpu_ms(perf->cpu_start_100ns, cpu_now);
#else
    struct timespec now;
    struct rusage cpu_now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    getrusage(RUSAGE_SELF, &cpu_now);

    const double wall_ms = perf_now_ms(&perf->wall_start, &now);
    const double cpu_ms = perf_cpu_ms(&perf->cpu_start, &cpu_now);
#endif
    if (perf->frames == 0u || wall_ms <= 0.0) {
        return;
    }
    const double fps = (double)perf->frames * 1000.0 / wall_ms;
    const double per_frame_cpu = cpu_ms / (double)perf->frames;

    printf("\n  performance%s\n", tag != NULL ? tag : "");
    printf("    frames              %u\n", perf->frames);
    printf("    wall clock          %.2f s  (%.1f fps, %.3fx real time)\n", wall_ms / 1000.0,
           fps, fps / 59.727);
    printf("    cpu per frame       %.3f ms  (%.1f%% of the %.2f ms budget)\n", per_frame_cpu,
           100.0 * per_frame_cpu / PERF_FRAME_BUDGET_MS, PERF_FRAME_BUDGET_MS);
    printf("    first frame         %.3f ms  (window + CRT setup, not a steady-state cost)\n",
           perf->first_frame_ms);
    printf("    worst steady frame  %.3f ms  (frame %.0f, %.3f ms of it cpu)\n",
           perf->worst_frame_ms, perf->worst_frame_number, perf->worst_frame_cpu_ms);
    /* The platform splits its own cost out. Without it "5.9 ms per frame" is
     * one number that could be emulation, texture upload, or SDL compositing,
     * and those have nothing to do with each other when the time has to come
     * down. Pacing is the sleeping half: it should be the budget minus
     * everything else, and if it is zero the loop is not being held to
     * real time at all. */
    GBPlatformTimingInfo timing;
    gb_platform_get_timing_info(&timing);
    printf("    platform compose    %.3f ms\n", timing.compose_ms);
    printf("    platform present    %.3f ms\n", timing.present_ms);
    printf("    platform render     %.3f ms\n", timing.total_render_ms);
    printf("    platform pacing     %.3f ms  (%u cycles of target)\n", timing.pacing_ms,
           timing.pacing_cycles);
    printf("    audio generated     %llu\n", (unsigned long long)g_audio_stats.total_samples_generated);
    printf("    audio queued        %llu\n", (unsigned long long)g_audio_stats.total_samples_queued);
    printf("    audio dropped       %llu  (%.3f%% of generated)\n",
           (unsigned long long)g_audio_stats.total_samples_dropped,
           g_audio_stats.total_samples_generated > 0
               ? 100.0 * (double)g_audio_stats.total_samples_dropped /
                     (double)g_audio_stats.total_samples_generated
               : 0.0);
    printf("    audio underruns     %llu\n", (unsigned long long)g_audio_stats.total_buffer_underruns);
    printf("    audio buffer        %u/%u frames  (%.1f ms)\n", g_audio_stats.buffer_fill_samples,
           g_audio_stats.buffer_capacity_samples, g_audio_stats.buffer_latency_ms);
    /* A ring that is both full and underrunning is not one problem, it is the
     * level swinging between both walls. Whether the producer runs ahead of
     * the device or behind it decides which wall it hits first, so say which.
     * They agree when the game is paced to real time and disagree when it is
     * not - and a mismatch of a few percent is enough to walk a 370ms buffer
     * from empty to full over half a minute. */
    printf("    audio rate          %.0f Hz produced / %u Hz device\n",
           g_audio_stats.sample_rate_actual, g_audio_stats.device_sample_rate);
    printf("    audio mastering     %s\n", gb_platform_get_audio_mastering() ? "on" : "off");
    printf("    screen effect       %s\n", screen_effect_name(gb_platform_get_screen_effect()));
    printf("    audio high water    %u frames\n", g_audio_stats.buffer_high_water_samples);
    if (fps > 59.727 * 1.05) {
        printf("\n  NOTE: the loop is running faster than real time, so the guest\n"
               "        produces audio faster than the device consumes it and the\n"
               "        surplus is dropped. That is a pacing fault, not an audio one.\n");
    }
}

/* The learned edge is the one counter that only exists while the session is
 * open: the graph is in the presenter, not in a file, so a run that is watched
 * live has to say what it learned before it exits. The final summary already
 * prints it; this is the same line inside the 10s report, where a crossing can
 * be seen as it happens instead of after the window is closed. */
static void perf_report_learned(const NativePresenter* presenter) {
    printf("    learned edges      %u stored / %u placements / %u refused / %u conflicts / %u full\n",
           presenter->learned.recorded, presenter->learned_placements, presenter->learned_refused,
           presenter->learned.conflicts, presenter->learned.full);
}

static void perf_tick(PerfCounters* perf, const NativePresenter* presenter) {
#ifdef _WIN32
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint64_t cpu_now = perf_cpu_100ns();
    const double ms = perf_now_ms(&perf->frame_start, &now, perf->freq);
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct rusage cpu_now;
    getrusage(RUSAGE_SELF, &cpu_now);
    const double ms = perf_now_ms(&perf->frame_start, &now);
#endif
    /* Wall time alone cannot tell a heavy frame from a blocked one. The guest
     * stalling on I/O and the loop being descheduled both look like a long
     * frame, and they call for opposite fixes, so the CPU half is recorded
     * alongside. */
    /* The first frame builds the window's textures and compiles the CRT
     * effect, and costs more than any hundred that follow. Averaging it in
     * makes the whole run look heavier than it is, and calling it the worst
     * frame hides the frames that actually matter. So it is reported on its
     * own line and kept out of the worst. */
    if (perf->frames == 0u) {
        perf->first_frame_ms = ms;
    } else if (ms > perf->worst_frame_ms) {
        perf->worst_frame_ms = ms;
        perf->worst_frame_number = (double)perf->frames;
#ifdef _WIN32
        perf->worst_frame_cpu_ms = perf_cpu_ms(perf->cpu_mark_100ns, cpu_now);
#else
        perf->worst_frame_cpu_ms = perf_cpu_ms(&perf->cpu_mark, &cpu_now);
#endif
    }
#ifdef _WIN32
    perf->cpu_mark_100ns = cpu_now;
#else
    perf->cpu_mark = cpu_now;
#endif
    perf->frame_start = now;
    perf->frames++;
    if (g_perf_report && perf->frames - perf->reported_at >= PERF_REPORT_EVERY_FRAMES) {
        perf->reported_at = perf->frames;
        perf_report(perf, " (10s)");
        perf_report_learned(presenter);
    }
}

static bool write_ppm(const char* path, const uint32_t* pixels, uint32_t width) {
    FILE* out = fopen(path, "wb");
    if (out == NULL) {
        return false;
    }
    fprintf(out, "P6\n%u %u\n255\n", width, SCREEN_HEIGHT);
    for (uint32_t y = 0; y < SCREEN_HEIGHT; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint16_t c = (uint16_t)pixels[(size_t)y * width + x];
            const unsigned char rgb[3] = {
                (unsigned char)(((c >> 0) & 0x1Fu) * 255u / 31u),
                (unsigned char)(((c >> 5) & 0x1Fu) * 255u / 31u),
                (unsigned char)(((c >> 10) & 0x1Fu) * 255u / 31u),
            };
            fwrite(rgb, 1, 3, out);
        }
    }
    fclose(out);
    return true;
}

/* The accurate pixels on their own, unpainted. */
static bool write_reference_ppm(const GBPresentationFrame* frame, const char* path) {
    FILE* out = fopen(path, "wb");
    if (out == NULL) {
        return false;
    }
    fprintf(out, "P6\n%u %u\n255\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    for (uint32_t y = 0; y < SCREEN_HEIGHT; ++y) {
        for (uint32_t x = 0; x < SCREEN_WIDTH; ++x) {
            const uint16_t c = frame->hardware.accurate_pixels[(size_t)y * SCREEN_WIDTH + x];
            const unsigned char rgb[3] = {
                (unsigned char)(((c >> 0) & 0x1Fu) * 255u / 31u),
                (unsigned char)(((c >> 5) & 0x1Fu) * 255u / 31u),
                (unsigned char)(((c >> 10) & 0x1Fu) * 255u / 31u),
            };
            fwrite(rgb, 1, 3, out);
        }
    }
    fclose(out);
    return true;
}

/* The playable host.
 *
 * This is the authentic platform's loop with one thing changed: instead of
 * handing the platform the PPU framebuffer, it hands over the tile renderer's
 * surface. Everything else - window, input, the Esc menu, vsync, save states,
 * input recording - is the platform's, reused as it is. Rebuilding a front end
 * would mean rebuilding the menu and the settings with it, which is the one
 * thing a remaster must not do.
 *
 * The width is set before gb_platform_init because the window is sized from
 * it; asking afterwards would flash a 160px window before stretching. */
/* RGB555 to ARGB8888, matching the PPU's own expansion in ppu.c.
 *
 * Two formats meet here and confusing them is silent. The tile renderer works in
 * RGB555 because that is what the capture gives it and what the audit compares
 * against the reference. The platform's presentation buffer is ARGB8888, because
 * that is what the SDL texture is declared as and what gb_get_framebuffer
 * returns - ppu_get_framebuffer hands back rgb_framebuffer, not color_framebuffer.
 *
 * Handing RGB555 straight to the platform therefore does not crash and does not
 * look obviously wrong on a colourful screen: white 0x7FFF reads as (0,127,255).
 * It looked fine until a screenshot from the platform's own writer disagreed with
 * the headless dump of the same frame. */
static uint32_t rgb555_to_argb(uint32_t color) {
    const uint32_t r = ((color >> 0) & 0x1Fu) * 255u / 31u;
    const uint32_t g = ((color >> 5) & 0x1Fu) * 255u / 31u;
    const uint32_t b = ((color >> 10) & 0x1Fu) * 255u / 31u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static void drain_after_frame(GBContext* ctx, uint32_t drain_cycles) {
    if (drain_cycles == 0u) {
        return;
    }
    const uint32_t frame_cycles = ctx->frame_cycles;
    gb_reset_frame(ctx);
    ctx->stopped = 0;
    gb_run_cycles(ctx, drain_cycles);
    /* gb_run_cycles records the cycles it ran in the context, and the pacing
     * reads that number to decide how long a frame is allowed to take. The
     * drain is not outside the frame: it advances the guest, so it consumes
     * real time and it fills the audio pipeline. The frame's true length is
     * the part the guest ran plus the part we asked it to run past VBlank.
     *
     * Leaving this alone had two failures, and the first one hid the second.
     * Untouched, the drain's own cycle count became the frame's, so the target
     * was 4560 instead of 70224 and the loop ran 2.8 times too fast: the ring
     * sat full at 371ms and 28 million samples were dropped, which reads like
     * an audio fault and was not one. Restoring the pre-drain count fixed the
     * gross error but was still wrong by the drain itself - the target became
     * 65660, the loop ran 6.9% fast, and the APU produced 47161Hz against a
     * 44100Hz device. That surplus is small enough to look like nothing for a
     * minute and large enough to walk a big buffer from empty to full. The
     * symptom is the same both times, which is why only the measured rate
     * settles it: 65660 + 4560 = 70220, and 70220 is a Game Boy frame. */
    ctx->frame_cycles = frame_cycles + drain_cycles;
    ctx->frame_done = 1;
}

static int run_windowed(GBContext* ctx,
                        GBPresentationSource* source,
                        GBPresentationScene* scene,
                        GBPortFrame* ui,
                        NativePresenter* presenter,
                        uint32_t* surface,
                        uint32_t* present_pixels,
                        const uint32_t view_width,
                        const bool cgb_mode,
                        const uint32_t frame_limit,
                        CameraMargin margin,
                        CameraExtent* extent,
                        const uint32_t margin_left_cap,
                        const bool column_cache,
                        const bool room_map,
                        const char* screenshot_prefix,
                        const char* screenshot_frames,
                        const uint32_t capture_drain_cycles,
                        const bool perf_report_flag) {
    /* The platform's own screenshots go through the same present path, at
     * g_present_width, so they show what is actually on screen rather than the
     * guest framebuffer. That makes them the honest check that the wider
     * surface reached the window. */
    if (screenshot_prefix != NULL) {
        gb_platform_set_screenshot_prefix(screenshot_prefix);
    }
    if (screenshot_frames != NULL) {
        gb_platform_set_dump_frames(screenshot_frames);
    }
    if (!gb_platform_set_present_width((int)view_width)) {
        fprintf(stderr, "remaster: unsupported presentation width %u\n", view_width);
        return 1;
    }
    /* The remaster is the mastered artifact (ADR-009). Setting the default
     * before init leaves a saved audio.mastering preference in charge, so a
     * player who chose raw audio keeps that choice across launches. The
     * scope keeps that choice out of the authentic artifact's file: the two
     * artifacts share the bindings and the device, not their identity. */
    gb_platform_set_preference_scope("remaster");
    gb_platform_set_audio_mastering_default(true);
    gb_platform_set_screen_effect_default(GB_SCREEN_EFFECT_SHADER);
    if (!gb_platform_init(5)) {
        fprintf(stderr, "remaster: platform init failed\n");
        return 1;
    }
    native_presenter_enable_column_cache(presenter, column_cache);
    native_presenter_enable_room_map(presenter, room_map);
    gb_platform_register_context(ctx);

    ui->canvas_width = view_width;
    ui->canvas_height = SCREEN_HEIGHT;

    PresenterRoomSync room_sync;
    memset(&room_sync, 0, sizeof(room_sync));
    PerfCounters perf;
    perf_init(&perf);

    SyncedGuestState* prev_state = calloc(1u, sizeof(*prev_state));
    SyncedGuestState* post_state = calloc(1u, sizeof(*post_state));
    SyncedGuestState* render_state = calloc(1u, sizeof(*render_state));
    if (prev_state == NULL || post_state == NULL || render_state == NULL) {
        fprintf(stderr, "remaster: out of memory\n");
        free(prev_state);
        free(post_state);
        free(render_state);
        gb_platform_shutdown();
        return 1;
    }

    int status = 0;
    for (uint32_t frame = 0; frame < frame_limit; ++frame) {
        /* Input is pumped at the frame boundary, before the reset, which is
         * where the harness pumps it too. Polling after the frame instead put
         * the activation window 4560 cycles - a whole VBlank - later, and since
         * a script is anchored to absolute cycles that shifted which buttons
         * were held for which part of the frame. The two modes then played the
         * same input differently and the world drifted about 30% from the
         * measured composition within a dozen frames. */
        if (!gb_platform_poll_events(ctx)) {
            status = 0;
            goto shutdown;
        }
        const uint64_t pre_cycles = ctx->total_cycles;
        if (prev_state->valid && prev_state->total_cycles != pre_cycles) {
            prev_state->valid = false;
            room_sync.known = false;
            native_presenter_clear_column_cache(presenter);
        }
        gb_reset_frame(ctx);
        ctx->stopped = 0;
        while (!ctx->frame_done) {
            gb_run_cycles(ctx, 0xFFFFFFFFu);
        }

        /* Same capture point the harness validated: one VBlank past frame_done.
         * Capturing at frame_done itself shows a different instant and renders
         * a flat picture, which is not a rendering bug - it is the wrong
         * moment. */
        drain_after_frame(ctx, capture_drain_cycles);

        GBPresentationFrame snapshot;
        if (gbrt_presentation_capture(source, scene, ui, &snapshot) !=
            GB_PRESENTATION_OK) {
            /* Nothing to draw this frame; present what is on screen rather than
             * tearing it down to a blank. */
            prev_state->valid = false;
            gb_platform_vsync(ctx->frame_cycles);
            continue;
        }
        capture_guest_state(ctx, &snapshot, room_map, post_state);
        build_render_state(prev_state, post_state, pre_cycles, capture_drain_cycles, render_state);
        /* Show exactly as much as the room itself has. The guest has already
         * answered the question - its room loader put the camera's two limits
         * in WRAM - so this needs no walking, no learning and no fade: on the
         * first frame of a room the margin is already the right width, and it
         * can never reach into the next room. */
        if (room_map) {
            sync_presenter_room(presenter, ctx->rom, ctx->rom_size, render_state, &room_sync);
        }
        if (camera_extent_observe(extent, render_state->frame.hardware.scx,
                                  render_state->frame.hardware.scy, render_state->bounds)) {
            /* New room: the remembered columns belong to the one just left. */
            native_presenter_clear_column_cache(presenter);
        }
        camera_margin_for(extent, render_state->camera_x, margin_left_cap, &margin);

        /* $CBC2 is the transition the player is standing on, so it names the
         * door the margin should preview when the room has more than one on
         * the same edge. Values outside the reachable 0..11 are not a door. */
        native_presenter_set_source_node(
            presenter, render_state->source_node < 12u ? render_state->source_node : 0xFFu);
        native_presenter_set_actors(presenter, &render_state->actors);

        /* No sprite pass. The game's own 160 columns are copied from the
         * PPU's framebuffer, which already contains its sprites; redrawing them
         * from OAM over an exact region could only lose. */
        if (!native_presenter_render_background(presenter, &render_state->frame, margin.left,
                                                margin.right, cgb_mode)) {
            fprintf(stderr, "remaster: render failed at frame %u\n", frame);
            status = 1;
            break;
        }
        SyncedGuestState* tmp = prev_state;
        prev_state = post_state;
        post_state = tmp;
        /* render_frame rather than present_framebuffer: the latter is the
         * smooth-LCD path for partial frames and deliberately does not count a
         * guest frame, which would leave the platform's frame counter - and so
         * its screenshots and timing - frozen. Here the tile renderer's surface
         * *is* the completed frame. */
        for (size_t i = 0; i < (size_t)view_width * SCREEN_HEIGHT; ++i) {
            present_pixels[i] = rgb555_to_argb(surface[i]);
        }
        gb_platform_render_frame(present_pixels);
        gb_platform_vsync(ctx->frame_cycles);

        perf_tick(&perf, presenter);
    }

shutdown:
    free(prev_state);
    free(post_state);
    free(render_state);
    gb_platform_shutdown();
    perf_report(&perf, perf_report_flag ? "final" : NULL);
    perf_report_learned(presenter);
    return status;
}

static bool run_one_frame(GBContext* ctx) {
    /* Pump scripted input before running the frame.
     *
     * The automation input lives in the platform layer, which is where the
     * joypad register is written. A host with its own run loop therefore has to
     * ask for it explicitly: installing a script is not the same as playing
     * it. Skipping this looks exactly like the game ignoring input, because the
     * machine runs perfectly and nothing moves.
     *
     * Safe without a window: SDL_PollEvent reports no events when video is not
     * initialised, and the app is not suspended, so this only reaches the
     * automation block. */
    if (!gb_platform_poll_events(ctx)) {
        return false;
    }
    gb_reset_frame(ctx);
    ctx->stopped = 0;
    uint32_t guard = 0;
    while (!ctx->frame_done) {
        gb_run_cycles(ctx, 0xFFFFFFFFu);
        if (++guard > 100000u) {
            return false;
        }
    }
    return true;
}

/* Advance past frame_done so the capture sees the guest's next-frame setup.
 *
 * gb_run_cycles loops on `while (!ctx->frame_done)`, so a call made while the
 * flag is set returns immediately without running anything and without ever
 * consulting its cycle budget. gb_reset_frame clears the flag without touching
 * the PPU's LY, so re-arming the loop does not rewind the scanline. */

static double score_render(const GBPresentationFrame* frame, const uint32_t* rendered) {
    uint64_t matched = 0;
    for (uint32_t y = 0; y < SCREEN_HEIGHT; ++y) {
        for (uint32_t x = 0; x < SCREEN_WIDTH; ++x) {
            const size_t i = (size_t)y * SCREEN_WIDTH + x;
            if (frame->hardware.accurate_pixels[i] == (uint16_t)rendered[i]) {
                matched++;
            }
        }
    }
    return (double)matched * 100.0 / ((double)SCREEN_WIDTH * SCREEN_HEIGHT);
}

static bool sweep_capture_points(
    GBContext* ctx, GBPresentationSource* source, GBPresentationScene* scene, GBPortFrame* ui,
    NativePresenter* presenter, uint32_t* surface, uint32_t frame_limit, const uint32_t* drains,
    size_t drain_count) {
    size_t* wins = calloc(drain_count, sizeof(size_t));
    double* best = calloc(frame_limit != 0 ? frame_limit : 1u, sizeof(double));
    if (wins == NULL || best == NULL) {
        return false;
    }
    for (uint32_t index = 0; index < frame_limit; ++index) {
        if (!run_one_frame(ctx)) {
            return false;
        }
        if (!gb_context_save_state_file(ctx, SWEEP_STATE_PATH)) {
            return false;
        }
        double frame_best = -1.0;
        size_t frame_best_index = 0;
        for (size_t d = 0; d < drain_count; ++d) {
            if (!gb_context_load_state_file(ctx, SWEEP_STATE_PATH)) {
                return false;
            }
            drain_after_frame(ctx, drains[d]);
            GBPresentationFrame probe;
            if (gbrt_presentation_capture(source, scene, ui, &probe) != GB_PRESENTATION_OK) {
                continue;
            }
            if (!native_presenter_render_background(presenter, &probe, 0u, 0u, true)) {
                continue;
            }
            const double score = score_render(&probe, surface);
            if (score > frame_best) {
                frame_best = score;
                frame_best_index = d;
            }
        }
        if (frame_best >= 0.0) {
            wins[frame_best_index]++;
            best[index] = frame_best;
        }
        if (!gb_context_load_state_file(ctx, SWEEP_STATE_PATH)) {
            return false;
        }
    }
    printf("\ncapture-point sweep: %u frames x %zu drains (UNVERIFIED, see logs)\n", frame_limit,
           drain_count);
    for (size_t d = 0; d < drain_count; ++d) {
        printf("    drain %-8u won %zu frames\n", drains[d], wins[d]);
    }
    free(wins);
    free(best);
    return true;
}

static void usage(void) {
    printf(
        "ghost-babel-remaster: native presentation host (dumps frames, counts nothing)\n"
        "\n"
        "  --rom-sha256 HEX     optional; verify against expected ROM SHA-256\n"
        "  --limit-frames N     frames to run; 600 headless, unlimited in a window\n"
        "  --headless         dump harness instead of a window\n"
        "  --classic-width    play at 160px instead of the 256px widescreen\n"
        "  --screenshot-frames A,B   platform-written PPMs of those frames\n"
        "  --screenshot-prefix P     where those PPMs go\n"
        "  --widescreen      256px view, HUD on the right (the default in a window)\n"
        "  --perf-report        print loop timing and audio counters every 10s\n"
        "  --no-column-cache   rebuild the margins from the map instead of\n"
        "                        replaying columns the PPU drew on screen\n"
        "  --drain-cycles N     advance N cycles past frame_done before capturing\n"
        "  --input SCRIPT      cycle-anchored input (see tools/trajectories)\n"
        "  --oam-shadow-at N    search WRAM for the OAM staging page at frame N\n"
        "  --load-state-file P    resume from an emulated save state\n"
        "  --dump-palettes         print every BG and OBJ palette entry\n"
        "  --dump-map-row N       print the BG tile IDs of tile row N\n"
        "  --oam-motion N        report OAM movement per block of N frames\n"
        "  --oam-trace P,S,N     write N raw (WRAM,OAM) records from frame S to P\n"
        "  --dump-stride N      dump every Nth frame\n"
        "  --dump-frames A,B    dump specific frames (evenly spaced input)\n"
        "  --dump-dir PATH      where ref_*.ppm, nat_*.ppm and meta.csv go\n"
        "  --dump-tiles DIR     write the live VRAM bank 1 tileset per room, as\n"
        "                        area_XX_room_YY_tiles.bin (6144 bytes)\n"
        "  --room-record PATH   CSV: one row per room the guest enters, with the\n"
        "                        record resolved from the cartridge and the camera\n"
        "                        limits the loader wrote to WRAM\n"
        "  --room-map PATH      CSV: per frame, the hash of the room's tile-index\n"
        "                        table ($D000, WRAM bank 5) and the atlas pointer\n"
        "                        ($CBFF). A hash that never moves says the table is\n"
        "                        a room, not a camera window\n"
        "  --sweep-drains A,B   per-frame capture-point sweep (UNVERIFIED)\n"
        "  --sweep-frames N     frames for the sweep\n"
        "  --hw-trace P,S,N     write N PPU snapshots (patterns, maps, attributes,\n"
        "                        palettes, reference pixels) from frame S to P\n");
}

/* The PPU snapshot, written with an explicit field order rather than the raw
 * struct: the reader is Python, and struct padding is a bug waiting to happen.
 * `accurate_pixels` goes along because it is the frame the presenter had to
 * match, so an offline renderer can be scored against it without a second
 * capture point. */
#define HW_TRACE_MAGIC 0x48574231u /* "HWB1" */
static bool write_hw_record(FILE* file, const GBPresentationHardwareSnapshot* hw) {
    const uint8_t regs[8] = {hw->lcdc, hw->scx, hw->scy, hw->wy, hw->wx, 0u, 0u, 0u};
    if (fwrite(regs, 1u, sizeof(regs), file) != sizeof(regs)) {
        return false;
    }
    static const size_t chunks[4] = {
        GB_PRESENTATION_TILE_DATA_BYTES,
        GB_PRESENTATION_TILE_MAP_BYTES,
        GB_PRESENTATION_TILE_MAP_BYTES,
        0x40u,
    };
    for (unsigned bank = 0; bank < 2u; ++bank) {
        if (fwrite(hw->tile_data[bank], 1u, chunks[0], file) != chunks[0] ||
            fwrite(hw->tile_ids[bank], 1u, chunks[1], file) != chunks[1] ||
            fwrite(hw->tile_attributes[bank], 1u, chunks[2], file) != chunks[2]) {
            return false;
        }
    }
    if (fwrite(hw->background_palettes, 1u, chunks[3], file) != chunks[3] ||
        fwrite(hw->object_palettes, 1u, chunks[3], file) != chunks[3] ||
        fwrite(hw->accurate_pixels, 1u, sizeof(hw->accurate_pixels), file) !=
            sizeof(hw->accurate_pixels) ||
        fwrite(hw->scanline_lcdc, 1u, sizeof(hw->scanline_lcdc), file) !=
            sizeof(hw->scanline_lcdc)) {
        return false;
    }
    return true;
}

static void format_sha256_hex(const uint8_t digest[32], char out_hex[65]) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32u; ++i) {
        out_hex[i * 2u] = hex[(digest[i] >> 4) & 0x0Fu];
        out_hex[i * 2u + 1u] = hex[digest[i] & 0x0Fu];
    }
    out_hex[64] = '\0';
}

int main(int argc, char** argv) {
    /* The harness has a budget; the product does not. A windowed run must play
     * until the user closes it, so the frame limit only applies when one was
     * asked for. Defaulting both to 600 meant the game quit by itself after ten
     * seconds, which reads as a crash rather than as a limit. */
    uint32_t limit_frames = 600;
    int limit_explicit = 0;
    const char* rom_sha256 = NULL;
    const char* dump_dir = NULL;
    const char* dump_tiles_dir = NULL;
    const char* room_record_path = NULL;
    const char* room_map_path = NULL;
    bool perf_report_flag = false;
    const char* sweep_drains = NULL;
    uint32_t dump_stride = 0;
    uint32_t dump_frames[64];
    size_t dump_frames_count = 0;
    uint32_t sweep_frames = 300;
    uint32_t drain_cycles = 4560u;

    const char* input_arg = NULL;
    const char* load_state_file = NULL;
    const char* trace_path = NULL;
    uint32_t trace_start = 0;
    uint32_t trace_count = 0;
    const char* hw_trace_path = NULL;
    uint32_t hw_trace_start = 0;
    uint32_t hw_trace_count = 0;
    int widescreen = false;
    int headless = 0;
    const char* screenshot_prefix = NULL;
    const char* screenshot_frames = NULL;
    int width_explicit = 0;
    int map_row = -1;
    bool column_cache = true;
    bool room_map = true;
    int dump_palettes = 0;
    uint32_t motion_block = 0;
    uint8_t previous_oam[OAM_BYTES];
    int search_oam_shadow = 0;
    uint32_t search_frame = 0;
    uint32_t view_width = SCREEN_WIDTH;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--limit-frames") == 0 && i + 1 < argc) {
            limit_frames = (uint32_t)strtoul(argv[++i], NULL, 10);
            limit_explicit = 1;
        } else if (strcmp(argv[i], "--rom-sha256") == 0 && i + 1 < argc) {
            rom_sha256 = argv[++i];
        } else if (strcmp(argv[i], "--drain-cycles") == 0 && i + 1 < argc) {
            drain_cycles = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_arg = argv[++i];
        } else if (strcmp(argv[i], "--no-column-cache") == 0) {
            /* Read the map for the margins instead of replaying what the PPU
             * drew while those columns were inside the game's own view. The map
             * was measured against the guest's own pixels and loses outdoors, so
             * this is not a build anybody should play: it exists so the audit can
             * put the two side by side and count where they disagree. */
            column_cache = false;
        } else if (strcmp(argv[i], "--no-room-map") == 0) {
            /* Keep the margin on the older sources. The room map is the one
             * that describes the whole room, so this exists only so the audit
             * can measure what it changed. */
            room_map = false;
        } else if (strcmp(argv[i], "--dump-palettes") == 0) {
            dump_palettes = 1;
        } else if (strcmp(argv[i], "--dump-map-row") == 0 && i + 1 < argc) {
            map_row = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--oam-motion") == 0 && i + 1 < argc) {
            /* Rank blocks of frames by how much the OAM actually moves.
             *
             * Correlation needs motion. A window where the sprites sit still
             * agrees trivially and hides everything, so before tracing memory
             * it is worth knowing which part of a session is busy. */
            motion_block = (uint32_t)strtoul(argv[++i], NULL, 10);
            if (motion_block == 0u) {
                usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--load-state-file") == 0 && i + 1 < argc) {
            load_state_file = argv[++i];
        } else if (strcmp(argv[i], "--oam-trace") == 0 && i + 1 < argc) {
            /* "path,start,count" as one argument: raw memory records for
             * offline correlation in tools/trace_oam_source.py. */
            char* cursor = argv[++i];
            trace_path = cursor;
            char* comma = strchr(cursor, ',');
            if (comma == NULL) {
                usage();
                return 2;
            }
            *comma = '\0';
            trace_start = (uint32_t)strtoul(comma + 1, &cursor, 10);
            if (*cursor != ',') {
                usage();
                return 2;
            }
            trace_count = (uint32_t)strtoul(cursor + 1, NULL, 10);
            if (trace_count == 0u) {
                usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--hw-trace") == 0 && i + 1 < argc) {
            /* "path,start,count": the PPU snapshot as the capture handed it to
             * the presenter - the two tile banks, both maps, the attributes,
             * the palettes and the reference pixels - for offline correlation
             * in tools/. Read-only, like --oam-trace. */
            char* cursor = argv[++i];
            hw_trace_path = cursor;
            char* comma = strchr(cursor, ',');
            if (comma == NULL) {
                usage();
                return 2;
            }
            *comma = '\0';
            hw_trace_start = (uint32_t)strtoul(comma + 1, &cursor, 10);
            if (*cursor != ',') {
                usage();
                return 2;
            }
            hw_trace_count = (uint32_t)strtoul(cursor + 1, NULL, 10);
            if (hw_trace_count == 0u) {
                usage();
                return 2;
            }
        } else if (strcmp(argv[i], "--oam-shadow-at") == 0 && i + 1 < argc) {
            search_frame = (uint32_t)strtoul(argv[++i], NULL, 10);
            search_oam_shadow = 1;
        } else if (strcmp(argv[i], "--dump-frames") == 0 && i + 1 < argc) {
            char* cursor = (char*)argv[++i];
            while (*cursor != '\0' && dump_frames_count < 64u) {
                dump_frames[dump_frames_count++] = (uint32_t)strtoul(cursor, &cursor, 10);
                if (*cursor == ',') {
                    cursor++;
                }
            }
        } else if (strcmp(argv[i], "--screenshot-prefix") == 0 && i + 1 < argc) {
            screenshot_prefix = argv[++i];
        } else if (strcmp(argv[i], "--screenshot-frames") == 0 && i + 1 < argc) {
            screenshot_frames = argv[++i];
        } else if (strcmp(argv[i], "--perf-report") == 0) {
            /* Print the loop's own numbers every ten seconds and once more at
             * the end. Nothing about the pacing is visible from outside, and
             * the platform's overlay only speaks about audio. */
            perf_report_flag = true;
            g_perf_report = true;
        } else if (strcmp(argv[i], "--headless") == 0) {
            headless = 1;
        } else if (strcmp(argv[i], "--classic-width") == 0) {
            width_explicit = 1;
        } else if (strcmp(argv[i], "--widescreen") == 0) {
            /* The remaster's default composition.
             *
             * The 160px game view sits centred, with WIDESCREEN_MARGIN_PX of
             * scene on each side, and the HUD stays pinned to the right edge.
             * The two need different shifts, which is why there are two numbers
             * and not one: see native_presenter.c.
             *
             * Left as an explicit flag rather than a default so the audit keeps
             * measuring the reference 160px composition without arguments. */
            view_width = SCREEN_WIDTH + 2u * WIDESCREEN_MARGIN_PX;
            widescreen = true;
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            view_width = (uint32_t)strtoul(argv[++i], NULL, 10);
            width_explicit = 1;
            if (view_width < SCREEN_WIDTH) {
                view_width = SCREEN_WIDTH;
            }
        } else if (strcmp(argv[i], "--camera-x") == 0 && i + 1 < argc) {
            (void)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--dump-stride") == 0 && i + 1 < argc) {
            dump_stride = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--dump-dir") == 0 && i + 1 < argc) {
            dump_dir = argv[++i];
        } else if (strcmp(argv[i], "--dump-tiles") == 0 && i + 1 < argc) {
            dump_tiles_dir = argv[++i];
        } else if (strcmp(argv[i], "--room-record") == 0 && i + 1 < argc) {
            room_record_path = argv[++i];
        } else if (strcmp(argv[i], "--room-map") == 0 && i + 1 < argc) {
            room_map_path = argv[++i];
        } else if (strcmp(argv[i], "--sweep-drains") == 0 && i + 1 < argc) {
            sweep_drains = argv[++i];
        } else if (strcmp(argv[i], "--sweep-frames") == 0 && i + 1 < argc) {
            sweep_frames = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else {
            usage();
            return 2;
        }
    }

    if ((dump_stride != 0 || dump_frames_count != 0) && dump_dir == NULL) {
        fprintf(stderr, "remaster: dumping needs --dump-dir\n");
        return 2;
    }

    GBConfig config = *baserom_default_config();
    config.native_presentation_enabled = true;
    /* Reaching real gameplay needs the recorded session, not a cold boot: a
     * cold start spends its first thousand frames on logos. The script goes in
     * the config, which is where the runtime already reads it. */
    if (input_arg != NULL && !gb_platform_set_input_script(input_arg)) {
        fprintf(stderr, "remaster: invalid input script\n");
        return 2;
    }
    /* The product is the widescreen; the harness measures the reference
     * composition and must stay at 160 unless told otherwise. Decided here,
     * after parsing, so an explicit --width wins either way. */
    if (!width_explicit && !widescreen && !headless) {
        widescreen = true;
        view_width = SCREEN_WIDTH + 2u * WIDESCREEN_MARGIN_PX;
    }

    const bool cgb_mode = !config.cgb_compatibility_mode;
    /* Zeroed, so the extent starts "no room yet" and the widescreen opens with
     * the game's own 160 pixels until the room loader has run. A stack
     * CameraExtent would carry whatever was in that memory, and its valid flag
     * would decide by luck. */
    CameraExtent extent;
    CameraMargin margin;
    memset(&extent, 0, sizeof(extent));
    memset(&margin, 0, sizeof(margin));
    const uint32_t margin_left_cap = WIDESCREEN_BAND_MAX_PX;

    GBContext* ctx = gb_context_create(&config);
    if (ctx == NULL) {
        fprintf(stderr, "remaster: could not create context\n");
        return 1;
    }
    baserom_init(ctx);

    char computed_rom_sha256[65];
    if (rom_sha256 == NULL) {
        if (ctx->rom == NULL || ctx->rom_size == 0u) {
            fprintf(stderr, "remaster: no ROM loaded to verify\n");
            gb_context_destroy(ctx);
            return 1;
        }
        uint8_t digest[32];
        gbrt_sha256(ctx->rom, (size_t)ctx->rom_size, digest);
        format_sha256_hex(digest, computed_rom_sha256);
        rom_sha256 = computed_rom_sha256;
    }

    GBPresentationSource source;
    const GBPresentationConfig presentation_config = {
        .abi_version = GB_PRESENTATION_ABI_VERSION,
        .rom_sha256 = rom_sha256,
        .rom_size = (size_t)ctx->rom_size,
        .headless = true,
    };
    const GBPresentationStatus init_status =
        gbrt_presentation_source_init(&source, ctx, &presentation_config);
    if (init_status != GB_PRESENTATION_OK) {
        fprintf(stderr, "remaster: source init failed: %s\n", status_name(init_status));
        return 1;
    }

    GBPresentationScene scene;
    memset(&scene, 0, sizeof(scene));
    scene.abi_version = GB_PRESENTATION_ABI_VERSION;

    GBPortFrame ui;
    memset(&ui, 0, sizeof(ui));
    ui.abi_version = GB_PORT_ABI_VERSION;
    ui.canvas_width = SCREEN_WIDTH;
    ui.canvas_height = SCREEN_HEIGHT;

    /* The colour index and attribute-priority buffers are indexed by the same
     * surface column the background pass writes, margin included: the object
     * pass needs the margin's own priority, not just the picture's. */
    const size_t view_pixels = (size_t)view_width * SCREEN_HEIGHT;
    uint32_t* surface = calloc(view_pixels, sizeof(uint32_t));
    uint32_t* previous_render = calloc(view_pixels, sizeof(uint32_t));
    uint8_t* bg_color_index = calloc(view_pixels, sizeof(uint8_t));
    uint8_t* bg_attr_priority = calloc(view_pixels, sizeof(uint8_t));
    if (surface == NULL || previous_render == NULL || bg_color_index == NULL ||
        bg_attr_priority == NULL) {
        fprintf(stderr, "remaster: out of memory\n");
        return 1;
    }

    NativePresenter presenter;
    native_presenter_init(&presenter, view_width, SCREEN_HEIGHT, surface, view_width,
                          bg_color_index, bg_attr_priority);
    if (dump_tiles_dir) {
        native_presenter_set_dump_tiles_dir(&presenter, dump_tiles_dir);
    }

    /* A save state is the whole machine - WRAM, VRAM, OAM, registers - so it
     * lands the analysis in the middle of a scene instead of replaying to reach
     * it. That matters because the interesting question, where the game keeps
     * its enemies, can only be asked while enemies are on screen, and booting
     * there costs thousands of frames of menus.
     *
     * Reading state does not write back: nothing here mutates the guest. */
    if (load_state_file != NULL) {
        if (!gb_context_load_state_file(ctx, load_state_file)) {
            fprintf(stderr, "remaster: cannot load state file %s\n", load_state_file);
            return 1;
        }
        /* Input scripts are anchored to absolute cycle counts. After a state
         * load the clock is already deep into the run, so a script written
         * from cycle zero fires entirely in the past and the player never
         * moves - which looks exactly like input being ignored. Print the
         * anchor so the script can be written against it. */
        printf("  state loaded           %s\n", load_state_file);
        printf("    total cycles         %llu  (frame %llu)\n",
               (unsigned long long)ctx->total_cycles,
               (unsigned long long)(ctx->total_cycles / 70224ull));
        printf("    input scripts must start at or after this cycle\n");
    }

    if (!headless) {
        uint32_t* present_pixels = calloc(view_pixels, sizeof(uint32_t));
        if (present_pixels == NULL) {
            fprintf(stderr, "remaster: out of memory\n");
            return 1;
        }
        const int rc = run_windowed(ctx, &source, &scene, &ui, &presenter, surface, present_pixels,
                                    view_width,
                                    cgb_mode, limit_explicit ? limit_frames : UINT32_MAX,
                                    margin, &extent, margin_left_cap, column_cache, room_map,
                                    screenshot_prefix, screenshot_frames, drain_cycles,
                                    perf_report_flag);
        free(surface);
        free(present_pixels);
        free(previous_render);
        free(bg_color_index);
        free(bg_attr_priority);
        gb_context_destroy(ctx);
        return rc;
    }

    if (sweep_drains != NULL) {
        uint32_t drains[SWEEP_MAX_DRAINS];
        size_t drain_count = 0;
        char* cursor = (char*)sweep_drains;
        while (*cursor != '\0' && drain_count < SWEEP_MAX_DRAINS) {
            drains[drain_count++] = (uint32_t)strtoul(cursor, &cursor, 10);
            if (*cursor == ',') {
                cursor++;
            }
        }
        const bool ok = sweep_capture_points(ctx, &source, &scene, &ui, &presenter, surface,
                                             sweep_frames, drains, drain_count);
        return ok ? 0 : 1;
    }

    FILE* meta = NULL;
    native_presenter_enable_column_cache(&presenter, column_cache);
    native_presenter_enable_room_map(&presenter, room_map);

    if (dump_dir != NULL) {
        char path[512];
        snprintf(path, sizeof(path), "%s/meta.csv", dump_dir);
        meta = fopen(path, "w");
        if (meta != NULL) {
            fputs(META_HEADER, meta);
        }
    }

    /* Room identity, one row per change. See room_record_row. */
    FILE* room_out = NULL;
    if (room_record_path != NULL) {
        room_out = fopen(room_record_path, "w");
        if (room_out == NULL) {
            fprintf(stderr, "remaster: cannot write room record to %s\n", room_record_path);
            return 1;
        }
        fputs(ROOM_HEADER, room_out);
    }

    /* Whether the room's index table stands still. See room_map_row. */
    FILE* room_map_out = NULL;
    if (room_map_path != NULL) {
        room_map_out = fopen(room_map_path, "w");
        if (room_map_out == NULL) {
            fprintf(stderr, "remaster: cannot write room map to %s\n", room_map_path);
            return 1;
        }
        fputs("frame,area,room,cam_x,cam_y,table_hash,table_nonzero,atlas\n", room_map_out);
    }
    uint32_t room_rows = 0;
    uint32_t room_resolved = 0;
    uint32_t room_agreed = 0;
    uint32_t room_camera_in_room = 0;
    uint32_t room_camera_checked = 0;
    bool have_room = false;
    uint8_t last_area = 0;
    uint8_t last_room = 0;
    /* The presenter's room, kept apart from the CSV's identity because they
     * answer different questions: the CSV records every change it sees, while
     * the presenter only switches once WRAM and cartridge agree. */
    PresenterRoomSync room_sync;
    memset(&room_sync, 0, sizeof(room_sync));
    int last_agreement = ROOM_AGREE_UNRESOLVED;

    StructuralCounters stats;
    memset(&stats, 0, sizeof(stats));
    GBPresentationFrame frame;
    GBPresentationStatus last_status = GB_PRESENTATION_OK;
    uint32_t dumped = 0;

    /* Raw memory trace.
     *
     * Finding who writes the staging page by reading disassembly is slow: this
     * ROM decompresses its own code, so static analysis has no symbols to read.
     * Instead record the machine state and let the correlation be empirical.
     * A source field is a WRAM address whose value tracks a fixed byte of the
     * staging page across every sampled frame - a coincidence cannot survive
     * a hundred frames of moving sprites. The host only writes bytes; all
     * interpretation happens in tools/, keeping one place where the analysis
     * lives and one place where a bug can hide. */
    FILE* trace_file = NULL;
    if (trace_path != NULL) {
        trace_file = fopen(trace_path, "wb");
        if (trace_file == NULL) {
            fprintf(stderr, "remaster: cannot write trace to %s\n", trace_path);
            return 1;
        }
    }
    FILE* hw_file = NULL;
    if (hw_trace_path != NULL) {
        hw_file = fopen(hw_trace_path, "wb");
        if (hw_file == NULL) {
            fprintf(stderr, "remaster: cannot write hardware trace to %s\n", hw_trace_path);
            return 1;
        }
        const uint32_t header[2] = {HW_TRACE_MAGIC, (uint32_t)1u};
        if (fwrite(header, 1u, sizeof(header), hw_file) != sizeof(header)) {
            fprintf(stderr, "remaster: hardware trace write failed\n");
            fclose(hw_file);
            return 1;
        }
    }

    bool have_previous_oam = false;
    uint32_t block_oam_changes = 0;
    uint32_t block_frame = 0;

    SyncedGuestState* prev_state = calloc(1u, sizeof(*prev_state));
    SyncedGuestState* post_state = calloc(1u, sizeof(*post_state));
    SyncedGuestState* render_state = calloc(1u, sizeof(*render_state));
    if (prev_state == NULL || post_state == NULL || render_state == NULL) {
        fprintf(stderr, "remaster: out of memory\n");
        free(prev_state);
        free(post_state);
        free(render_state);
        return 1;
    }

    for (uint32_t index = 0; index < limit_frames; ++index) {
        const uint64_t pre_cycles = ctx->total_cycles;
        if (prev_state->valid && prev_state->total_cycles != pre_cycles) {
            prev_state->valid = false;
            room_sync.known = false;
            native_presenter_clear_column_cache(&presenter);
        }
        if (!run_one_frame(ctx)) {
            fprintf(stderr, "remaster: frame %u did not complete\n", index);
            free(prev_state);
            free(post_state);
            free(render_state);
            return 1;
        }
        drain_after_frame(ctx, drain_cycles);

        const GBPresentationStatus status = gbrt_presentation_capture(&source, &scene, &ui, &frame);
        if (status != GB_PRESENTATION_OK) {
            prev_state->valid = false;
            last_status = status;
            stats.captures_refused++;
            continue;
        }
        stats.frames_captured++;
        capture_guest_state(ctx, &frame, room_map, post_state);
        build_render_state(prev_state, post_state, pre_cycles, drain_cycles, render_state);
        const GBPresentationHardwareSnapshot* hw = &render_state->frame.hardware;

        /* The room the guest is in, for the margin's map. Decoding is done on a
         * change only; the per-frame cost is one table read. */
        if (room_map) {
            sync_presenter_room(&presenter, ctx->rom, ctx->rom_size, render_state, &room_sync);
        }

        /* The absolute frame, not the loop index: a state load starts the loop
         * at zero with the clock already deep into the run, and a room row that
         * cannot be placed in the session is not worth writing. */
        if (room_out != NULL) {
            const RoomObservation obs = render_state->room_obs;
            const bool identity_changed =
                !have_room || obs.area != last_area || obs.room != last_room;
            if (identity_changed || obs.agreement != last_agreement) {
                write_room_row(room_out, (uint32_t)(render_state->total_cycles / 70224ull), &obs);
                room_rows++;
                if (obs.resolved) {
                    room_resolved++;
                }
                if (obs.agreement == ROOM_AGREE_SAME) {
                    room_agreed++;
                }
                if (obs.resolved) {
                    room_camera_checked++;
                    if (obs.camera_in_room) {
                        room_camera_in_room++;
                    }
                }
                have_room = true;
                last_area = obs.area;
                last_room = obs.room;
                last_agreement = obs.agreement;
            }
        }

        if (room_map_out != NULL) {
            room_map_row(room_map_out, ctx, (uint32_t)(ctx->total_cycles / 70224ull));
        }

        if (motion_block != 0u) {
            if (have_previous_oam) {
                for (uint32_t k = 0; k < OAM_BYTES; ++k) {
                    if (ctx->oam[k] != previous_oam[k]) {
                        block_oam_changes++;
                    }
                }
            }
            memcpy(previous_oam, ctx->oam, OAM_BYTES);
            have_previous_oam = true;
            if (++block_frame == motion_block) {
                printf("  oam-motion  frame %8u..%8u  %u byte changes\n",
                       index - motion_block + 1u, index, block_oam_changes);
                block_oam_changes = 0;
                block_frame = 0;
            }
        }

        if (trace_file != NULL && index >= trace_start &&
            index < trace_start + trace_count) {
            if (fwrite(ctx->wram, 1u, WRAM_TOTAL, trace_file) != WRAM_TOTAL ||
                fwrite(ctx->oam, 1u, OAM_BYTES, trace_file) != OAM_BYTES) {
                fprintf(stderr, "remaster: trace write failed\n");
                fclose(trace_file);
                free(prev_state);
                free(post_state);
                free(render_state);
                return 1;
            }
            stats.frames_traced++;
        }

        if (hw_file != NULL && index >= hw_trace_start &&
            index < hw_trace_start + hw_trace_count) {
            if (!write_hw_record(hw_file, hw)) {
                fprintf(stderr, "remaster: hardware trace write failed\n");
                fclose(hw_file);
                free(prev_state);
                free(post_state);
                free(render_state);
                return 1;
            }
            stats.frames_traced++;
        }

        /* Locate the OAM shadow in WRAM.
         *
         * Every GB game stages its 40 objects in a WRAM page and copies that
         * page to OAM with DMA, because the CPU cannot write OAM while the PPU
         * owns it. So the enemy table this project needs for world-space
         * rendering already exists in WRAM, one step upstream of OAM - and the
         * address is discoverable by search rather than by reading disassembly.
         * This is the cheap half of the semantic layer: find the staging page,
         * then work backwards from whoever fills it. */
        if (search_oam_shadow && index == search_frame) {
            uint32_t best_offset = 0;
            uint32_t best_hits = 0;
            for (uint32_t base = 0; base + OAM_BYTES <= WRAM_TOTAL; ++base) {
                uint32_t hits = 0;
                for (uint32_t k = 0; k < OAM_BYTES; ++k) {
                    if (ctx->wram[base + k] == ctx->oam[k]) {
                        hits++;
                    }
                }
                if (hits > best_hits) {
                    best_hits = hits;
                    best_offset = base;
                }
            }
            printf("  OAM shadow search at frame %u\n", index);
            printf("    best WRAM offset 0x%04X matches %u/%u bytes (%.1f%%)\n", best_offset,
                   best_hits, OAM_BYTES, 100.0 * best_hits / OAM_BYTES);
            if (best_offset < 0x1000u) {
                printf("    -> WRAM bank 0, game address $%04X\n", 0xC000u + best_offset);
            } else if (best_offset < 0x4000u) {
                printf("    -> WRAM bank %u, game address $%04X\n", best_offset / 0x1000u,
                       0xD000u + (best_offset % 0x1000u));
            }
            printf("    (160/160 means the staging page was found; a high percentage on\n");
            printf("     some other address means the object table has not been staged yet)\n");
        }

        /* Index 0 has no previous render yet: the buffer is still zeroed, so
         * comparing it would report a perfect mismatch that says nothing about
         * the renderer. Start at 1. */
        /* Checkpoint dumps come in two shapes: an explicit list, or a stride. The
         * explicit list is served by checking membership directly - the
         * stride shortcut it used to take divided by a gap that stays zero for
         * a one-element list, which killed the process with an illegal
         * instruction on --dump-frames 600. */
        bool should_dump = false;
        if (dump_dir != NULL) {
            for (size_t k = 0; k < dump_frames_count; ++k) {
                if (dump_frames[k] == index) {
                    should_dump = true;
                    break;
                }
            }
            if (!should_dump && dump_stride != 0u && index > 0u && (index % dump_stride) == 0u) {
                should_dump = true;
            }
        }
        /* Render before anything is written out. The dump used to come first
         * and read `previous_render`, a copy of the previous iteration's
         * surface, which is how the labels were kept aligned back when the
         * presenter reconstructed the frame from state and lagged the PPU by
         * one. Copying the game's columns from the PPU's own framebuffer
         * removed the lag, and the lag's only remaining job was to make every
         * cross-fade compare against the wrong image: 10.96% on the Konami
         * fade at frame 300, of a screen that was pixel-exact. */
        /* The margin is the product's composition and only applies to it. The
         * audit measures the reference 160px picture, so a dump without
         * --widescreen keeps the view exactly where it was. */
        if (widescreen) {
            if (camera_extent_observe(&extent, hw->scx, hw->scy, render_state->bounds)) {
                native_presenter_clear_column_cache(&presenter);
            }
            camera_margin_for(&extent, render_state->camera_x, margin_left_cap, &margin);
            native_presenter_set_source_node(
                &presenter, render_state->source_node < 12u ? render_state->source_node : 0xFFu);
            native_presenter_set_actors(&presenter, &render_state->actors);
        } else {
            memset(&margin, 0, sizeof(margin));
            native_presenter_set_actors(&presenter, NULL);
        }
        if (!native_presenter_render_background(&presenter, &render_state->frame, margin.left,
                                                margin.right, cgb_mode)) {
            fprintf(stderr, "remaster: render failed at %u\n", index);
            free(prev_state);
            free(post_state);
            free(render_state);
            return 1;
        }
        memcpy(previous_render, surface, view_pixels * sizeof(uint32_t));

        if (should_dump) {
            char ref_path[512];
            char nat_path[512];
            snprintf(ref_path, sizeof(ref_path), "%s/ref_%05u.ppm", dump_dir, index);
            snprintf(nat_path, sizeof(nat_path), "%s/nat_%05u.ppm", dump_dir, index);
            /* The surface just rendered from this frame, so both files
             * describe the same image. */
            if (write_reference_ppm(&render_state->frame, ref_path) &&
                write_ppm(nat_path, surface, view_width)) {
                uint32_t oam_live = 0;
                for (size_t s = 0; s < GB_PRESENTATION_SPRITE_COUNT; ++s) {
                    if (hw->sprites[s].raw_y != 0u) {
                        oam_live++;
                    }
                }
                if (meta != NULL) {
                    /* Is the background map actually populated? A renderer that
                     * emits one flat colour per frame usually means it is
                     * sampling an empty map, and this is how that gets told apart
                     * from a wrong palette. */
                    const unsigned map_index = (hw->lcdc & 0x08u) != 0u ? 1u : 0u;
                    uint32_t map_unique = 0;
                    uint8_t seen[256];
                    memset(seen, 0, sizeof(seen));
                    for (uint32_t k = 0; k < GB_PRESENTATION_TILE_MAP_BYTES; ++k) {
                        seen[hw->tile_ids[map_index][k]] = 1u;
                    }
                    for (uint32_t k = 0; k < 256u; ++k) {
                        map_unique += seen[k];
                    }
                    uint32_t tile_nonzero = 0;
                    for (uint32_t k = 0; k < GB_PRESENTATION_TILE_DATA_BYTES; ++k) {
                        if (hw->tile_data[0][k] != 0u) {
                            tile_nonzero++;
                        }
                    }
                    /* Both VRAM banks, and how many visible cells actually
                     * select bank 1. A frame that renders one flat colour is a
                     * frame where every cell resolves to colour index 0, and
                     * the two usual causes are an empty map or patterns in the
                     * bank nobody is reading. Counting both banks tells those
                     * apart without touching the renderer. */
                    uint32_t tile1_nonzero = 0;
                    for (uint32_t k = 0; k < GB_PRESENTATION_TILE_DATA_BYTES; ++k) {
                        if (hw->tile_data[1][k] != 0u) {
                            tile1_nonzero++;
                        }
                    }
                    uint32_t attr_nonzero = 0;
                    for (uint32_t k = 0; k < GB_PRESENTATION_TILE_MAP_BYTES; ++k) {
                        if (hw->tile_attributes[map_index][k] != 0u) {
                            attr_nonzero++;
                        }
                    }
                    /* CGB layout: 8 palettes, 2 little-endian bytes per colour. */
                    const uint32_t pal0_c0 = (uint32_t)hw->background_palettes[0] |
                                             ((uint32_t)hw->background_palettes[1] << 8);
                    const uint32_t pal0_c1 = (uint32_t)hw->background_palettes[2] |
                                             ((uint32_t)hw->background_palettes[3] << 8);
                    /* With LCDC bit 4 clear the tile data window is signed:
                     * IDs 0-127 map forward from $9000 and 128-255 map
                     * backward from $9000 too. An ID above 127 read as if it
                     * were unsigned lands past $97F0, in memory the game never
                     * wrote, and comes back blank. So the range of IDs the
                     * visible cells actually use decides whether that matters. */
                    uint32_t tid_min = 255u;
                    uint32_t tid_max = 0u;
                    uint32_t tid_ge128 = 0u;
                    for (uint32_t ty = 0; ty < 18u; ++ty) {
                        for (uint32_t tx = 0; tx < 20u; ++tx) {
                            const uint32_t tid = hw->tile_ids[map_index][ty * 32u + tx];
                            if (tid < tid_min) {
                                tid_min = tid;
                            }
                            if (tid > tid_max) {
                                tid_max = tid;
                            }
                            if (tid >= 128u) {
                                tid_ge128++;
                            }
                        }
                    }
                    /* How many visible cells resolve to an all-zero pattern.
                     *
                     * This is the direct measure of "the renderer is painting
                     * one flat colour": every such cell contributes colour
                     * index 0. When the reference disagrees and this count is
                     * high, the tile lookup is pointing at VRAM the game never
                     * wrote, and the fix is in the addressing rather than in
                     * the palette. */
                    uint32_t visible_blank = 0;
                    for (uint32_t ty = 0; ty < 18u; ++ty) {
                        for (uint32_t tx = 0; tx < 20u; ++tx) {
                            const uint32_t tid = hw->tile_ids[map_index][ty * 32u + tx];
                            const uint32_t base =
                                ((hw->lcdc & 0x10u) != 0u)
                                    ? (tid * 16u)
                                    : (0x1000u +
                                       (uint32_t)((int32_t)(int8_t)(uint8_t)tid * 16));
                            uint32_t nonzero = 0;
                            if (base + 16u <= GB_PRESENTATION_TILE_DATA_BYTES) {
                                for (uint32_t b = 0; b < 16u; ++b) {
                                    if (hw->tile_data[0][base + b] != 0u) {
                                        nonzero++;
                                    }
                                }
                            }
                            if (nonzero == 0u) {
                                visible_blank++;
                            }
                        }
                    }
                    uint8_t seen1[256];
                    memset(seen1, 0, sizeof(seen1));
                    for (uint32_t k = 0; k < GB_PRESENTATION_TILE_MAP_BYTES; ++k) {
                        seen1[hw->tile_ids[1][k]] = 1u;
                    }
                    uint32_t map1_unique = 0;
                    for (uint32_t k = 0; k < 256u; ++k) {
                        map1_unique += seen1[k];
                    }
                    uint32_t visible_nonzero = 0;
                    for (uint32_t ty = 0; ty < 18u; ++ty) {
                        for (uint32_t tx = 0; tx < 20u; ++tx) {
                            if (hw->tile_ids[map_index][ty * 32u + tx] != 0u) {
                                visible_nonzero++;
                            }
                        }
                    }
                    if (dump_palettes) {
                        printf("frame %u lcdc %3u attribute byte of row 10 x=0: 0x%02X\n", index,
                               (unsigned)hw->lcdc, hw->tile_attributes[map_index][10u * 32u]);
                        for (uint32_t pal = 0; pal < 8u; ++pal) {
                            printf("  BG pal %u:", pal);
                            for (uint32_t c = 0; c < 4u; ++c) {
                                const uint16_t raw = (uint16_t)(
                                    (uint16_t)hw->background_palettes[pal * 8u + c * 2u] |
                                    ((uint16_t)hw->background_palettes[pal * 8u + c * 2u + 1u] << 8));
                                printf(" (%3u,%3u,%3u)", (raw & 31u) * 255u / 31u,
                                       ((raw >> 5) & 31u) * 255u / 31u, ((raw >> 10) & 31u) * 255u / 31u);
                            }
                            printf("\n");
                        }
                        for (uint32_t pal = 0; pal < 8u; ++pal) {
                            printf("  OB pal %u:", pal);
                            for (uint32_t c = 0; c < 4u; ++c) {
                                const uint16_t raw = (uint16_t)(
                                    (uint16_t)hw->object_palettes[pal * 8u + c * 2u] |
                                    ((uint16_t)hw->object_palettes[pal * 8u + c * 2u + 1u] << 8));
                                printf(" (%3u,%3u,%3u)", (raw & 31u) * 255u / 31u,
                                       ((raw >> 5) & 31u) * 255u / 31u, ((raw >> 10) & 31u) * 255u / 31u);
                            }
                            printf("\n");
                        }
                    }
                    if (map_row >= 0) {
                        /* All 32 columns, not the 20 the camera can see. The
                         * question this dump exists to answer is what is in the
                         * map *behind* the visible window, so printing only the
                         * visible part answered a different question. */
                        for (int32_t row = map_row - 1; row <= map_row + 1; ++row) {
                            if (row < 0 || row > 31) {
                                continue;
                            }
                            printf("frame %u row %2d ids  :", index, row);
                            for (uint32_t tx = 0; tx < 32u; ++tx) {
                                printf(" %3u", hw->tile_ids[map_index][(uint32_t)row * 32u + tx]);
                            }
                            printf("\n          row %2d attr :", row);
                            for (uint32_t tx = 0; tx < 32u; ++tx) {
                                printf(" %02X", hw->tile_attributes[map_index][(uint32_t)row * 32u + tx]);
                            }
                            printf("\n          row %2d scx=%u scy=%u\n", row, hw->scx, hw->scy);
                        }
                    }
                    uint32_t top_overlay_rows = 0u;
                    uint32_t bottom_status_rows = 0u;
                    count_overlay_rows(hw, &top_overlay_rows, &bottom_status_rows);
                    fprintf(meta,
                            "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                            index, hw->scx, hw->scy, hw->lcdc, hw->wy, hw->wx, oam_live, map_unique,
                            map_index, tile_nonzero, map1_unique, visible_nonzero,
                            (uint32_t)hw->tile_data[0][0] * 256u + hw->tile_data[0][1],
                            tile1_nonzero, pal0_c0 ^ (pal0_c1 << 24), attr_nonzero, tid_min,
                            tid_max, tid_ge128, visible_blank, margin.left, margin.right,
                            bottom_status_rows, top_overlay_rows);
                }
                dumped++;
            }
        }
        SyncedGuestState* tmp = prev_state;
        prev_state = post_state;
        post_state = tmp;

    }

    if (meta != NULL) {
        fclose(meta);
    }
    if (room_out != NULL) {
        fclose(room_out);
    }
    if (room_map_out != NULL) {
        fclose(room_map_out);
    }
    if (trace_file != NULL) {
        fclose(trace_file);
    }
    if (hw_file != NULL) {
        fclose(hw_file);
    }

    stats.sprite_pixels_drawn = presenter.sprite_pixels_drawn;
    stats.sprite_pixels_hidden_by_bg = presenter.sprite_pixels_hidden_by_bg;
    stats.sprites_over_budget = presenter.sprites_over_budget;
    stats.sprite_line_placements = presenter.sprites_considered;
    stats.shadows_admitted = presenter.shadows_admitted;
    stats.shadows_refused = presenter.shadows_refused;
    stats.sprites_exiting = presenter.sprites_exiting;
    stats.sprites_exiting_left = presenter.sprites_exiting_left;
    stats.shadows_blocked_panel = presenter.shadows_blocked_panel;
    stats.shadows_blocked_picture = presenter.shadows_blocked_picture;
    stats.shadows_blocked_room = presenter.shadows_blocked_room;
    stats.shadows_last_frame = presenter.shadows_last_frame;
    stats.shadows_last_x = presenter.shadows_last_x;
    stats.shadows_last_y = presenter.shadows_last_y;
    stats.shadows_last_width = presenter.shadows_last_width;
    stats.shadow_pixels_last_frame = presenter.shadow_pixels_last_frame;
    stats.shadow_box_x0 = presenter.shadow_box_x0;
    stats.shadow_box_x1 = presenter.shadow_box_x1;
    stats.shadow_box_y0 = presenter.shadow_box_y0;
    stats.shadow_box_y1 = presenter.shadow_box_y1;
    stats.shadow_box_frame = presenter.shadow_box_frame;

    printf("ghost-babel-remaster\n");
    printf("  frames run                 %u\n", limit_frames);
    printf("  frames captured            %u\n", stats.frames_captured);
    printf("  captures refused           %u\n", stats.captures_refused);
    printf("  frames dumped              %u\n", dumped);
    printf("  frames traced              %u\n", stats.frames_traced);
    printf("  margin pixels               %u replayed / %u from the map / %u backdrop\n",
           presenter.margin_replayed, presenter.margin_from_map, presenter.margin_backdrop);
    printf("  margin from the room        %u from the cartridge / %u outside it\n",
           presenter.margin_from_room, presenter.margin_room_miss);
    printf("  margin from a neighbour      %u pixels / %u frames placed / %u misses\n",
           presenter.margin_from_neighbour, presenter.neighbour_placements,
           presenter.room.neighbour_misses);
    printf("  learned edges                %u stored / %u placements / %u refused / %u conflicts / %u full\n",
           presenter.learned.recorded, presenter.learned_placements, presenter.learned_refused,
           presenter.learned.conflicts, presenter.learned.full);
    printf("  room map                    %u rooms refused / %u camera placements\n",
           room_sync.refusals, room_sync.placements);
    printf("  room map probes             %u tiles / %u disagreed with the guest\n",
           presenter.room.probed_tiles, presenter.room.probe_mismatches);
    /* The margin that is not about the room. Split out from the three above
     * because it is the one count that says whether the guest was playing at
     * all: no panel means no gameplay, and then the picture is letterboxed into
     * the game's own colour - black when a room is still loaded behind a
     * cutscene, the frame's dominant colour when there is no room at all.
     * Padding past the room's own edge lands here too, with the panel on: the
     * layer's grid is a rectangle and the room inside it is not. */
    printf("  margin letterbox            %u (%s)\n", presenter.margin_letterbox,
           presenter.margin_letterbox != 0u
               ? "the room's own padding, or a cutscene with no panel"
               : "none");
    printf("  margin map bytes            %u written by the guest / %u nothing has written\n",
           presenter.margin_map_fresh, presenter.margin_map_untouched);
    printf("    of the written ones       %u on the trailing side / %u on the leading side\n",
           presenter.margin_map_fresh_left,
           presenter.margin_map_fresh - presenter.margin_map_fresh_left);
    printf("    of the untouched ones     %u on the trailing side / %u on the leading side, "
           "taken as the room\n",
           presenter.margin_map_untouched_left,
           presenter.margin_map_untouched - presenter.margin_map_untouched_left);
    printf("  margin refused              %u (%u trailing / %u leading): the byte holds another "
           "piece of the room\n",
           presenter.margin_map_stale, presenter.margin_map_stale_left,
           presenter.margin_map_stale - presenter.margin_map_stale_left);
    {
        CameraExtentCauses causes;
        camera_extent_causes(&causes);
        printf("  margin cache clears         %u (bounds %u, scy %u, jump %u)\n", causes.total,
               causes.by_bounds, causes.by_scy, causes.by_step);
    }
    printf("  sprite pixels drawn        %u\n", stats.sprite_pixels_drawn);
    printf("  sprite pixels hidden by BG %u\n", stats.sprite_pixels_hidden_by_bg);
    printf("  objects dropped (>10/line) %u\n", stats.sprites_over_budget);
    printf("  sprite-line placements     %u\n", stats.sprite_line_placements);
    printf("  carried shadows            %u admitted / %u refused\n", stats.shadows_admitted,
           stats.shadows_refused);
    printf("  sprites crossing the edge  %u (%u to the left)\n", stats.sprites_exiting,
           stats.sprites_exiting_left);
    printf("  shadow hand-over blocked   %u no panel / %u no picture / %u no room\n",
           stats.shadows_blocked_panel, stats.shadows_blocked_picture,
           stats.shadows_blocked_room);
    printf("  last hand-over             frame %u at x=%d y=%d w=%u\n", stats.shadows_last_frame,
           stats.shadows_last_x, stats.shadows_last_y, stats.shadows_last_width);
    printf("  last shadow pixels         %u at frame %u, columns %d..%d rows %d..%d\n",
           stats.shadow_pixels_last_frame, stats.shadow_box_frame, stats.shadow_box_x0,
           stats.shadow_box_x1, stats.shadow_box_y0, stats.shadow_box_y1);
    if (room_out != NULL) {
        printf("  room records               %u rows, %u resolved, %u reproduce all four "
               "fields\n",
               room_rows, room_resolved, room_agreed);
        printf("  room camera row            %u of %u rooms hold the camera inside the "
               "record's Y pair\n",
               room_camera_in_room, room_camera_checked);
    }
    printf("  (no pixel comparison here by design; count the dumps with\n"
           "   tools/audit_presenter_divergence.py)\n");
    if (last_status != GB_PRESENTATION_OK) {
        printf("  last capture status        %s\n", status_name(last_status));
    }

    free(prev_state);
    free(post_state);
    free(render_state);
    free(surface);
    free(previous_render);
    free(bg_color_index);
    free(bg_attr_priority);
    return 0;
}