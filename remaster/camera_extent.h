/* How far the widescreen may show beyond the game's own view.
 *
 * Ghost Babel clamps its camera to the room, and it says where the room ends.
 * The room loader (bank 41, 29:410a) reads an 8-byte record out of a table
 * indexed by area and room - a pointer table at 29:4492 selected by $CBC0, an
 * 8-byte stride within it selected by $CBC1 - and stores the record's first two
 * 16-bit fields in WRAM: $CC0B/$CC0C is the camera's right limit, $CC0D/$CC0E
 * its left. The base room's record is 256 and 288, which is why walking left
 * for 800 frames leaves SCX at 0 and walking right leaves it at 14: the player
 * stops at a wall before the camera reaches its own right limit, and the limit
 * is what the room is.
 *
 * So the room's visible world is [min_x, max_x + 159] - every column the camera
 * can put on screen while it is inside its own range. Beyond that the game has
 * nothing to show: the map there is either a different room or the same room's
 * far side. An earlier build that filled the margin to the surface edge showed
 * a grey stairwell in the base room's right margin, which is that error.
 *
 * Reading the limits beats learning them. An extent learned by watching the
 * camera walk opens only after the player has already walked far enough to earn
 * it, and it opens asymmetrically - the side behind the camera first, the side
 * ahead never - which is what the fade was covering for. The limits are exact
 * on the first frame of the room and correct in every area.
 *
 * This is observation only. Nothing here writes to the guest.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A camera moving further than this in one frame is not a walk. Walking moves
 * SCX by one pixel per frame, and the fastest scroll anywhere in the first hour
 * - a cutscene pan - moves it by five. Eight leaves room for that and still
 * catches the teleport a door makes. */
#define CAMERA_SCROLL_JUMP_PX 8u

/* The room's own camera limits, in world pixels, as the guest stores them.
 * Both are zero until a room has been loaded, which is the whole of the boot
 * and the opening. */
typedef struct CameraBounds {
    uint16_t min_x;
    uint16_t max_x;
} CameraBounds;

/* The room as this frame found it: the bounds above, plus enough of the
 * camera's own motion to notice a room change the bounds do not describe. */
typedef struct CameraExtent {
    uint32_t scx_last;
    uint32_t scy_anchor;
    uint16_t min_x;
    uint16_t max_x;
    bool valid;
} CameraExtent;

typedef struct CameraMargin {
    uint32_t left;
    uint32_t right;
    uint32_t camera_x;
} CameraMargin;

/* Why the extent has been reset, over a whole run.
 *
 * A cold margin means very different things depending on the answer. If the
 * rooms are changing, nothing can be done about it and the columns have to be
 * earned again in each one. If the extent is resetting on its own - a bounds
 * field the guest rewrites, a vertical origin that wobbles - then the cache is
 * being thrown away while it is still true, and that is a bug with a fix.
 * Counted here rather than guessed at from scx in a dump, because the resets
 * happen on frames a dump does not carry. */
typedef struct CameraExtentCauses {
    uint32_t total;
    uint32_t by_bounds;
    uint32_t by_scy;
    uint32_t by_step;
} CameraExtentCauses;

/* The counts so far. Bounds wins when more than one cause fires together, since
 * it is the one that is exact. */
void camera_extent_causes(CameraExtentCauses* out);

/* Forget the last frame and start again from this one. */
void camera_extent_reset(CameraExtent* extent, uint8_t scx, uint8_t scy, CameraBounds bounds);

/* Fold one frame in, reporting a room change.
 *
 * Returns true when this frame started a new room. Anything keyed by world
 * coordinate has to be dropped at that moment: the tilemap is 256 pixels wide
 * and areas repeat within it, so a column remembered in the room the player
 * just left is not the column they are looking at now.
 *
 * Three signals, and each is needed. The bounds changing is the room loader
 * having run, and it is exact - but two rooms in a row can share a rectangle.
 * A vertical origin moving is what a room change looks like when the game
 * clamps its camera. And a jump too large to be walking is what a door looks
 * like. The step is measured the short way round the map, so a camera crossing
 * the seam - 255 to 0, one pixel of walking - is not read as a teleport. */
bool camera_extent_observe(CameraExtent* extent, uint8_t scx, uint8_t scy, CameraBounds bounds);

/* How far the surface may show on each side, from the room's own limits.
 *
 * `max_margin` caps each side; the halves need not match, so the view stays put
 * while one side opens before the other. */
void camera_margin_for(const CameraExtent* extent,
                       uint16_t camera_x,
                       uint32_t max_margin,
                       CameraMargin* out_margin);

/* Where the picture sits on the surface, from what the room has on each side.
 *
 * Centred while both bands can be filled, which is every frame in the middle of
 * a room - the composition is unchanged for all but the last `half` pixels of
 * camera travel before a wall. There one side runs out: staying centred would
 * letterbox that band while the room still had columns on the other side, so
 * the picture slides away from the empty side by exactly what that side cannot
 * fill, and no further (ADR-011).
 *
 * `play_width` is what the picture may use, the HUD strip already subtracted.
 * The result is in [0, play_width - 160]; the 160px artifact and any surface
 * too narrow to have a choice return 0. */
uint32_t camera_picture_left(uint32_t play_width,
                             uint32_t margin_left,
                             uint32_t margin_right);

/* Same layout rule for an arbitrary active picture width (e.g. 136 columns
 * during gameplay once the 24-column status panel is removed from the PPU view). */
uint32_t camera_picture_left_for(uint32_t play_width,
                                 uint32_t picture_width,
                                 uint32_t margin_left,
                                 uint32_t margin_right);
