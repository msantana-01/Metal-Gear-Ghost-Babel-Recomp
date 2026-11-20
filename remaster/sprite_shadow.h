/* Carrying a sprite past the edge of the game's own view.
 *
 * Ghost Babel spawns objects against its 160px viewport and releases them the
 * moment they leave: a traced soldier's OAM slot survives 3 to 9 frames
 * off-screen and then goes free, while crossing a 48px margin takes about 48
 * frames. So by the time a sprite would be visible out in the widescreen, the
 * guest has already thrown the slot away. Measured sequence, a soldier walking
 * left off column 8:
 *
 *     frame  y    x
 *         96  63   8      last frame on screen
 *         97  63   7
 *        100  63   2
 *        101  63   1
 *        102  63   0      wraps to 255, 253, 252
 *        105  69 252
 *        106  ---      slot free
 *
 * The guest's map survives the edge - it lives in VRAM and can be read back for
 * any column. The guest's *characters* do not. That asymmetry is why the margin
 * shows ground and nobody.
 *
 * So the presenter takes a copy at the moment of exit and carries it itself.
 * Pixels, not a tile index: the game is free to recycle tile 16 into something
 * else the frame after it frees the slot, and a carried sprite that renders
 * from a borrowed tile becomes a different sprite a few frames later. Once
 * rasterised, the shadow is ours.
 *
 * Nothing here touches the guest.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SPRITE_SHADOW_COUNT 8
#define SPRITE_SHADOW_MAX_W 16
#define SPRITE_SHADOW_MAX_H 16

/* What the presenter hands over when a sprite has just left the view. */
typedef struct ShadowCandidate {
    int32_t screen_x; /* game space; may sit outside 0..159 */
    int32_t screen_y;
    uint32_t width;
    uint32_t height;
    /* Colour indices 0..3, 0 transparent, row major from the top left. */
    uint8_t index[SPRITE_SHADOW_MAX_W * SPRITE_SHADOW_MAX_H];
    /* Resolved RGB555 so the shadow does not re-read a palette it may outlive. */
    uint16_t colour[4];
    /* The guest put this object behind the background. A carried copy that
     * ignored that would walk a soldier through a wall the game drew over
     * him. */
    bool behind_bg;
} ShadowCandidate;

typedef struct SpriteShadow {
    bool alive;
    int32_t screen_x;
    int32_t screen_y;
    int32_t vx;
    int32_t vy;
    uint32_t width;
    uint32_t height;
    uint8_t index[SPRITE_SHADOW_MAX_W * SPRITE_SHADOW_MAX_H];
    uint16_t colour[4];
    bool behind_bg;
    uint32_t age;
} SpriteShadow;

typedef struct SpriteShadowSet {
    SpriteShadow items[SPRITE_SHADOW_COUNT];
} SpriteShadowSet;

void sprite_shadow_reset(SpriteShadowSet *set);

/* Take a copy. Returns true if it was admitted, false when the set is full -
 * which is a limit worth reporting rather than a silent drop. */
bool sprite_shadow_admit(SpriteShadowSet *set, const ShadowCandidate *candidate);

/* One frame of motion. A shadow that has been carried for longer than its
 * budget is dropped: the guest does not keep entities alive off-screen, and
 * neither should we invent one forever. */
void sprite_shadow_advance(SpriteShadowSet *set);

/* Draw into the surface, in the margin columns only.
 *
 * `game_left` is where the game's 160 columns start on the surface, so a shadow
 * at game x=159 belongs at game_left+159. The margin widths bound the fade:
 * the shadow is solid at the edge of the game view and gone by the outer edge,
 * which means a room with no margin shows nothing at all - the same answer the
 * terrain gives. */
/* Where the carried shadows actually landed. A shadow that is admitted but
 * never drawn is a different failure from one that is drawn in the wrong
 * place, and only the box tells them apart. */
typedef struct ShadowDrawBox {
    uint32_t pixels;
    int32_t x0;
    int32_t x1;
    int32_t y0;
    int32_t y1;
} ShadowDrawBox;

/* The background the shadow has to respect.
 *
 * The same rule the object pass uses: a background pixel that is opaque and
 * carries the priority bit wins over every object, and an object that the
 * guest put behind the background never shows through it. Without this a
 * carried soldier walks in front of the wall the game drew over him. */
typedef struct ShadowBackground {
    const uint8_t* color_index;   /* per surface pixel, 0 = transparent */
    const uint8_t* attr_priority; /* per surface pixel, 1 = the BG wins */
    uint32_t stride_pixels;
    bool objects_win_over_bg; /* the background is switched off */
} ShadowBackground;

void sprite_shadow_render(const SpriteShadowSet *set,
                          uint32_t *surface,
                          uint32_t stride_pixels,
                          uint32_t height,
                          int32_t game_left,
                          uint32_t room_left,
                          uint32_t room_right,
                          const ShadowBackground* background,
                          ShadowDrawBox *out_box);