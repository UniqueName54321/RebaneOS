/* gcc -std=c11 -Wall -Wextra tests/framebuffer_layout_host_test.c
 *     -o build/framebuffer_layout_host_test */
#include "../include/framebuffer_layout.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    uint8_t positions[3], sizes[3];
    const uint8_t packed[] = {16, 8, 8, 8, 0, 8};
    const uint8_t aligned[] = {0, 0, 16, 8, 8, 8, 0, 8};
    const uint8_t incomplete[] = {0, 0, 16, 8, 8, 8};
    const uint8_t overlapping[] = {16, 8, 16, 8, 0, 8};
    const uint8_t outside[] = {24, 8, 8, 8, 0, 8};
    assert(framebuffer_decode_rgb(packed, sizeof(packed), 32, positions, sizes));
    assert(positions[0] == 16 && positions[1] == 8 && positions[2] == 0);
    assert(sizes[0] == 8 && sizes[1] == 8 && sizes[2] == 8);
    assert(framebuffer_decode_rgb(aligned, sizeof(aligned), 32, positions, sizes));
    assert(positions[0] == 16 && positions[1] == 8 && positions[2] == 0);
    assert(!framebuffer_decode_rgb(incomplete, sizeof(incomplete), 32, positions, sizes));
    assert(!framebuffer_decode_rgb(overlapping, sizeof(overlapping), 32, positions, sizes));
    assert(!framebuffer_decode_rgb(outside, sizeof(outside), 24, positions, sizes));
    assert(!framebuffer_decode_rgb(packed, 5, 32, positions, sizes));
    puts("Framebuffer RGB layout: packed/aligned masks and invalid layouts PASS");
    return 0;
}
