#include "codec_lipsync.h"

#include <string.h>

#define LCDC_BG_ENABLE 0x01u
#define LCDC_BG_MAP 0x08u
#define LCDC_TILE_DATA 0x10u
#define LCDC_WINDOW_ENABLE 0x20u
#define LCDC_LCD_ENABLE 0x80u

#define TILE_BYTES 16u
#define SIGNED_TILE_BASE_OFFSET 0x1000u
#define PORTRAIT_TILE_COUNT 24u
#define PORTRAIT_BYTES (PORTRAIT_TILE_COUNT * TILE_BYTES)
#define RIGHT_PORTRAIT_BASE_TILE 80u
#define LEFT_PORTRAIT_BASE_TILE 104u

/* WRAM0 $C449 speaker slot ID (0xA9 = Snake on right, other non-zero = contact on left). */
#define WRAM_SPEAKER_ADDR 0x0449u
#define SPEAKER_ID_SNAKE 0xA9u

#define DIALOGUE_X0 16u
#define DIALOGUE_X1 144u /* Exclude blinking continue cursor at x >= 144 */
#define DIALOGUE_Y0 80u
#define DIALOGUE_Y1 136u

#define DIALOGUE_HOLD_FRAMES 10u
#define MOUTH_STEP_FRAMES 5u
#define BLINK_GUARD_FRAMES 45u
#define BLINK_DEFER_FRAMES 55u
#define BLINK_ENVELOPE_FRAMES 6u

typedef struct CodecPatchPixel {
    uint8_t dx;
    uint8_t dy;
    uint8_t shade;
} CodecPatchPixel;

typedef struct CodecPortraitProfile {
    const char* name;
    uint32_t vram_hash;
    const CodecPatchPixel* mouth_s1;
    size_t mouth_s1_count;
    const CodecPatchPixel* mouth_s2;
    size_t mouth_s2_count;
    const CodecPatchPixel* blink_s1;
    size_t blink_s1_count;
    const CodecPatchPixel* blink_s2;
    size_t blink_s2_count;
} CodecPortraitProfile;

static const CodecPatchPixel kSnake_Mouth1[] = {
    {14u, 35u, 0u}, {14u, 36u, 0u}, {15u, 36u, 0u}, {12u, 37u, 2u}, {13u, 37u, 0u}, {14u, 37u, 0u},
    {15u, 37u, 0u}, {16u, 37u, 0u}, {17u, 37u, 2u}, {13u, 38u, 2u}, {14u, 38u, 3u}, {15u, 38u, 3u},
    {16u, 38u, 3u}, {17u, 38u, 2u},
};
static const CodecPatchPixel kSnake_Mouth2[] = {
    {14u, 35u, 0u}, {14u, 36u, 0u}, {15u, 36u, 0u}, {12u, 37u, 1u}, {13u, 37u, 0u}, {14u, 37u, 0u},
    {15u, 37u, 0u}, {16u, 37u, 0u}, {17u, 37u, 1u}, {13u, 38u, 0u}, {14u, 38u, 0u}, {15u, 38u, 0u},
    {16u, 38u, 0u}, {17u, 38u, 1u}, {13u, 39u, 2u}, {14u, 39u, 3u}, {15u, 39u, 3u}, {16u, 39u, 3u},
    {17u, 39u, 2u},
};
static const CodecPatchPixel kCampbell_Mouth1[] = {
    {16u, 33u, 1u}, {17u, 33u, 0u}, {18u, 33u, 0u}, {19u, 33u, 0u}, {20u, 33u, 0u}, {21u, 33u, 0u},
    {22u, 33u, 2u}, {16u, 34u, 2u}, {17u, 34u, 3u}, {18u, 34u, 3u}, {19u, 34u, 3u}, {20u, 34u, 3u},
    {21u, 34u, 3u}, {22u, 34u, 2u},
};
static const CodecPatchPixel kCampbell_Mouth2[] = {
    {16u, 33u, 1u}, {17u, 33u, 0u}, {18u, 33u, 0u}, {19u, 33u, 0u}, {20u, 33u, 0u}, {21u, 33u, 0u},
    {22u, 33u, 1u}, {17u, 34u, 1u}, {18u, 34u, 0u}, {19u, 34u, 0u}, {20u, 34u, 0u}, {21u, 34u, 1u},
    {17u, 35u, 2u}, {18u, 35u, 3u}, {19u, 35u, 3u}, {20u, 35u, 3u}, {21u, 35u, 3u}, {22u, 35u, 2u},
    {18u, 36u, 1u}, {19u, 36u, 1u}, {20u, 36u, 1u},
};
static const CodecPatchPixel kCampbell_Blink1[] = {
    {9u, 18u, 2u}, {10u, 18u, 2u}, {11u, 18u, 2u}, {12u, 18u, 2u}, {13u, 18u, 2u}, {14u, 18u, 2u},
    {23u, 19u, 1u}, {24u, 19u, 1u}, {25u, 19u, 1u}, {26u, 19u, 1u}, {27u, 19u, 1u}, {25u, 21u, 1u},
    {26u, 21u, 1u},
};
static const CodecPatchPixel kCampbell_Blink2[] = {
    {8u, 18u, 2u}, {9u, 18u, 2u}, {10u, 18u, 2u}, {11u, 18u, 2u}, {12u, 18u, 2u}, {13u, 18u, 2u},
    {14u, 18u, 2u}, {15u, 18u, 2u}, {8u, 19u, 1u}, {9u, 19u, 0u}, {10u, 19u, 0u}, {11u, 19u, 0u},
    {12u, 19u, 0u}, {13u, 19u, 0u}, {14u, 19u, 0u}, {15u, 19u, 1u}, {10u, 20u, 2u}, {11u, 20u, 2u},
    {12u, 20u, 2u}, {13u, 20u, 2u}, {14u, 20u, 2u}, {15u, 20u, 2u}, {23u, 19u, 2u}, {24u, 19u, 2u},
    {25u, 19u, 2u}, {26u, 19u, 2u}, {27u, 19u, 2u}, {23u, 20u, 0u}, {24u, 20u, 0u}, {25u, 20u, 0u},
    {26u, 20u, 0u}, {27u, 20u, 0u}, {25u, 21u, 1u}, {26u, 21u, 1u},
};
static const CodecPatchPixel kMeiLing_Mouth1[] = {
    {10u, 37u, 1u}, {11u, 37u, 0u}, {12u, 37u, 0u}, {13u, 37u, 1u}, {10u, 38u, 2u}, {11u, 38u, 3u},
    {12u, 38u, 2u},
};
static const CodecPatchPixel kMeiLing_Mouth2[] = {
    {10u, 37u, 0u}, {11u, 37u, 0u}, {12u, 37u, 0u}, {13u, 37u, 1u}, {10u, 38u, 2u}, {11u, 38u, 0u},
    {12u, 38u, 1u}, {13u, 38u, 2u}, {10u, 39u, 2u}, {11u, 39u, 3u}, {12u, 39u, 2u},
};
static const CodecPatchPixel kMeiLing_Blink1[] = {
    {6u, 23u, 2u}, {7u, 23u, 1u}, {8u, 23u, 1u}, {16u, 23u, 2u}, {17u, 23u, 2u}, {18u, 23u, 2u},
    {19u, 23u, 2u}, {16u, 24u, 0u}, {17u, 24u, 0u}, {18u, 24u, 1u}, {19u, 24u, 0u},
};
static const CodecPatchPixel kMeiLing_Blink2[] = {
    {6u, 23u, 2u}, {7u, 23u, 2u}, {8u, 23u, 2u}, {6u, 24u, 0u}, {7u, 24u, 0u}, {8u, 24u, 0u},
    {15u, 23u, 2u}, {16u, 23u, 2u}, {17u, 23u, 2u}, {18u, 23u, 2u}, {19u, 23u, 2u}, {20u, 23u, 2u},
    {15u, 24u, 0u}, {16u, 24u, 0u}, {17u, 24u, 0u}, {18u, 24u, 0u}, {19u, 24u, 0u}, {20u, 24u, 1u},
    {18u, 25u, 2u}, {19u, 25u, 2u}, {20u, 25u, 2u}, {21u, 25u, 2u},
};
static const CodecPatchPixel kWeasel_Mouth1[] = {
    {17u, 35u, 1u}, {18u, 35u, 0u}, {19u, 35u, 0u}, {20u, 35u, 0u}, {21u, 35u, 0u}, {22u, 35u, 0u},
    {23u, 35u, 0u}, {17u, 36u, 2u}, {18u, 36u, 3u}, {19u, 36u, 3u}, {20u, 36u, 3u}, {21u, 36u, 3u},
    {22u, 36u, 2u}, {23u, 36u, 2u},
};
static const CodecPatchPixel kWeasel_Mouth2[] = {
    {17u, 35u, 0u}, {18u, 35u, 0u}, {19u, 35u, 0u}, {20u, 35u, 0u}, {21u, 35u, 0u}, {22u, 35u, 0u},
    {23u, 35u, 0u}, {17u, 36u, 1u}, {18u, 36u, 0u}, {19u, 36u, 0u}, {20u, 36u, 0u}, {21u, 36u, 0u},
    {22u, 36u, 0u}, {23u, 36u, 1u}, {17u, 37u, 2u}, {18u, 37u, 3u}, {19u, 37u, 3u}, {20u, 37u, 3u},
    {21u, 37u, 3u}, {22u, 37u, 2u}, {18u, 38u, 1u}, {19u, 38u, 1u}, {20u, 38u, 1u}, {21u, 38u, 1u},
};
static const CodecPatchPixel kWeasel_Blink1[] = {
    {11u, 21u, 2u}, {12u, 21u, 2u}, {13u, 21u, 2u}, {14u, 21u, 2u}, {15u, 21u, 2u}, {16u, 21u, 2u},
    {25u, 21u, 1u}, {26u, 21u, 1u}, {27u, 21u, 1u}, {28u, 21u, 1u},
};
static const CodecPatchPixel kWeasel_Blink2[] = {
    {12u, 20u, 2u}, {13u, 20u, 2u}, {14u, 20u, 2u}, {15u, 20u, 2u}, {16u, 20u, 2u}, {11u, 21u, 2u},
    {12u, 21u, 2u}, {13u, 21u, 2u}, {14u, 21u, 2u}, {15u, 21u, 2u}, {16u, 21u, 2u}, {11u, 22u, 1u},
    {12u, 22u, 0u}, {13u, 22u, 0u}, {14u, 22u, 0u}, {15u, 22u, 0u}, {16u, 22u, 0u}, {17u, 22u, 1u},
    {25u, 20u, 1u}, {26u, 20u, 1u}, {27u, 20u, 1u}, {28u, 20u, 1u}, {25u, 21u, 2u}, {26u, 21u, 2u},
    {27u, 21u, 2u}, {28u, 21u, 2u}, {25u, 22u, 0u}, {26u, 22u, 0u}, {27u, 22u, 0u}, {28u, 22u, 0u},
};
static const CodecPatchPixel kBrian_Mouth1[] = {
    {11u, 35u, 2u}, {12u, 35u, 0u}, {13u, 35u, 0u}, {14u, 35u, 0u}, {15u, 35u, 0u}, {16u, 35u, 0u},
    {17u, 35u, 2u}, {11u, 36u, 2u}, {12u, 36u, 3u}, {13u, 36u, 3u}, {14u, 36u, 3u}, {15u, 36u, 3u},
    {16u, 36u, 3u}, {17u, 36u, 2u}, {12u, 37u, 1u}, {13u, 37u, 1u}, {14u, 37u, 1u}, {15u, 37u, 1u},
    {16u, 37u, 1u},
};
static const CodecPatchPixel kBrian_Mouth2[] = {
    {11u, 35u, 1u}, {12u, 35u, 0u}, {13u, 35u, 0u}, {14u, 35u, 0u}, {15u, 35u, 0u}, {16u, 35u, 0u},
    {17u, 35u, 1u}, {11u, 36u, 2u}, {12u, 36u, 0u}, {13u, 36u, 0u}, {14u, 36u, 0u}, {15u, 36u, 0u},
    {16u, 36u, 0u}, {17u, 36u, 2u}, {11u, 37u, 2u}, {12u, 37u, 3u}, {13u, 37u, 3u}, {14u, 37u, 3u},
    {15u, 37u, 3u}, {16u, 37u, 3u}, {17u, 37u, 2u}, {12u, 38u, 1u}, {13u, 38u, 1u}, {14u, 38u, 1u},
    {15u, 38u, 1u}, {16u, 38u, 1u},
};
static const CodecPatchPixel kBrian_Blink1[] = {
    {6u, 20u, 2u}, {7u, 20u, 2u}, {8u, 20u, 2u}, {9u, 20u, 2u}, {10u, 20u, 2u}, {18u, 20u, 2u},
    {19u, 20u, 2u}, {20u, 20u, 2u}, {21u, 20u, 2u}, {22u, 20u, 2u}, {23u, 20u, 2u},
};
static const CodecPatchPixel kBrian_Blink2[] = {
    {6u, 20u, 2u}, {7u, 20u, 2u}, {8u, 20u, 2u}, {9u, 20u, 2u}, {10u, 20u, 2u}, {5u, 21u, 2u},
    {6u, 21u, 2u}, {7u, 21u, 2u}, {8u, 21u, 2u}, {9u, 21u, 2u}, {10u, 21u, 2u}, {11u, 21u, 2u},
    {5u, 22u, 1u}, {6u, 22u, 0u}, {7u, 22u, 0u}, {8u, 22u, 0u}, {9u, 22u, 0u}, {10u, 22u, 0u},
    {11u, 22u, 1u}, {18u, 20u, 2u}, {19u, 20u, 2u}, {20u, 20u, 2u}, {21u, 20u, 2u}, {22u, 20u, 2u},
    {23u, 20u, 2u}, {17u, 21u, 2u}, {18u, 21u, 2u}, {19u, 21u, 2u}, {20u, 21u, 2u}, {21u, 21u, 2u},
    {22u, 21u, 2u}, {23u, 21u, 2u}, {17u, 22u, 1u}, {18u, 22u, 0u}, {19u, 22u, 0u}, {20u, 22u, 0u},
    {21u, 22u, 0u}, {22u, 22u, 0u}, {23u, 22u, 1u},
};
static const CodecPatchPixel kChris_Mouth1[] = {
    {15u, 32u, 1u}, {15u, 33u, 0u}, {16u, 33u, 0u}, {17u, 33u, 0u}, {18u, 33u, 0u}, {19u, 33u, 0u},
    {20u, 33u, 0u}, {21u, 33u, 1u}, {13u, 34u, 2u}, {14u, 34u, 2u}, {15u, 34u, 3u}, {16u, 34u, 3u},
    {17u, 34u, 3u}, {18u, 34u, 3u}, {19u, 34u, 3u}, {20u, 34u, 2u}, {21u, 34u, 2u}, {15u, 35u, 2u},
    {16u, 35u, 1u}, {17u, 35u, 1u}, {18u, 35u, 1u}, {19u, 35u, 2u},
};
static const CodecPatchPixel kChris_Mouth2[] = {
    {15u, 32u, 1u}, {14u, 33u, 0u}, {15u, 33u, 0u}, {16u, 33u, 0u}, {17u, 33u, 0u}, {18u, 33u, 0u},
    {19u, 33u, 0u}, {20u, 33u, 0u}, {21u, 33u, 1u}, {14u, 34u, 1u}, {15u, 34u, 0u}, {16u, 34u, 0u},
    {17u, 34u, 0u}, {18u, 34u, 0u}, {19u, 34u, 0u}, {20u, 34u, 1u}, {21u, 34u, 2u}, {14u, 35u, 2u},
    {15u, 35u, 2u}, {16u, 35u, 3u}, {17u, 35u, 3u}, {18u, 35u, 3u}, {19u, 35u, 3u}, {20u, 35u, 2u},
    {16u, 36u, 1u}, {17u, 36u, 1u}, {18u, 36u, 1u}, {19u, 36u, 1u},
};
static const CodecPatchPixel kChris_Blink1[] = {
    {5u, 20u, 0u}, {6u, 20u, 1u}, {7u, 20u, 1u}, {8u, 20u, 1u}, {9u, 20u, 1u}, {10u, 20u, 0u},
    {6u, 21u, 0u}, {7u, 21u, 0u}, {8u, 21u, 0u}, {9u, 21u, 0u}, {18u, 19u, 0u}, {19u, 19u, 1u},
    {20u, 19u, 1u}, {21u, 19u, 1u}, {22u, 19u, 1u}, {23u, 19u, 0u}, {19u, 20u, 0u}, {20u, 20u, 0u},
    {21u, 20u, 0u}, {22u, 20u, 0u},
};
static const CodecPatchPixel kChris_Blink2[] = {
    {5u, 20u, 0u}, {6u, 20u, 1u}, {7u, 20u, 1u}, {8u, 20u, 1u}, {9u, 20u, 1u}, {10u, 20u, 0u},
    {5u, 21u, 0u}, {6u, 21u, 1u}, {7u, 21u, 1u}, {8u, 21u, 1u}, {9u, 21u, 1u}, {10u, 21u, 0u},
    {5u, 22u, 2u}, {6u, 22u, 0u}, {7u, 22u, 0u}, {8u, 22u, 0u}, {9u, 22u, 0u}, {10u, 22u, 2u},
    {7u, 23u, 2u}, {8u, 23u, 2u}, {9u, 23u, 2u}, {18u, 19u, 0u}, {19u, 19u, 1u}, {20u, 19u, 1u},
    {21u, 19u, 1u}, {22u, 19u, 1u}, {23u, 19u, 0u}, {18u, 20u, 0u}, {19u, 20u, 1u}, {20u, 20u, 1u},
    {21u, 20u, 1u}, {22u, 20u, 1u}, {23u, 20u, 0u}, {18u, 21u, 3u}, {19u, 21u, 0u}, {20u, 21u, 0u},
    {21u, 21u, 0u}, {22u, 21u, 0u}, {23u, 21u, 3u},
};
static const CodecPatchPixel kJimmy_Mouth1[] = {
    {9u, 34u, 1u}, {10u, 34u, 0u}, {11u, 34u, 0u}, {10u, 35u, 0u}, {11u, 35u, 0u}, {12u, 35u, 0u},
    {13u, 35u, 0u}, {14u, 35u, 1u}, {9u, 36u, 3u}, {10u, 36u, 3u}, {11u, 36u, 3u}, {12u, 36u, 3u},
    {13u, 36u, 3u}, {14u, 36u, 3u}, {10u, 37u, 1u}, {11u, 37u, 0u}, {12u, 37u, 0u}, {13u, 37u, 1u},
};
static const CodecPatchPixel kJimmy_Mouth2[] = {
    {9u, 34u, 0u}, {10u, 34u, 0u}, {11u, 34u, 0u}, {12u, 34u, 0u}, {13u, 34u, 0u}, {14u, 34u, 0u},
    {9u, 35u, 1u}, {10u, 35u, 0u}, {11u, 35u, 0u}, {12u, 35u, 0u}, {13u, 35u, 0u}, {14u, 35u, 0u},
    {9u, 36u, 3u}, {10u, 36u, 2u}, {11u, 36u, 0u}, {12u, 36u, 0u}, {13u, 36u, 0u}, {14u, 36u, 1u},
    {9u, 37u, 3u}, {10u, 37u, 3u}, {11u, 37u, 3u}, {12u, 37u, 3u}, {13u, 37u, 3u}, {14u, 37u, 3u},
    {10u, 38u, 1u}, {11u, 38u, 0u}, {12u, 38u, 0u}, {13u, 38u, 1u},
};
static const CodecPatchPixel kJimmy_Blink1[] = {
    {7u, 20u, 2u}, {8u, 20u, 2u}, {9u, 20u, 2u}, {10u, 20u, 2u}, {7u, 21u, 1u}, {8u, 21u, 0u},
    {9u, 21u, 0u}, {10u, 21u, 0u}, {11u, 21u, 1u}, {19u, 21u, 2u}, {20u, 21u, 2u}, {21u, 21u, 2u},
    {22u, 21u, 2u}, {19u, 22u, 1u}, {20u, 22u, 0u}, {21u, 22u, 0u}, {22u, 22u, 0u}, {23u, 22u, 1u},
};
static const CodecPatchPixel kJimmy_Blink2[] = {
    {7u, 20u, 2u}, {8u, 20u, 2u}, {9u, 20u, 2u}, {10u, 20u, 2u}, {7u, 21u, 2u}, {8u, 21u, 2u},
    {9u, 21u, 2u}, {10u, 21u, 2u}, {11u, 21u, 2u}, {7u, 22u, 1u}, {8u, 22u, 0u}, {9u, 22u, 0u},
    {10u, 22u, 0u}, {11u, 22u, 1u}, {19u, 21u, 2u}, {20u, 21u, 2u}, {21u, 21u, 2u}, {22u, 21u, 2u},
    {19u, 22u, 2u}, {20u, 22u, 2u}, {21u, 22u, 2u}, {22u, 22u, 2u}, {23u, 22u, 2u}, {18u, 23u, 2u},
    {19u, 23u, 0u}, {20u, 23u, 0u}, {21u, 23u, 0u}, {22u, 23u, 0u}, {23u, 23u, 1u}, {20u, 24u, 3u},
};

static const CodecPortraitProfile kPortraitProfiles[] = {
    {"SNAKE", 0xBBBF124Au, kSnake_Mouth1, sizeof(kSnake_Mouth1) / sizeof(kSnake_Mouth1[0]), kSnake_Mouth2, sizeof(kSnake_Mouth2) / sizeof(kSnake_Mouth2[0]), NULL, 0u, NULL, 0u},
    {"CAMPBELL", 0x259AD81Fu, kCampbell_Mouth1, sizeof(kCampbell_Mouth1) / sizeof(kCampbell_Mouth1[0]), kCampbell_Mouth2, sizeof(kCampbell_Mouth2) / sizeof(kCampbell_Mouth2[0]), kCampbell_Blink1, sizeof(kCampbell_Blink1) / sizeof(kCampbell_Blink1[0]), kCampbell_Blink2, sizeof(kCampbell_Blink2) / sizeof(kCampbell_Blink2[0])},
    {"MEI_LING", 0xE38FE3CEu, kMeiLing_Mouth1, sizeof(kMeiLing_Mouth1) / sizeof(kMeiLing_Mouth1[0]), kMeiLing_Mouth2, sizeof(kMeiLing_Mouth2) / sizeof(kMeiLing_Mouth2[0]), kMeiLing_Blink1, sizeof(kMeiLing_Blink1) / sizeof(kMeiLing_Blink1[0]), kMeiLing_Blink2, sizeof(kMeiLing_Blink2) / sizeof(kMeiLing_Blink2[0])},
    {"WEASEL", 0x23FF2FC3u, kWeasel_Mouth1, sizeof(kWeasel_Mouth1) / sizeof(kWeasel_Mouth1[0]), kWeasel_Mouth2, sizeof(kWeasel_Mouth2) / sizeof(kWeasel_Mouth2[0]), kWeasel_Blink1, sizeof(kWeasel_Blink1) / sizeof(kWeasel_Blink1[0]), kWeasel_Blink2, sizeof(kWeasel_Blink2) / sizeof(kWeasel_Blink2[0])},
    {"BRIAN", 0xDB07023Bu, kBrian_Mouth1, sizeof(kBrian_Mouth1) / sizeof(kBrian_Mouth1[0]), kBrian_Mouth2, sizeof(kBrian_Mouth2) / sizeof(kBrian_Mouth2[0]), kBrian_Blink1, sizeof(kBrian_Blink1) / sizeof(kBrian_Blink1[0]), kBrian_Blink2, sizeof(kBrian_Blink2) / sizeof(kBrian_Blink2[0])},
    {"CHRIS", 0x28F2DBA1u, kChris_Mouth1, sizeof(kChris_Mouth1) / sizeof(kChris_Mouth1[0]), kChris_Mouth2, sizeof(kChris_Mouth2) / sizeof(kChris_Mouth2[0]), kChris_Blink1, sizeof(kChris_Blink1) / sizeof(kChris_Blink1[0]), kChris_Blink2, sizeof(kChris_Blink2) / sizeof(kChris_Blink2[0])},
    {"JIMMY", 0x181F10CFu, kJimmy_Mouth1, sizeof(kJimmy_Mouth1) / sizeof(kJimmy_Mouth1[0]), kJimmy_Mouth2, sizeof(kJimmy_Mouth2) / sizeof(kJimmy_Mouth2[0]), kJimmy_Blink1, sizeof(kJimmy_Blink1) / sizeof(kJimmy_Blink1[0]), kJimmy_Blink2, sizeof(kJimmy_Blink2) / sizeof(kJimmy_Blink2[0])},
};

static uint32_t fnv1a_bytes(const uint8_t* data, size_t len) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0u; i < len; ++i) {
        h ^= (uint32_t)data[i];
        h *= 0x01000193u;
    }
    return h;
}

static uint32_t hash_dialogue_box(const GBPresentationHardwareSnapshot* hw) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t y = DIALOGUE_Y0; y < DIALOGUE_Y1; ++y) {
        const size_t row_off = (size_t)y * GB_PRESENTATION_ACCURATE_WIDTH;
        for (uint32_t x = DIALOGUE_X0; x < DIALOGUE_X1; ++x) {
            const uint16_t px = hw->accurate_pixels[row_off + x];
            h ^= (uint32_t)(px & 0xFFu);
            h *= 0x01000193u;
            h ^= (uint32_t)((px >> 8) & 0xFFu);
            h *= 0x01000193u;
        }
    }
    return h;
}

static bool has_tile_run(const uint8_t* map_row, uint8_t first_tile, uint32_t count) {
    for (uint32_t c = 0u; c + count <= 32u; ++c) {
        bool match = true;
        for (uint32_t k = 0u; k < count; ++k) {
            if (map_row[c + k] != (uint8_t)(first_tile + k)) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static bool detect_codec_screen(const GBPresentationHardwareSnapshot* hw) {
    if (hw == NULL || !hw->lcd_enabled) {
        return false;
    }
    if ((hw->lcdc & LCDC_LCD_ENABLE) == 0u || (hw->lcdc & LCDC_WINDOW_ENABLE) != 0u ||
        (hw->lcdc & LCDC_BG_MAP) == 0u) {
        return false;
    }
    const uint8_t* map = hw->tile_ids[1];
    return has_tile_run(&map[10u * 32u], 0xA0u, 6u) && has_tile_run(&map[12u * 32u], 0xB0u, 6u);
}

static const CodecPortraitProfile* identify_portrait(const GBPresentationHardwareSnapshot* hw,
                                                     uint8_t base_tile) {
    const uint32_t base_offset =
        ((hw->lcdc & LCDC_TILE_DATA) != 0u) ? 0u : SIGNED_TILE_BASE_OFFSET;
    const uint32_t byte_off = base_offset + (uint32_t)base_tile * TILE_BYTES;
    if (byte_off + PORTRAIT_BYTES > GB_PRESENTATION_TILE_DATA_BYTES) {
        return NULL;
    }
    const uint32_t hash = fnv1a_bytes(&hw->tile_data[0][byte_off], PORTRAIT_BYTES);
    const size_t n = sizeof(kPortraitProfiles) / sizeof(kPortraitProfiles[0]);
    for (size_t i = 0u; i < n; ++i) {
        if (kPortraitProfiles[i].vram_hash == hash) {
            return &kPortraitProfiles[i];
        }
    }
    return NULL;
}

static bool find_portrait_origin(const GBPresentationHardwareSnapshot* hw,
                                 uint8_t base_tile,
                                 int32_t* out_sx,
                                 int32_t* out_sy) {
    const uint8_t* map = hw->tile_ids[1];
    for (uint32_t r = 0u; r < 32u; ++r) {
        const uint32_t r_next = (r + 1u) & 31u;
        for (uint32_t c = 0u; c < 32u; ++c) {
            const uint32_t c_next = (c + 1u) & 31u;
            if (map[r * 32u + c] == base_tile &&
                map[r * 32u + c_next] == (uint8_t)(base_tile + 1u) &&
                map[r_next * 32u + c] == (uint8_t)(base_tile + 4u)) {
                int32_t sx = (int32_t)(((c * 8u) - (uint32_t)hw->scx) & 0xFFu);
                int32_t sy = (int32_t)(((r * 8u) - (uint32_t)hw->scy) & 0xFFu);
                if (sx >= 224) {
                    sx -= 256;
                }
                if (sy >= 208) {
                    sy -= 256;
                }
                *out_sx = sx;
                *out_sy = sy;
                return true;
            }
        }
    }
    return false;
}

static uint32_t next_rng(CodecLipsyncState* state) {
    uint32_t x = state->rng;
    if (x == 0u) {
        x = 0x6D2B79F5u;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state->rng = x;
    return x;
}

static void init_blink_channels(CodecLipsyncState* state) {
    state->blink_left.timer = 95u;
    state->blink_left.step = 0u;
    state->blink_right.timer = 215u;
    state->blink_right.step = 0u;
}

static uint8_t step_blink_channel(CodecLipsyncState* state,
                                  CodecBlinkChannel* self,
                                  const CodecBlinkChannel* peer,
                                  uint32_t channel_salt) {
    if (self->step > 0u) {
        const uint8_t s = self->step;
        if (self->step >= BLINK_ENVELOPE_FRAMES) {
            self->step = 0u;
            const uint32_t r = next_rng(state) ^ (channel_salt * 0x9E3779B9u);
            self->timer = 192u + (r % 139u); /* 192..330 frames (~3.2s - 5.5s) */
        } else {
            self->step++;
        }
        /* 6-frame blink envelope: 1 (half), 2..4 (closed), 5 (half), 6 (open). */
        if (s == 1u || s == 5u) {
            return 1u;
        }
        if (s >= 2u && s <= 4u) {
            return 2u;
        }
        return 0u;
    }

    if (self->timer > 0u) {
        self->timer--;
        return 0u;
    }

    if (peer->step > 0u || peer->timer < BLINK_GUARD_FRAMES) {
        self->timer = BLINK_DEFER_FRAMES;
        return 0u;
    }

    self->step = 1u;
    return 1u;
}

static void paint_patch_pixels(const GBPresentationHardwareSnapshot* hw,
                               uint8_t base_tile,
                               int32_t sx,
                               int32_t sy,
                               const CodecPatchPixel* pixels,
                               size_t count,
                               uint32_t* surface,
                               uint32_t surface_width,
                               uint32_t surface_stride,
                               uint32_t margin_left) {
    if (pixels == NULL || count == 0u) {
        return;
    }
    for (size_t i = 0u; i < count; ++i) {
        const int32_t px = sx + (int32_t)pixels[i].dx;
        const int32_t py = sy + (int32_t)pixels[i].dy;
        if (px < 0 || px >= (int32_t)GB_PRESENTATION_ACCURATE_WIDTH || py < 0 ||
            py >= (int32_t)GB_PRESENTATION_ACCURATE_HEIGHT) {
            continue;
        }
        const uint32_t dst_x = margin_left + (uint32_t)px;
        if (dst_x >= surface_width) {
            continue;
        }
        const uint8_t line_lcdc = hw->scanline_lcdc[py];
        if ((line_lcdc & LCDC_LCD_ENABLE) == 0u || (line_lcdc & LCDC_BG_ENABLE) == 0u ||
            (line_lcdc & LCDC_WINDOW_ENABLE) != 0u) {
            continue;
        }
        const uint32_t bg_map = ((line_lcdc & LCDC_BG_MAP) != 0u) ? 1u : 0u;
        const uint32_t bg_x = ((uint32_t)px + (uint32_t)hw->scx) & 0xFFu;
        const uint32_t bg_y = ((uint32_t)py + (uint32_t)hw->scy) & 0xFFu;
        const uint32_t map_idx = (bg_y >> 3) * 32u + (bg_x >> 3);
        const uint8_t expected_tile =
            (uint8_t)(base_tile + ((uint32_t)pixels[i].dy >> 3) * 4u + ((uint32_t)pixels[i].dx >> 3));
        if (hw->tile_ids[bg_map][map_idx] != expected_tile) {
            continue;
        }
        const uint8_t pal = hw->tile_attributes[bg_map][map_idx] & 0x07u;
        const uint8_t shade = pixels[i].shade & 0x03u;
        const size_t pal_off = (size_t)pal * 8u + (size_t)shade * 2u;
        const uint16_t rgb555 = (uint16_t)hw->background_palettes[pal_off] |
                                ((uint16_t)hw->background_palettes[pal_off + 1u] << 8);
        surface[(size_t)py * surface_stride + dst_x] = (uint32_t)(rgb555 & 0x7FFFu);
    }
}

static void render_portrait(const GBPresentationHardwareSnapshot* hw,
                            uint8_t base_tile,
                            const CodecPortraitProfile* profile,
                            uint8_t mouth_state,
                            uint8_t blink_state,
                            uint32_t* surface,
                            uint32_t surface_width,
                            uint32_t surface_stride,
                            uint32_t margin_left) {
    if (profile == NULL || (mouth_state == 0u && blink_state == 0u)) {
        return;
    }
    int32_t sx = 0;
    int32_t sy = 0;
    if (!find_portrait_origin(hw, base_tile, &sx, &sy)) {
        return;
    }
    if (mouth_state == 1u) {
        paint_patch_pixels(hw, base_tile, sx, sy, profile->mouth_s1, profile->mouth_s1_count,
                           surface, surface_width, surface_stride, margin_left);
    } else if (mouth_state == 2u) {
        paint_patch_pixels(hw, base_tile, sx, sy, profile->mouth_s2, profile->mouth_s2_count,
                           surface, surface_width, surface_stride, margin_left);
    }
    if (blink_state == 1u) {
        paint_patch_pixels(hw, base_tile, sx, sy, profile->blink_s1, profile->blink_s1_count,
                           surface, surface_width, surface_stride, margin_left);
    } else if (blink_state == 2u) {
        paint_patch_pixels(hw, base_tile, sx, sy, profile->blink_s2, profile->blink_s2_count,
                           surface, surface_width, surface_stride, margin_left);
    }
}

void codec_lipsync_reset(CodecLipsyncState* state) {
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->rng = 0x51A7C0DEu;
    init_blink_channels(state);
}

void codec_lipsync_apply(CodecLipsyncState* state,
                         const GBPresentationHardwareSnapshot* hw,
                         const uint8_t* wram,
                         size_t wram_size,
                         uint32_t* surface,
                         uint32_t surface_width,
                         uint32_t surface_stride,
                         uint32_t margin_left,
                         bool enabled) {
    if (state == NULL || hw == NULL || surface == NULL || !enabled) {
        return;
    }
    if (!detect_codec_screen(hw)) {
        if (state->codec_active) {
            state->codec_active = false;
            state->dialogue_hash = 0u;
            state->dialogue_hold_frames = 0u;
            state->mouth_tick = 0u;
            init_blink_channels(state);
        }
        return;
    }

    if (!state->initialized) {
        state->initialized = true;
        if (state->rng == 0u) {
            state->rng = 0x51A7C0DEu;
        }
    }

    const uint32_t dhash = hash_dialogue_box(hw);
    if (!state->codec_active) {
        state->codec_active = true;
        state->dialogue_hash = dhash;
        state->dialogue_hold_frames = 0u;
        state->mouth_tick = 0u;
        init_blink_channels(state);
    } else if (dhash != state->dialogue_hash) {
        state->dialogue_hash = dhash;
        state->dialogue_hold_frames = DIALOGUE_HOLD_FRAMES;
    } else if (state->dialogue_hold_frames > 0u) {
        state->dialogue_hold_frames--;
    }

    state->frame_counter++;
    uint8_t active_mouth = 0u;
    if (state->dialogue_hold_frames > 0u) {
        static const uint8_t kMouthCadence[6] = {1u, 2u, 1u, 2u, 0u, 1u};
        active_mouth = kMouthCadence[(state->mouth_tick / MOUTH_STEP_FRAMES) % 6u];
        state->mouth_tick++;
    } else {
        state->mouth_tick = 0u;
    }

    const uint8_t speaker =
        (wram != NULL && wram_size > WRAM_SPEAKER_ADDR) ? wram[WRAM_SPEAKER_ADDR] : 0u;
    const uint8_t mouth_left = (speaker != 0u && speaker != SPEAKER_ID_SNAKE) ? active_mouth : 0u;
    const uint8_t mouth_right = (speaker == SPEAKER_ID_SNAKE) ? active_mouth : 0u;

    const CodecPortraitProfile* left_profile = identify_portrait(hw, LEFT_PORTRAIT_BASE_TILE);
    const CodecPortraitProfile* right_profile = identify_portrait(hw, RIGHT_PORTRAIT_BASE_TILE);

    const uint8_t blink_left = (left_profile != NULL && left_profile->blink_s2_count > 0u)
                                   ? step_blink_channel(state, &state->blink_left, &state->blink_right, 1u)
                                   : 0u;
    const uint8_t blink_right = (right_profile != NULL && right_profile->blink_s2_count > 0u)
                                    ? step_blink_channel(state, &state->blink_right, &state->blink_left, 2u)
                                    : 0u;

    render_portrait(hw, LEFT_PORTRAIT_BASE_TILE, left_profile, mouth_left, blink_left,
                    surface, surface_width, surface_stride, margin_left);
    render_portrait(hw, RIGHT_PORTRAIT_BASE_TILE, right_profile, mouth_right, blink_right,
                    surface, surface_width, surface_stride, margin_left);
}
