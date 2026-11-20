#include "camera_extent.h"

#include <stddef.h>

/* Reset counters, for the summary. File scope because the render paths are not
 * the ones being counted. */
static CameraExtentCauses g_causes;

void camera_extent_causes(CameraExtentCauses* out) {
    if (out != NULL) {
        *out = g_causes;
    }
}

void camera_extent_reset(CameraExtent* extent, uint8_t scx, uint8_t scy, CameraBounds bounds) {
    extent->scx_last = scx;
    extent->scy_anchor = scy;
    extent->min_x = bounds.min_x;
    extent->max_x = bounds.max_x;
    extent->valid = true;
}

bool camera_extent_observe(CameraExtent* extent, uint8_t scx, uint8_t scy, CameraBounds bounds) {
    if (!extent->valid) {
        camera_extent_reset(extent, scx, scy, bounds);
        ++g_causes.total;
        return true;
    }

    /* Measured the short way round the map. As a plain difference a camera
     * crossing the seam - 255 to 0, one pixel of walking - reads as a
     * 255-pixel teleport and resets the room every time the player goes
     * round, which is exactly what kept an outdoor area's margin from ever
     * opening. */
    const int32_t step = (int32_t)(int8_t)(uint8_t)(scx - (uint8_t)extent->scx_last);
    const uint32_t travelled = (uint32_t)(step < 0 ? -step : step);
    const int32_t step_y = (int32_t)(int8_t)(uint8_t)(scy - (uint8_t)extent->scy_anchor);
    const uint32_t travelled_y = (uint32_t)(step_y < 0 ? -step_y : step_y);
    const bool moved_bounds = bounds.min_x != extent->min_x || bounds.max_x != extent->max_x;
    const bool moved_scy = travelled_y > CAMERA_SCROLL_JUMP_PX;
    const bool jumped = travelled > CAMERA_SCROLL_JUMP_PX;
    if (moved_scy || jumped || moved_bounds) {
        camera_extent_reset(extent, scx, scy, bounds);
        ++g_causes.total;
        if (moved_bounds) {
            ++g_causes.by_bounds;
        } else if (moved_scy) {
            ++g_causes.by_scy;
        } else {
            ++g_causes.by_step;
        }
        return true;
    }

    extent->scx_last = scx;
    extent->scy_anchor = scy;
    return false;
}

void camera_margin_for(const CameraExtent* extent,
                       uint16_t camera_x,
                       uint32_t max_margin,
                       CameraMargin* out_margin) {
    uint32_t left = 0u;
    uint32_t right = 0u;
    if (extent->valid && extent->max_x > extent->min_x) {
        /* The room's world is [min_x, max_x + 159]. Each band sits against the
         * picture, so the count on a side is simply how far the camera still
         * has to travel inside its own range; how wide that band ends up is the
         * presenter's composition (camera_picture_left), not this file's. */
        if (camera_x > extent->min_x) {
            left = (uint32_t)camera_x - (uint32_t)extent->min_x;
        }
        if (extent->max_x > camera_x) {
            right = (uint32_t)extent->max_x - (uint32_t)camera_x;
        }
        if (left > max_margin) {
            left = max_margin;
        }
        if (right > max_margin) {
            right = max_margin;
        }
    }
    out_margin->left = left;
    out_margin->right = right;
    /* The presenter resolves world_x = surface_x + camera_x + scx while the game
     * resolves world_x = game_x + scx, so a surface pixel shows the game's
     * game_x - camera_x. Putting the view at surface `left` therefore needs a
     * shift of -left, taken modulo the byte the tilemap lives in. */
    out_margin->camera_x = (0u - left) & 0xFFu;
}

/* The picture is 160 columns; every GB presentation surface is. */
#define CAMERA_PICTURE_WIDTH 160u

uint32_t camera_picture_left_for(uint32_t play_width,
                                 uint32_t picture_width,
                                 uint32_t margin_left,
                                 uint32_t margin_right) {
    if (play_width <= picture_width) {
        return 0u;
    }
    const uint32_t room = play_width - picture_width;
    const uint32_t half = room / 2u;
    /* A picture at `left` leaves a left band of `left` and a right band of
     * `room - left`, so leaving neither empty needs `room - right <= left <=
     * margin_left`. */
    const uint32_t lo = (margin_right < room) ? room - margin_right : 0u;
    const uint32_t hi = (margin_left < room) ? margin_left : room;
    if (lo > hi) {
        /* Neither side has the travel for its band - a room narrower than the
         * surface. Centred splits what is missing between the two ends instead
         * of emptying one of them. */
        return half;
    }
    if (half < lo) {
        return lo;
    }
    if (half > hi) {
        return hi;
    }
    return half;
}

uint32_t camera_picture_left(uint32_t play_width,
                             uint32_t margin_left,
                             uint32_t margin_right) {
    return camera_picture_left_for(play_width, CAMERA_PICTURE_WIDTH, margin_left, margin_right);
}
