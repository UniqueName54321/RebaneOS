/* gcc -std=c11 -Wall -Wextra -Iinclude tests/linux_x11_host_test.c
 *     src/kernel/linux_x11.c -o build/linux_x11_host_test */
#include "../include/linux_x11.h"
#include "../include/display.h"
#include "../include/mouse.h"
#include "../include/pmm.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t framebuffer[64 * 64];
static bool little;
static size_t allocated_pages;
void *pmm_alloc(size_t pages) {
    void *memory = calloc(pages, PMM_PAGE_SIZE);
    if (memory) allocated_pages += pages;
    return memory;
}
bool pmm_free(void *memory, size_t pages) {
    free(memory);
    assert(allocated_pages >= pages);
    allocated_pages -= pages;
    return true;
}
bool display_get_framebuffer_size(uint32_t *width, uint32_t *height) {
    *width = *height = 64;
    return true;
}
bool display_fill_rect_rgb(uint32_t x, uint32_t y, uint32_t width,
                          uint32_t height, uint8_t r, uint8_t g, uint8_t b) {
    assert(x + width <= 64 && y + height <= 64);
    for (uint32_t yy = y; yy < y + height; yy++)
        for (uint32_t xx = x; xx < x + width; xx++)
            framebuffer[yy * 64 + xx] = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
    return true;
}
void display_present(void) {}
void mouse_get_state(input_mouse_state_t *state) { memset(state, 0, sizeof(*state)); }
uint64_t timer_get_ticks(void) { return 0; }
static void put16(uint8_t *p, unsigned n) {
    p[little ? 0 : 1] = n;
    p[little ? 1 : 0] = n >> 8;
}
static void put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++) p[little ? i : 3 - i] = n >> (8 * i);
}
static void send_request(uint8_t *q, size_t length) {
    put16(q + 2, length / 4);
    /* Requests may be split across arbitrary socket writes. */
    for (size_t offset = 0; offset < length;) {
        size_t size = length - offset < 7 ? length - offset : 7;
        assert(linux_x11_write(q + offset, size) == (long)size);
        offset += size;
    }
}
static void planar_upload(void) {
    const uint8_t address[] = {1, 0, '/', 't', 'm', 'p', '/', '.', 'X', '1', '1',
                               '-', 'u', 'n', 'i', 'x', '/', 'X', '0', 0};
    assert(linux_x11_connect(address, sizeof(address)) == 0);
    uint8_t setup[12] = {0}, response[144];
    setup[0] = little ? 'l' : 'B';
    put16(setup + 2, 11);
    assert(linux_x11_write(setup, sizeof(setup)) == sizeof(setup));
    assert(linux_x11_read(response, sizeof(response)) == sizeof(response));
    assert(response[0] == 1);
    uint8_t window[32] = {1};
    put32(window + 4, 0x200001); put32(window + 8, 0x100);
    put16(window + 16, 2); put16(window + 18, 1); put16(window + 22, 1);
    send_request(window, sizeof(window));
    uint8_t gc[16] = {55};
    put32(gc + 4, 0x200002); put32(gc + 8, 0x200001);
    send_request(gc, sizeof(gc));
    uint8_t map[8] = {8}; put32(map + 4, 0x200001);
    send_request(map, sizeof(map));
    uint8_t image[24 + 24 * 4] = {72, 1};
    put32(image + 4, 0x200001); put32(image + 8, 0x200002);
    put16(image + 12, 2); put16(image + 14, 1); image[21] = 24;
    const uint32_t colors[] = {0x804020, 0x102040};
    for (unsigned plane = 0; plane < 24; plane++)
        for (unsigned pixel = 0; pixel < 2; pixel++)
            if (colors[pixel] & (1U << (23 - plane)))
                image[24 + plane * 4] |= 1U << (little ? pixel : 7 - pixel);
    send_request(image, sizeof(image));
    assert(linux_x11_available() == 0); /* No BadMatch or other protocol error. */
    linux_x11_refresh();
    assert(framebuffer[0] == colors[0] && framebuffer[1] == colors[1]);
    linux_x11_reset();
    assert(allocated_pages == 0);
}
int main(void) {
    little = true; planar_upload();
    little = false; planar_upload();
    puts("X11 planar uploads: little/big endian, fragmented writes, exact pixels PASS");
    return 0;
}
