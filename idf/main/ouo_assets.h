#pragma once

#include <stdint.h>

#ifndef PROGMEM
#define PROGMEM
#endif

struct OuOSpriteAsset {
    uint16_t handle;
    uint16_t width;
    uint16_t height;
    int16_t hotspotX;
    int16_t hotspotY;
    uint32_t offset;
};

extern const uint16_t OUO_SPRITE_COUNT;
extern const OuOSpriteAsset OUO_SPRITES[] PROGMEM;
extern const uint16_t OUO_SPRITE_PIXELS[] PROGMEM;
extern const uint8_t OUO_SPRITE_ALPHA[] PROGMEM;

const OuOSpriteAsset* findOuOSprite(uint16_t handle);
