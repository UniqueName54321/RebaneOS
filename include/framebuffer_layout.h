#ifndef REBANE_FRAMEBUFFER_LAYOUT_H
#define REBANE_FRAMEBUFFER_LAYOUT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Multiboot specifies RGB bytes at offset 110. Some Limine releases align the
 * color union to offset 112. Decode actual complete masks, never invent a
 * missing channel: the latter rotates colors on the aligned layout. */
static inline bool framebuffer_decode_rgb(const uint8_t* bytes, size_t length,
    uint8_t bpp, uint8_t positions[3], uint8_t sizes[3]) {
    for (size_t offset = 0; offset <= 2 && offset + 6 <= length; offset += 2) {
        if (offset && (bytes[0] || bytes[1])) break;
        uint32_t occupied = 0;
        bool valid = true;
        for (unsigned channel = 0; channel < 3; channel++) {
            unsigned position = bytes[offset + channel * 2];
            unsigned size = bytes[offset + channel * 2 + 1];
            if (!size || size > 8 || position + size > bpp || position >= 32) {
                valid = false;
                break;
            }
            uint32_t mask = ((1U << size) - 1) << position;
            if (occupied & mask) { valid = false; break; }
            occupied |= mask;
        }
        if (valid) {
            for (unsigned channel = 0; channel < 3; channel++) {
                positions[channel] = bytes[offset + channel * 2];
                sizes[channel] = bytes[offset + channel * 2 + 1];
            }
            return true;
        }
    }
    return false;
}
#endif
