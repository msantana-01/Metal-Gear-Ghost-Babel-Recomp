#include "sprite_shadow.h"

#include <string.h>

/* How long a shadow is carried before it is dropped. At the speed the guest
 * moves a soldier - one to two pixels a frame - the 48px margin takes about 48
 * frames, so this is generous by a third. It exists so that a sprite which left
 * on a turn nobody can see does not accumulate forever. */
#define SPRITE_SHADOW_MAX_AGE 64u

/* The width of the Game Boy's own screen. Spelled here rather than taken from
 * the engine so this module stays a leaf: it is pure motion and compositing, and
 * can be compiled and measured without the emulator. */
#define SPRITE_SHADOW_GAME_WIDTH 160

void sprite_shadow_reset(SpriteShadowSet *set) {
    memset(set, 0, sizeof(*set));
}

bool sprite_shadow_admit(SpriteShadowSet *set, const ShadowCandidate *candidate) {
    if (candidate == NULL || candidate->width == 0u || candidate->height == 0u) {
        return false;
    }
    if (candidate->width > SPRITE_SHADOW_MAX_W || candidate->height > SPRITE_SHADOW_MAX_H) {
        return false;
    }
    for (uint32_t i = 0; i < SPRITE_SHADOW_COUNT; ++i) {
        SpriteShadow *shadow = &set->items[i];
        if (shadow->alive) {
            continue;
        }
        shadow->alive = true;
        shadow->screen_x = candidate->screen_x;
        shadow->screen_y = candidate->screen_y;
        /* Direction from which it left. Zero velocity until the caller gives it
         * one, which keeps the admission path free of guessing. */
        shadow->vx = 0;
        shadow->vy = 0;
        shadow->width = candidate->width;
        shadow->height = candidate->height;
        memcpy(shadow->index, candidate->index, sizeof(shadow->index));
        memcpy(shadow->colour, candidate->colour, sizeof(shadow->colour));
        shadow->behind_bg = candidate->behind_bg;
        shadow->age = 0u;
        return true;
    }
    return false;
}

void sprite_shadow_advance(SpriteShadowSet *set) {
    for (uint32_t i = 0; i < SPRITE_SHADOW_COUNT; ++i) {
        SpriteShadow *shadow = &set->items[i];
        if (!shadow->alive) {
            continue;
        }
        shadow->screen_x += shadow->vx;
        shadow->screen_y += shadow->vy;
        shadow->age++;
        if (shadow->age > SPRITE_SHADOW_MAX_AGE) {
            shadow->alive = false;
        }
    }
}

/* Blend one RGB555 sample toward black at `strength` (0 = as authored). */
static uint32_t shadow_shade(uint16_t colour, uint32_t strength) {
    if (strength >= 255u) {
        return (uint32_t)colour;
    }
    const uint32_t keep = 255u - strength;
    const uint32_t r = ((uint32_t)(colour & 31u) * keep) / 255u;
    const uint32_t g = (((uint32_t)(colour >> 5) & 31u) * keep) / 255u;
    const uint32_t b = (((uint32_t)(colour >> 10) & 31u) * keep) / 255u;
    return r | (g << 5) | (b << 10);
}

void sprite_shadow_render(const SpriteShadowSet *set,
                          uint32_t *surface,
                          uint32_t stride_pixels,
                          uint32_t height,
                          int32_t game_left,
                          uint32_t room_left,
                          uint32_t room_right,
                          const ShadowBackground* background,
                          ShadowDrawBox *out_box) {
    if (out_box != NULL) {
        out_box->pixels = 0u;
        out_box->x0 = 0;
        out_box->x1 = -1;
        out_box->y0 = 0;
        out_box->y1 = -1;
    }
    if (surface == NULL || set == NULL) {
        return;
    }
    for (uint32_t i = 0; i < SPRITE_SHADOW_COUNT; ++i) {
        const SpriteShadow *shadow = &set->items[i];
        if (!shadow->alive) {
            continue;
        }
        /* How far outside the game's own 160 columns the shadow sits, and
         * which side. The margins bound the fade: solid against the game's
         * edge, gone by the outer edge of the room. */
        int32_t outside = 0;
        uint32_t room = 0u;
        if (shadow->screen_x >= SPRITE_SHADOW_GAME_WIDTH) {
            outside = shadow->screen_x - (int32_t)SPRITE_SHADOW_GAME_WIDTH;
            room = room_right;
        } else if (shadow->screen_x < 0) {
            outside = -shadow->screen_x;
            room = room_left;
        } else {
            continue; /* still inside the game view; the PPU owns that column */
        }
        if (room == 0u) {
            continue; /* the room does not continue that way */
        }
        /* Solid until the shadow has cleared the game's edge, then fading to
         * nothing by the outer edge of the room.
         *
         * Measuring the fade from the sprite's own outer edge is what keeps
         * the handover seamless. The PPU drew the sprite bright up to column
         * 159 and we take over at 160; fading by distance from the edge would
         * hand it over four percent dimmer, and a soldier walking across the
         * boundary would show it as a seam running down his side. Subtracting
         * the sprite's width first means the first pixel we draw is exactly as
         * bright as the last one it drew. */
        const uint32_t beyond = (uint32_t)outside > shadow->width + 1u
            ? (uint32_t)outside - shadow->width - 1u
            : 0u;
        const uint32_t span = room > shadow->width + 1u
            ? room - shadow->width - 1u
            : 1u;
        const uint32_t strength = (beyond * 255u) / span;
        if (strength > 255u) {
            continue;
        }

        for (uint32_t py = 0; py < shadow->height; ++py) {
            const int32_t surface_y = shadow->screen_y + (int32_t)py;
            if (surface_y < 0 || surface_y >= (int32_t)height) {
                continue;
            }
            uint32_t *row = surface + (size_t)surface_y * stride_pixels;
            for (uint32_t px = 0; px < shadow->width; ++px) {
                const uint8_t ci = shadow->index[py * shadow->width + px];
                if (ci == 0u || ci > 3u) {
                    continue;
                }
                const int32_t game_x = shadow->screen_x + (int32_t)px;
                /* Never draw over the game's own columns. A sprite caught
                 * half off the edge has its visible half already in the PPU's
                 * framebuffer, copied exactly, and drawing our copy on top of
                 * it could only lose - the guest may already have moved that
                 * tile on. The shadow owns the columns the PPU never drew. */
                if (game_x >= 0 && game_x < SPRITE_SHADOW_GAME_WIDTH) {
                    continue;
                }
                const int32_t surface_x = game_left + game_x;
                if (surface_x < 0 || surface_x >= (int32_t)stride_pixels) {
                    continue;
                }
                /* The background the game drew over this object wins, the
                 * same way it wins for the objects the guest still holds. */
                if (background != NULL && background->color_index != NULL &&
                    background->attr_priority != NULL) {
                    const size_t bg_at = (size_t)surface_y * background->stride_pixels +
                                         (size_t)surface_x;
                    if (background->color_index[bg_at] != 0u) {
                        const uint8_t bg_priority = background->attr_priority[bg_at];
                        if (!background->objects_win_over_bg &&
                            (bg_priority != 0u || shadow->behind_bg)) {
                            continue;
                        }
                    }
                }
                row[surface_x] = shadow_shade(shadow->colour[ci], strength);
                if (out_box != NULL) {
                    out_box->pixels++;
                    if (out_box->x1 < out_box->x0) {
                        out_box->x0 = surface_x;
                        out_box->x1 = surface_x;
                        out_box->y0 = surface_y;
                        out_box->y1 = surface_y;
                    } else {
                        if (surface_x < out_box->x0) out_box->x0 = surface_x;
                        if (surface_x > out_box->x1) out_box->x1 = surface_x;
                        if (surface_y < out_box->y0) out_box->y0 = surface_y;
                        if (surface_y > out_box->y1) out_box->y1 = surface_y;
                    }
                }
            }
        }
    }
}