#include "linux_x11.h"
#include "display.h"
#include "mouse.h"
#include "pmm.h"
#include "timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Core X11, deliberately without extensions. In particular, advertising no
 * SHAPE or RENDER makes clients use their ordinary core drawing paths. All
 * resources and pixels below originate in requests, not application knowledge. */
#define ROOT 0x100U
#define COLORMAP 0x101U
#define VISUAL 0x102U
#define INPUT_SIZE 262144U
#define OUTPUT_SIZE 1048576U
#define REPLY_RESERVE 131072U
#define OBJECTS 128U
#define ATOMS 256U
#define PROPERTIES 128U
#define CLIPS 32U
#define MAX_PROPERTY 65536U
#define EXPOSURE (1U << 15)
#define STRUCTURE (1U << 17)
#define SUBSTRUCTURE (1U << 19)

struct drawable {
    uint32_t id, parent, background, border, events, colormap;
    int x, y;
    unsigned width, height, border_width, depth, klass;
    bool mapped, override_redirect, pixmap;
    uint32_t *pixels;
    size_t pages;
};
struct rectangle { int x, y; unsigned width, height; };
struct gc {
    uint32_t id, foreground, background, plane_mask;
    unsigned function, line_width, arc_mode;
    int clip_x, clip_y;
    unsigned clip_count;
    bool clip_enabled, graphics_exposures;
    struct rectangle clips[CLIPS];
};
struct atom { unsigned length; char name[128]; };
struct property {
    uint32_t window, atom, type;
    unsigned format, bytes;
    uint8_t *data;
    size_t pages;
};
static struct drawable drawables[OBJECTS];
static struct gc gcs[OBJECTS];
static uint32_t fonts[OBJECTS], colormaps[OBJECTS];
static struct atom atoms[ATOMS];
static struct property properties[PROPERTIES];
static unsigned atom_count;
static uint8_t *input, *output;
static unsigned input_used, output_head, output_count;
static unsigned screen_width, screen_height;
static uint16_t sequence;
static bool connected, initialized, little_endian, failed;
static uint32_t focus = ROOT, selection_atom[32], selection_owner[32];
static bool dirty;

static void zero(void *p, size_t n) {
    uint8_t *b = p;
    while (n--) *b++ = 0;
}
static void copy(void *to, const void *from, size_t n) {
    uint8_t *d = to;
    const uint8_t *s = from;
    while (n--) *d++ = *s++;
}
static unsigned padded(unsigned n) { return (n + 3U) & ~3U; }
static uint16_t get16(const uint8_t *p) {
    return little_endian ? (uint16_t)(p[0] | p[1] << 8) :
        (uint16_t)(p[1] | p[0] << 8);
}
static uint32_t get32(const uint8_t *p) {
    return little_endian ? (uint32_t)get16(p) | (uint32_t)get16(p + 2) << 16 :
        (uint32_t)get16(p) << 16 | get16(p + 2);
}
static int signed16(const uint8_t *p) { return (int16_t)get16(p); }
static void put16(uint8_t *p, unsigned n) {
    p[little_endian ? 0 : 1] = (uint8_t)n;
    p[little_endian ? 1 : 0] = (uint8_t)(n >> 8);
}
static void put32(uint8_t *p, uint32_t n) {
    put16(p + (little_endian ? 0 : 2), n);
    put16(p + (little_endian ? 2 : 0), n >> 16);
}
static unsigned bits(uint32_t mask) {
    unsigned n = 0;
    while (mask) { n += mask & 1U; mask >>= 1; }
    return n;
}
static bool enqueue(const void *data, unsigned n) {
    if (n > OUTPUT_SIZE - output_count) { failed = true; return false; }
    const uint8_t *p = data;
    unsigned tail = (output_head + output_count) % OUTPUT_SIZE;
    for (unsigned i = 0; i < n; i++) output[(tail + i) % OUTPUT_SIZE] = p[i];
    output_count += n;
    return true;
}
static void reply(uint8_t *r, unsigned n, unsigned detail) {
    zero(r, n);
    r[0] = 1;
    r[1] = (uint8_t)detail;
    put16(r + 2, sequence);
    put32(r + 4, (n - 32U) / 4U);
}
static void error(unsigned code, uint32_t value, unsigned opcode) {
    uint8_t r[32];
    zero(r, sizeof(r));
    r[1] = (uint8_t)code;
    put16(r + 2, sequence);
    put32(r + 4, value);
    r[10] = (uint8_t)opcode;
    enqueue(r, sizeof(r));
}
static struct drawable *drawable(uint32_t id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < OBJECTS; i++) if (drawables[i].id == id) return &drawables[i];
    return NULL;
}
static struct gc *gc(uint32_t id) {
    if (!id) return NULL;
    for (unsigned i = 0; i < OBJECTS; i++) if (gcs[i].id == id) return &gcs[i];
    return NULL;
}
static bool named_resource(uint32_t *list, uint32_t id) {
    if (!id) return false;
    for (unsigned i = 0; i < OBJECTS; i++) if (list[i] == id) return true;
    return false;
}
static bool valid_id(uint32_t id) {
    return (id & ~0x1fffffU) == 0x200000U && !drawable(id) && !gc(id) &&
        !named_resource(fonts, id) &&
        !named_resource(colormaps, id);
}
static bool add_resource(uint32_t *list, uint32_t id) {
    for (unsigned i = 0; i < OBJECTS; i++) if (!list[i]) { list[i] = id; return true; }
    return false;
}
static bool remove_resource(uint32_t *list, uint32_t id) {
    if (!id) return false;
    for (unsigned i = 0; i < OBJECTS; i++) if (list[i] == id) { list[i] = 0; return true; }
    return false;
}
static struct property *property(uint32_t window, uint32_t atom) {
    for (unsigned i = 0; i < PROPERTIES; i++)
        if (properties[i].window == window && properties[i].atom == atom) return &properties[i];
    return NULL;
}
static void delete_property(struct property *p) {
    if (p->data) pmm_free(p->data, p->pages);
    zero(p, sizeof(*p));
}
static void absolute_position(struct drawable *d, int *x, int *y) {
    *x = *y = 0;
    for (unsigned i = 0; d && d->id != ROOT && i < OBJECTS; i++) {
        *x += d->x + (int)d->border_width;
        *y += d->y + (int)d->border_width;
        d = drawable(d->parent);
    }
}
static bool visible(struct drawable *d) {
    for (unsigned i = 0; d && i < OBJECTS; i++) {
        if (!d->mapped || d->pixmap) return false;
        if (d->id == ROOT) return true;
        d = drawable(d->parent);
    }
    return false;
}
static void paint_rect(int x, int y, unsigned w, unsigned h, uint32_t color) {
    if (x < 0) { if ((unsigned)-x >= w) return; w -= (unsigned)-x; x = 0; }
    if (y < 0) { if ((unsigned)-y >= h) return; h -= (unsigned)-y; y = 0; }
    if ((unsigned)x >= screen_width || (unsigned)y >= screen_height) return;
    if (w > screen_width - (unsigned)x) w = screen_width - (unsigned)x;
    if (h > screen_height - (unsigned)y) h = screen_height - (unsigned)y;
    display_fill_rect_rgb((unsigned)x, (unsigned)y, w, h,
        (uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color);
}
static void composite(struct drawable *d, int clip_x, int clip_y, int clip_r, int clip_b, unsigned level) {
    if (!d || !d->mapped || d->pixmap || level >= OBJECTS) return;
    int x, y;
    absolute_position(d, &x, &y);
    int left = x > clip_x ? x : clip_x, top = y > clip_y ? y : clip_y;
    int right = x + (int)d->width < clip_r ? x + (int)d->width : clip_r;
    int bottom = y + (int)d->height < clip_b ? y + (int)d->height : clip_b;
    if (d->id != ROOT && d->border_width) {
        int bx = x - (int)d->border_width, by = y - (int)d->border_width;
        int br = x + (int)d->width + (int)d->border_width;
        int bb = y + (int)d->height + (int)d->border_width;
        if (bx < clip_x) bx = clip_x;
        if (by < clip_y) by = clip_y;
        if (br > clip_r) br = clip_r;
        if (bb > clip_b) bb = clip_b;
        if (br > bx && bb > by) paint_rect(bx, by, (unsigned)(br - bx), (unsigned)(bb - by), d->border);
    }
    if (right <= left || bottom <= top) return;
    if (d->pixels) for (int yy = top; yy < bottom; yy++) {
        int xx = left;
        while (xx < right) {
            uint32_t color = d->pixels[(unsigned)(yy - y) * d->width + (unsigned)(xx - x)];
            int end = xx + 1;
            while (end < right && d->pixels[(unsigned)(yy - y) * d->width + (unsigned)(end - x)] == color) end++;
            paint_rect(xx, yy, (unsigned)(end - xx), 1, color);
            xx = end;
        }
    }
    for (unsigned i = 1; i < OBJECTS; i++)
        if (drawables[i].id && drawables[i].parent == d->id) composite(&drawables[i], left, top, right, bottom, level + 1);
}
void linux_x11_refresh(void) {
    if (!connected || !initialized) return;
    composite(&drawables[0], 0, 0, (int)screen_width, (int)screen_height, 0);
    dirty = false;
}
static bool resize(struct drawable *d, unsigned w, unsigned h) {
    if (!w || !h || w > 4096 || h > 4096) return false;
    size_t pages = ((size_t)w * h * sizeof(uint32_t) + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint32_t *pixels = pmm_alloc(pages);
    if (!pixels) return false;
    for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++)
        pixels[y * w + x] = d->pixels && x < d->width && y < d->height ?
            d->pixels[y * d->width + x] : d->background;
    if (d->pixels) pmm_free(d->pixels, d->pages);
    d->pixels = pixels; d->pages = pages; d->width = w; d->height = h;
    return true;
}
static void event(struct drawable *d, unsigned type) {
    uint8_t r[32];
    zero(r, sizeof(r)); r[0] = (uint8_t)type; put16(r + 2, sequence);
    put32(r + 4, d->id);
    if (type == 12) { put16(r + 12, d->width); put16(r + 14, d->height); }
    else {
        put32(r + 8, d->id);
        if (type == 22) {
            put16(r + 16, (unsigned)d->x); put16(r + 18, (unsigned)d->y);
            put16(r + 20, d->width); put16(r + 22, d->height);
            put16(r + 24, d->border_width); r[26] = d->override_redirect;
        } else r[12] = d->override_redirect;
    }
    if ((type == 12 && (d->events & EXPOSURE)) || (type != 12 && (d->events & STRUCTURE))) enqueue(r, 32);
    struct drawable *parent = drawable(d->parent);
    if (type != 12 && parent && (parent->events & SUBSTRUCTURE)) {
        put32(r + 4, parent->id); enqueue(r, 32);
    }
}
static void property_event(struct drawable *d, uint32_t atom, bool deleted) {
    if (!(d->events & (1U << 22))) return;
    uint8_t r[32]; zero(r, 32); r[0] = 28; put16(r + 2, sequence);
    put32(r + 4, d->id); put32(r + 8, atom);
    put32(r + 12, (uint32_t)timer_get_ticks() * 10U); r[16] = deleted;
    enqueue(r, 32);
}
static void destroy(struct drawable *d, unsigned level) {
    if (!d || d->id == ROOT || level >= OBJECTS) return;
    for (unsigned i = 1; i < OBJECTS; i++) if (drawables[i].id && drawables[i].parent == d->id) destroy(&drawables[i], level + 1);
    if (!d->pixmap) event(d, 17);
    for (unsigned i = 0; i < PROPERTIES; i++) if (properties[i].window == d->id) delete_property(&properties[i]);
    if (d->pixels) pmm_free(d->pixels, d->pages);
    zero(d, sizeof(*d)); dirty = true;
}
static uint32_t raster(unsigned function, uint32_t src, uint32_t dst) {
    switch (function) {
        case 0: return 0; case 1: return src & dst; case 2: return src & ~dst;
        case 3: return src; case 4: return ~src & dst; case 5: return dst;
        case 6: return src ^ dst; case 7: return src | dst; case 8: return ~(src | dst);
        case 9: return ~(src ^ dst); case 10: return ~dst; case 11: return src | ~dst;
        case 12: return ~src; case 13: return ~src | dst; case 14: return ~(src & dst);
        default: return ~0U;
    }
}
static void pixel(struct drawable *d, struct gc *g, int x, int y, uint32_t color) {
    if (!d->pixels || x < 0 || y < 0 || (unsigned)x >= d->width || (unsigned)y >= d->height) return;
    if (g->clip_enabled) {
        bool inside = false;
        for (unsigned i = 0; i < g->clip_count; i++) {
            struct rectangle *r = &g->clips[i];
            int xx = x - g->clip_x - r->x, yy = y - g->clip_y - r->y;
            if (xx >= 0 && yy >= 0 && (unsigned)xx < r->width && (unsigned)yy < r->height) { inside = true; break; }
        }
        if (!inside) return;
    }
    uint32_t *p = &d->pixels[(unsigned)y * d->width + (unsigned)x];
    uint32_t mask = g->plane_mask & (d->depth == 1 ? 1U : 0xffffffU);
    *p = (*p & ~mask) | (raster(g->function, color, *p) & mask);
    dirty = true;
}
static void fill(struct drawable *d, struct gc *g, int x, int y, unsigned w, unsigned h, uint32_t color) {
    int right = x + (int)w, bottom = y + (int)h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > (int)d->width) right = (int)d->width;
    if (bottom > (int)d->height) bottom = (int)d->height;
    for (int yy = y; yy < bottom; yy++) for (int xx = x; xx < right; xx++) pixel(d, g, xx, yy, color);
}
static void line(struct drawable *d, struct gc *g, int x, int y, int end_x, int end_y) {
    int dx = end_x > x ? end_x - x : x - end_x, sx = x < end_x ? 1 : -1;
    int dy = end_y > y ? y - end_y : end_y - y, sy = y < end_y ? 1 : -1;
    int e = dx + dy;
    unsigned width = g->line_width ? g->line_width : 1;
    for (;;) {
        fill(d, g, x - (int)width / 2, y - (int)width / 2, width, width, g->foreground);
        if (x == end_x && y == end_y) break;
        int twice = e * 2;
        if (twice >= dy) { e += dy; x += sx; }
        if (twice <= dx) { e += dx; y += sy; }
    }
}
/* Integer sine, in X11's 1/64 degree units; no FPU state in the kernel. */
static int sine(int angle) {
    angle %= 23040; if (angle < 0) angle += 23040;
    int sign = 1;
    if (angle > 11520) { angle -= 11520; sign = -1; }
    int64_t product = (int64_t)angle * (11520 - angle);
    return (int)(sign * 16 * product * 32768 / (5LL * 11520 * 11520 - 4 * product));
}
static bool arc_angle(int64_t vx, int64_t vy, int start, int extent, unsigned mode) {
    if (extent >= 23040 || extent <= -23040) return true;
    if (!extent) return false;
    if (extent < 0) { start += extent; extent = -extent; }
    int sx = sine(start + 5760), sy = sine(start);
    int ex = sine(start + extent + 5760), ey = sine(start + extent);
    if (mode == 0) {
        int mx = sine(start + extent / 2 + 5760), my = sine(start + extent / 2);
        int64_t side = (int64_t)(ex - sx) * (my - sy) - (int64_t)(ey - sy) * (mx - sx);
        int64_t point = (int64_t)(ex - sx) * (vy - sy) - (int64_t)(ey - sy) * (vx - sx);
        return side >= 0 ? point >= 0 : point <= 0;
    }
    int64_t a = (int64_t)sx * vy - (int64_t)sy * vx;
    int64_t b = vx * ey - vy * ex;
    return extent <= 11520 ? a >= 0 && b >= 0 : a >= 0 || b >= 0;
}
static void arc(struct drawable *d, struct gc *g, const uint8_t *r, bool filled) {
    int x = signed16(r), y = signed16(r + 2);
    unsigned w = get16(r + 4), h = get16(r + 6);
    int start = signed16(r + 8), extent = signed16(r + 10);
    if (!w || !h || !extent) return;
    int left = x < 0 ? 0 : x, top = y < 0 ? 0 : y;
    int right = x + (int)w, bottom = y + (int)h;
    if (right > (int)d->width) right = (int)d->width;
    if (bottom > (int)d->height) bottom = (int)d->height;
    unsigned stroke = g->line_width ? g->line_width : 1;
    for (int yy = top; yy < bottom; yy++) for (int xx = left; xx < right; xx++) {
        int64_t vx = ((int64_t)(2 * (xx - x) + 1) - w) * 32768 / w;
        int64_t vy = ((int64_t)h - (2 * (yy - y) + 1)) * 32768 / h;
        if (vx * vx + vy * vy > 1073741824LL || !arc_angle(vx, vy, start, extent, filled ? g->arc_mode : 1)) continue;
        if (!filled && w > stroke * 2 && h > stroke * 2) {
            int64_t ix = vx * w / (w - stroke * 2), iy = vy * h / (h - stroke * 2);
            if (ix * ix + iy * iy < 1073741824LL) continue;
        }
        pixel(d, g, xx, yy, g->foreground);
    }
}
static uint32_t intern(const uint8_t *name, unsigned n, bool only_existing) {
    if (n >= sizeof(atoms[0].name)) return 0;
    for (unsigned i = 0; i < atom_count; i++) {
        if (atoms[i].length != n) continue;
        unsigned j = 0; while (j < n && atoms[i].name[j] == (char)name[j]) j++;
        if (j == n) return i + 1;
    }
    if (only_existing || atom_count == ATOMS) return 0;
    struct atom *a = &atoms[atom_count++]; a->length = n; copy(a->name, name, n);
    return atom_count;
}
static void setup(void) {
    uint8_t r[144]; zero(r, sizeof(r)); r[0] = 1;
    put16(r + 2, 11); put16(r + 6, (sizeof(r) - 8) / 4);
    put32(r + 8, 1); put32(r + 12, 0x200000); put32(r + 16, 0x1fffff);
    put16(r + 24, 8); put16(r + 26, 65535); r[28] = 1; r[29] = 2;
    r[30] = little_endian ? 0 : 1; r[31] = little_endian ? 0 : 1;
    r[32] = 32; r[33] = 32; r[34] = 8; r[35] = 255;
    copy(r + 40, "RebaneOS", 8);
    r[48] = 1; r[49] = 1; r[50] = 32;
    r[56] = 24; r[57] = 32; r[58] = 32;
    put32(r + 64, ROOT); put32(r + 68, COLORMAP); put32(r + 72, 0xffffff);
    put16(r + 84, screen_width); put16(r + 86, screen_height);
    put16(r + 88, screen_width / 4); put16(r + 90, screen_height / 4);
    put16(r + 92, 1); put16(r + 94, 1); put32(r + 96, VISUAL);
    r[102] = 24; r[103] = 2;
    r[104] = 1; r[112] = 24; put16(r + 114, 1);
    put32(r + 120, VISUAL); r[124] = 4; r[125] = 8; put16(r + 126, 256);
    put32(r + 128, 0xff0000); put32(r + 132, 0xff00); put32(r + 136, 0xff);
    enqueue(r, sizeof(r)); initialized = true;
}
static bool attributes(struct drawable *d, uint32_t mask, const uint8_t *values) {
    if (mask & ~0x7fffU) return false;
    for (unsigned i = 0; i < 15; i++) if (mask & (1U << i)) {
        uint32_t v = get32(values); values += 4;
        switch (i) {
            case 0: if (v > 1) return false; break; /* no tiled backgrounds */
            case 1: d->background = v & 0xffffff; break;
            case 2: if (v) return false; break;
            case 3: d->border = v & 0xffffff; break;
            case 9: d->override_redirect = v != 0; break;
            case 11: d->events = v; break;
            case 13: d->colormap = v ? v : COLORMAP; break;
            default: break;
        }
    }
    return true;
}
static bool gc_values(struct gc *g, uint32_t mask, const uint8_t *values) {
    if (mask & ~0x7fffffU) return false;
    for (unsigned i = 0; i < 23; i++) if (mask & (1U << i)) {
        uint32_t v = get32(values); values += 4;
        switch (i) {
            case 0: if (v > 15) return false; g->function = v; break;
            case 1: g->plane_mask = v; break;
            case 2: g->foreground = v; break;
            case 3: g->background = v; break;
            case 4: if (v > 4096) return false; g->line_width = v; break;
            case 5: if (v) return false; break; /* solid lines only */
            case 8: if (v) return false; break; /* solid fill only */
            case 16: if (v > 1) return false; g->graphics_exposures = v != 0; break;
            case 17: g->clip_x = (int16_t)v; break;
            case 18: g->clip_y = (int16_t)v; break;
            case 19: if (v) return false; g->clip_enabled = false; break;
            case 22: if (v > 1) return false; g->arc_mode = v; break;
            default: break;
        }
    }
    return true;
}
static void dispatch(const uint8_t *q, unsigned n) {
    unsigned op = q[0];
    uint8_t r[64];
    uint32_t id = n >= 8 ? get32(q + 4) : 0;
    struct drawable *d = drawable(id);
    struct gc *g;
#define LENGTH(size) do { if (n != (unsigned)(size)) { error(16, n / 4, op); return; } } while (0)
#define MINIMUM(size) do { if (n < (unsigned)(size)) { error(16, n / 4, op); return; } } while (0)
#define WINDOW() do { if (!d || d->pixmap) { error(3, id, op); return; } } while (0)
#define DRAWABLE() do { if (!d) { error(9, id, op); return; } } while (0)
#define GC(offset) do { g = gc(get32(q + (offset))); if (!g) { error(13, get32(q + (offset)), op); return; } } while (0)
    switch (op) {
    case 1: { /* CreateWindow */
        MINIMUM(32); uint32_t mask = get32(q + 28); LENGTH(32 + 4 * bits(mask));
        if (!valid_id(id)) { error(14, id, op); return; }
        struct drawable *parent = drawable(get32(q + 8));
        if (!parent || parent->pixmap) { error(3, get32(q + 8), op); return; }
        unsigned klass = get16(q + 22); if (!klass) klass = parent->klass;
        unsigned depth = q[1] ? q[1] : parent->depth;
        if ((klass != 1 && klass != 2) || (klass == 1 && depth != 24) ||
            (get32(q + 24) && get32(q + 24) != VISUAL)) { error(8, id, op); return; }
        d = NULL; for (unsigned i = 1; i < OBJECTS; i++) if (!drawables[i].id) { d = &drawables[i]; break; }
        if (!d) { error(11, id, op); return; }
        zero(d, sizeof(*d)); d->parent = parent->id; d->depth = depth; d->klass = klass; d->colormap = COLORMAP;
        d->x = signed16(q + 12); d->y = signed16(q + 14); d->border_width = get16(q + 20);
        if (!attributes(d, mask, q + 32)) { zero(d, sizeof(*d)); error(2, mask, op); return; }
        if (klass == 1 && !resize(d, get16(q + 16), get16(q + 18))) { zero(d, sizeof(*d)); error(11, id, op); return; }
        if (klass == 2) { d->width = get16(q + 16); d->height = get16(q + 18); }
        d->id = id;
        if (parent->events & SUBSTRUCTURE) {
            zero(r, 32); r[0] = 16; put16(r + 2, sequence); put32(r + 4, parent->id); put32(r + 8, id);
            put16(r + 12, (unsigned)d->x); put16(r + 14, (unsigned)d->y);
            put16(r + 16, d->width); put16(r + 18, d->height); put16(r + 20, d->border_width); r[22] = d->override_redirect;
            enqueue(r, 32);
        }
        break;
    }
    case 2: MINIMUM(12); WINDOW(); LENGTH(12 + 4 * bits(get32(q + 8)));
        if (!attributes(d, get32(q + 8), q + 12)) error(2, get32(q + 8), op);
        break;
    case 3: LENGTH(8); WINDOW(); reply(r, 44, 0); put32(r + 8, VISUAL); put16(r + 12, d->klass);
        put32(r + 16, 0xffffff); r[25] = 1;
        r[26] = visible(d) ? 2 : d->mapped ? 1 : 0; r[27] = d->override_redirect; put32(r + 28, d->colormap);
        put32(r + 32, d->events); put32(r + 36, d->events); enqueue(r, 44); break;
    case 4: LENGTH(8); WINDOW(); destroy(d, 0); break;
    case 5: LENGTH(8); WINDOW();
        for (unsigned i = 1; i < OBJECTS; i++) if (drawables[i].id && drawables[i].parent == id) destroy(&drawables[i], 0);
        break;
    case 6: LENGTH(8); WINDOW(); if (q[1] > 1) error(2, q[1], op); break;
    case 7: { LENGTH(16); WINDOW(); struct drawable *p = drawable(get32(q + 8));
        if (!p || p->pixmap) { error(3, get32(q + 8), op); break; }
        struct drawable *ancestor = p; bool cycle = false;
        for (unsigned i = 0; ancestor && i < OBJECTS; i++) {
            if (ancestor == d) { cycle = true; break; } ancestor = drawable(ancestor->parent);
        }
        if (cycle || d->id == ROOT) { error(8, id, op); break; }
        d->parent = p->id; d->x = signed16(q + 12); d->y = signed16(q + 14); dirty = true;
        zero(r, 32); r[0] = 21; put16(r + 2, sequence); put32(r + 4, id); put32(r + 8, id);
        put32(r + 12, p->id); put16(r + 16, (unsigned)d->x); put16(r + 18, (unsigned)d->y); r[20] = d->override_redirect;
        if (d->events & STRUCTURE) enqueue(r, 32);
        break;
    }
    case 8: case 10: LENGTH(8); WINDOW();
        if (d->id != ROOT && d->mapped != (op == 8)) {
            d->mapped = op == 8; event(d, op == 8 ? 19 : 18);
            if (op == 8) event(d, 12);
            dirty = true;
        } break;
    case 9: case 11: LENGTH(8); WINDOW();
        for (unsigned i = 1; i < OBJECTS; i++) if (drawables[i].id && drawables[i].parent == id && !drawables[i].pixmap) {
            struct drawable *child = &drawables[i]; child->mapped = op == 9;
            event(child, op == 9 ? 19 : 18); if (op == 9) event(child, 12);
        } dirty = true; break;
    case 12: { MINIMUM(12); WINDOW(); unsigned mask = get16(q + 8); LENGTH(12 + bits(mask) * 4);
        if (mask & ~127U) { error(2, mask, op); break; }
        const uint8_t *v = q + 12; unsigned w = d->width, h = d->height;
        for (unsigned i = 0; i < 7; i++) if (mask & (1U << i)) {
            uint32_t value = get32(v); v += 4;
            if (i == 0) d->x = (int16_t)value;
            if (i == 1) d->y = (int16_t)value;
            if (i == 2) w = value;
            if (i == 3) h = value;
            if (i == 4) d->border_width = value & 65535;
            if (i >= 5) { /* Multiple top-level stacking is not implemented. */ error(17, value, op); return; }
        }
        if ((w != d->width || h != d->height) && !resize(d, w, h)) { error(11, id, op); break; }
        event(d, 22); if (d->mapped) event(d, 12); dirty = true; break;
    }
    case 14: LENGTH(8); DRAWABLE(); reply(r, 32, d->depth); put32(r + 8, ROOT);
        put16(r + 12, (unsigned)d->x); put16(r + 14, (unsigned)d->y); put16(r + 16, d->width);
        put16(r + 18, d->height); put16(r + 20, d->border_width); enqueue(r, 32); break;
    case 15: { LENGTH(8); WINDOW(); unsigned count = 0;
        for (unsigned i = 1; i < OBJECTS; i++) if (drawables[i].id && drawables[i].parent == id && !drawables[i].pixmap) count++;
        reply(r, 32, 0); put32(r + 4, count); put32(r + 8, ROOT); put32(r + 12, d->parent); put16(r + 16, count); enqueue(r, 32);
        for (unsigned i = 1; i < OBJECTS; i++) if (drawables[i].id && drawables[i].parent == id && !drawables[i].pixmap) {
            put32(r, drawables[i].id); enqueue(r, 4);
        } break;
    }
    case 16: MINIMUM(8); LENGTH(8 + padded(get16(q + 4))); reply(r, 32, 0);
        put32(r + 8, intern(q + 8, get16(q + 4), q[1] != 0)); enqueue(r, 32); break;
    case 17: { LENGTH(8); if (!id || id > atom_count) { error(5, id, op); break; }
        struct atom *a = &atoms[id - 1]; reply(r, 32, 0); put32(r + 4, padded(a->length) / 4); put16(r + 8, a->length); enqueue(r, 32);
        enqueue(a->name, a->length); zero(r, 4); enqueue(r, padded(a->length) - a->length); break;
    }
    case 18: { MINIMUM(24); WINDOW(); uint32_t atom = get32(q + 8), type = get32(q + 12);
        unsigned format = q[16], units = get32(q + 20);
        if (!atom || atom > atom_count || !type || type > atom_count) { error(5, !atom || atom > atom_count ? atom : type, op); break; }
        if (q[1] > 2 || (format != 8 && format != 16 && format != 32)) { error(2, format, op); break; }
        if (units > MAX_PROPERTY / (format / 8)) { error(11, units, op); break; }
        unsigned bytes = units * (format / 8); LENGTH(24 + padded(bytes));
        struct property *p = property(id, atom);
        if (p && q[1] && (p->type != type || p->format != format)) { error(8, atom, op); break; }
        unsigned old = p && q[1] ? p->bytes : 0;
        if (bytes > MAX_PROPERTY - old) { error(11, atom, op); break; }
        if (!p) for (unsigned i = 0; i < PROPERTIES; i++) if (!properties[i].window) { p = &properties[i]; break; }
        if (!p) { error(11, atom, op); break; }
        size_t pages = (old + bytes + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
        uint8_t *data = pages ? pmm_alloc(pages) : NULL;
        if (pages && !data) { error(11, atom, op); break; }
        if (q[1] == 1) { copy(data, q + 24, bytes); if (old) copy(data + bytes, p->data, old); }
        else { if (old) copy(data, p->data, old); if (bytes) copy(data + old, q + 24, bytes); }
        if (p->data) pmm_free(p->data, p->pages);
        p->window = id; p->atom = atom; p->type = type; p->format = format; p->bytes = old + bytes; p->data = data; p->pages = pages;
        property_event(d, atom, false); break;
    }
    case 19: { LENGTH(12); WINDOW(); struct property *p = property(id, get32(q + 8));
        if (p) { delete_property(p); property_event(d, get32(q + 8), true); } break;
    }
    case 20: { LENGTH(24); WINDOW(); uint32_t atom = get32(q + 8), type = get32(q + 12);
        struct property *p = property(id, atom); reply(r, 32, p ? p->format : 0);
        unsigned bytes = 0, offset = 0;
        if (p) {
            put32(r + 8, p->type);
            if (type && type != p->type) { put32(r + 12, p->bytes); r[1] = 0; }
            else {
                uint32_t words = get32(q + 16); if (words > p->bytes / 4) { error(2, words, op); break; }
                offset = words * 4; bytes = p->bytes - offset;
                if (get32(q + 20) < (bytes + 3) / 4) bytes = get32(q + 20) * 4;
                put32(r + 12, p->bytes - offset - bytes); put32(r + 16, bytes / (p->format / 8));
                put32(r + 4, padded(bytes) / 4);
            }
        }
        enqueue(r, 32); if (bytes) enqueue(p->data + offset, bytes); zero(r, 4); enqueue(r, padded(bytes) - bytes);
        if (p && q[1] && (!type || type == p->type) && offset + bytes == p->bytes) { delete_property(p); property_event(d, atom, true); }
        break;
    }
    case 21: { LENGTH(8); WINDOW(); unsigned count = 0;
        for (unsigned i = 0; i < PROPERTIES; i++) if (properties[i].window == id) count++;
        reply(r, 32, 0); put32(r + 4, count); put16(r + 8, count); enqueue(r, 32);
        for (unsigned i = 0; i < PROPERTIES; i++) if (properties[i].window == id) { put32(r, properties[i].atom); enqueue(r, 4); }
        break;
    }
    case 22: { LENGTH(16); if (id) WINDOW(); uint32_t atom = get32(q + 8);
        unsigned i = 0; while (i < 32 && selection_atom[i] && selection_atom[i] != atom) i++;
        if (i == 32) { error(11, atom, op); break; } selection_atom[i] = atom; selection_owner[i] = id; break;
    }
    case 23: LENGTH(8); reply(r, 32, 0);
        for (unsigned i = 0; i < 32; i++) if (selection_atom[i] == id) put32(r + 8, selection_owner[i]);
        enqueue(r, 32); break;
    case 24: LENGTH(24); WINDOW(); zero(r, 32); r[0] = 31; put16(r + 2, sequence);
        put32(r + 4, get32(q + 20)); put32(r + 8, id); put32(r + 12, get32(q + 8)); put32(r + 16, get32(q + 12)); enqueue(r, 32); break;
    case 25: LENGTH(44); if (id != 0 && id != 1) WINDOW();
        copy(r, q + 12, 32); r[0] |= 0x80; put16(r + 2, sequence); enqueue(r, 32); break;
    case 36: case 37: LENGTH(4); break;
    case 38: { LENGTH(8); WINDOW(); input_mouse_state_t mouse; mouse_get_state(&mouse); int x, y;
        absolute_position(d, &x, &y); reply(r, 32, 1); put32(r + 8, ROOT);
        for (unsigned i = 1; i < OBJECTS; i++) if (drawables[i].id && drawables[i].parent == id && visible(&drawables[i])) {
            int cx, cy; absolute_position(&drawables[i], &cx, &cy);
            if (mouse.x >= cx && mouse.y >= cy && mouse.x < cx + (int)drawables[i].width && mouse.y < cy + (int)drawables[i].height) put32(r + 12, drawables[i].id);
        }
        put16(r + 16, (unsigned)mouse.x); put16(r + 18, (unsigned)mouse.y);
        put16(r + 20, (unsigned)(mouse.x - x)); put16(r + 22, (unsigned)(mouse.y - y));
        put16(r + 24, (mouse.left_btn ? 256 : 0) | (mouse.middle_btn ? 512 : 0) | (mouse.right_btn ? 1024 : 0)); enqueue(r, 32); break;
    }
    case 39: LENGTH(16); WINDOW(); reply(r, 32, 0); enqueue(r, 32); break;
    case 40: { LENGTH(16); WINDOW(); struct drawable *to = drawable(get32(q + 8));
        if (!to || to->pixmap) { error(3, get32(q + 8), op); break; }
        int x, y, tx, ty; absolute_position(d, &x, &y); absolute_position(to, &tx, &ty);
        reply(r, 32, 1); put16(r + 12, (unsigned)(x + signed16(q + 12) - tx)); put16(r + 14, (unsigned)(y + signed16(q + 14) - ty)); enqueue(r, 32); break;
    }
    case 42: LENGTH(12); if (id > 1) WINDOW(); focus = id; break;
    case 43: LENGTH(4); reply(r, 32, 0); put32(r + 8, focus); enqueue(r, 32); break;
    case 45: { MINIMUM(12); LENGTH(12 + padded(get16(q + 8)));
        if (!valid_id(id)) { error(14, id, op); break; }
        bool matches = get16(q + 8) == 5;
        for (unsigned i = 0; matches && i < 5; i++) if (q[12 + i] != (uint8_t)"fixed"[i]) matches = false;
        if (!matches) error(15, 0, op);
        else if (!add_resource(fonts, id)) error(11, id, op);
        break;
    }
    case 46: LENGTH(8); if (!remove_resource(fonts, id)) error(7, id, op); break;
    case 47: LENGTH(8); if (!named_resource(fonts, id)) { error(7, id, op); break; }
        reply(r, 60, 0); put16(r + 8, 0); put16(r + 10, 8); put16(r + 12, 8); put16(r + 14, 12); put16(r + 16, 4);
        copy(r + 24, r + 8, 12); put16(r + 42, 255); r[51] = 1; put16(r + 52, 12); put16(r + 54, 4); enqueue(r, 60); break;
    case 48: MINIMUM(8); if (!named_resource(fonts, id)) { error(7, id, op); break; }
        reply(r, 32, 0); put16(r + 8, 12); put16(r + 10, 4); put16(r + 12, 12); put16(r + 14, 4);
        put32(r + 16, ((n - 8) / 2 - (q[1] ? 1 : 0)) * 8); put32(r + 24, get32(r + 16)); enqueue(r, 32); break;
    case 49: { MINIMUM(8); LENGTH(8 + padded(get16(q + 6)));
        /* Glob matching over the only installed core font, \"fixed\". */
        unsigned reachable = 1;
        for (unsigned i = 0; i < get16(q + 6); i++) {
            unsigned next = 0;
            if (q[8 + i] == '*') {
                for (unsigned j = 0; j <= 5; j++) if (reachable & (1U << j)) {
                    next = 63U & ~((1U << j) - 1U); break;
                }
            } else for (unsigned j = 0; j < 5; j++)
                if ((reachable & (1U << j)) && (q[8 + i] == '?' || q[8 + i] == (uint8_t)"fixed"[j])) next |= 1U << (j + 1);
            reachable = next;
        }
        bool found = (reachable & 32U) && get16(q + 4);
        reply(r, 32, 0); put32(r + 4, found ? 2 : 0); put16(r + 8, found ? 1 : 0); enqueue(r, 32);
        if (found) { zero(r, 8); r[0] = 5; copy(r + 1, "fixed", 5); enqueue(r, 8); }
        break;
    }
    case 53: { LENGTH(16); if (!valid_id(id)) { error(14, id, op); break; }
        if (!drawable(get32(q + 8))) { error(9, get32(q + 8), op); break; }
        if (q[1] != 1 && q[1] != 24) { error(2, q[1], op); break; }
        d = NULL; for (unsigned i = 1; i < OBJECTS; i++) if (!drawables[i].id) { d = &drawables[i]; break; }
        if (!d || !resize(d, get16(q + 12), get16(q + 14))) { error(11, id, op); break; }
        d->id = id; d->depth = q[1]; d->pixmap = true; break;
    }
    case 54: LENGTH(8); if (!d || !d->pixmap) error(4, id, op); else destroy(d, 0); break;
    case 55: { MINIMUM(16); LENGTH(16 + 4 * bits(get32(q + 12)));
        if (!valid_id(id)) { error(14, id, op); break; }
        if (!drawable(get32(q + 8))) { error(9, get32(q + 8), op); break; }
        g = NULL; for (unsigned i = 0; i < OBJECTS; i++) if (!gcs[i].id) { g = &gcs[i]; break; }
        if (!g) { error(11, id, op); break; }
        zero(g, sizeof(*g)); g->function = 3; g->plane_mask = ~0U; g->background = 1; g->arc_mode = 1; g->graphics_exposures = true;
        if (!gc_values(g, get32(q + 12), q + 16)) { zero(g, sizeof(*g)); error(2, get32(q + 12), op); break; }
        g->id = id; break;
    }
    case 56: MINIMUM(12); GC(4); LENGTH(12 + 4 * bits(get32(q + 8)));
        if (!gc_values(g, get32(q + 8), q + 12)) error(2, get32(q + 8), op);
        break;
    case 57: { LENGTH(16); GC(4); struct gc *to = gc(get32(q + 8));
        if (!to) { error(13, get32(q + 8), op); break; }
        unsigned mask = get32(q + 12); uint8_t values[92]; unsigned used = 0;
        if (mask & ~0x7fffffU) { error(2, mask, op); break; }
        for (unsigned i = 0; i < 23; i++) if (mask & (1U << i)) {
            uint32_t v = 0;
            switch (i) {
                case 0: v = g->function; break; case 1: v = g->plane_mask; break;
                case 2: v = g->foreground; break; case 3: v = g->background; break;
                case 16: v = g->graphics_exposures; break;
                case 4: v = g->line_width; break; case 17: v = (uint32_t)g->clip_x; break;
                case 18: v = (uint32_t)g->clip_y; break; case 22: v = g->arc_mode; break;
                default: break;
            } put32(values + used, v); used += 4;
        }
        gc_values(to, mask, values); if (mask & (1U << 19)) {
            to->clip_enabled = g->clip_enabled; to->clip_count = g->clip_count; copy(to->clips, g->clips, sizeof(g->clips));
        } break;
    }
    case 59: MINIMUM(12); GC(4); if ((n - 12) % 8) { error(16, n / 4, op); break; }
        if ((n - 12) / 8 > CLIPS) { error(11, id, op); break; }
        g->clip_x = signed16(q + 8); g->clip_y = signed16(q + 10); g->clip_count = (n - 12) / 8; g->clip_enabled = true;
        for (unsigned i = 0; i < g->clip_count; i++) {
            const uint8_t *v = q + 12 + i * 8; g->clips[i].x = signed16(v); g->clips[i].y = signed16(v + 2);
            g->clips[i].width = get16(v + 4); g->clips[i].height = get16(v + 6);
        } break;
    case 60: LENGTH(8); GC(4); zero(g, sizeof(*g)); break;
    case 61: { LENGTH(16); WINDOW(); struct gc clear; zero(&clear, sizeof(clear)); clear.function = 3; clear.plane_mask = ~0U;
        int x = signed16(q + 8), y = signed16(q + 10); unsigned w = get16(q + 12), h = get16(q + 14);
        if (!w) w = x < (int)d->width ? (unsigned)((int)d->width - x) : 0;
        if (!h) h = y < (int)d->height ? (unsigned)((int)d->height - y) : 0;
        fill(d, &clear, x, y, w, h, d->background); if (q[1]) event(d, 12); break;
    }
    case 62: case 63: { LENGTH(op == 62 ? 28 : 32); DRAWABLE(); struct drawable *to = drawable(get32(q + 8)); GC(12);
        if (!to) { error(9, get32(q + 8), op); break; }
        int sx = signed16(q + 16), sy = signed16(q + 18), dx = signed16(q + 20), dy = signed16(q + 22);
        unsigned w = get16(q + 24), h = get16(q + 26), plane = op == 63 ? get32(q + 28) : 0;
        if (!d->pixels || !to->pixels) { error(8, id, op); break; }
        int left = 0, top = 0, right = (int)w, bottom = (int)h;
        if (-dx > left) left = -dx;
        if (-dy > top) top = -dy;
        if ((int)to->width - dx < right) right = (int)to->width - dx;
        if ((int)to->height - dy < bottom) bottom = (int)to->height - dy;
        int source_left = left, source_top = top, source_right = right, source_bottom = bottom;
        if (-sx > source_left) source_left = -sx;
        if (-sy > source_top) source_top = -sy;
        if ((int)d->width - sx < source_right) source_right = (int)d->width - sx;
        if ((int)d->height - sy < source_bottom) source_bottom = (int)d->height - sy;
        if (source_left > right) source_left = right;
        if (source_right < source_left) source_right = source_left;
        if (source_top > bottom) source_top = bottom;
        if (source_bottom < source_top) source_bottom = source_top;
        int ystart = source_top, yend = source_bottom, ystep = 1;
        int xstart = source_left, xend = source_right, xstep = 1;
        if (to == d && dy > sy) { ystart = source_bottom - 1; yend = source_top - 1; ystep = -1; }
        if (to == d && dx > sx) { xstart = source_right - 1; xend = source_left - 1; xstep = -1; }
        for (int yy = ystart; yy != yend; yy += ystep) for (int xx = xstart; xx != xend; xx += xstep) {
            int px = sx + xx, py = sy + yy;
            uint32_t color = d->pixels[(unsigned)py * d->width + (unsigned)px];
            if (op == 63) color = color & plane ? g->foreground : g->background;
            pixel(to, g, dx + xx, dy + yy, color);
        }
        if (g->graphics_exposures) {
            struct rectangle missing[4]; unsigned count = 0;
            if (right > left && bottom > top) {
                if (source_top > top) missing[count++] = (struct rectangle){dx + left, dy + top, (unsigned)(right - left), (unsigned)(source_top - top)};
                if (source_bottom < bottom) missing[count++] = (struct rectangle){dx + left, dy + source_bottom, (unsigned)(right - left), (unsigned)(bottom - source_bottom)};
                if (source_bottom > source_top && source_left > left) missing[count++] = (struct rectangle){dx + left, dy + source_top, (unsigned)(source_left - left), (unsigned)(source_bottom - source_top)};
                if (source_bottom > source_top && source_right < right) missing[count++] = (struct rectangle){dx + source_right, dy + source_top, (unsigned)(right - source_right), (unsigned)(source_bottom - source_top)};
            }
            if (!count) {
                zero(r, 32); r[0] = 14; put16(r + 2, sequence); put32(r + 4, to->id); r[10] = (uint8_t)op; enqueue(r, 32);
            } else for (unsigned i = 0; i < count; i++) {
                zero(r, 32); r[0] = 13; put16(r + 2, sequence); put32(r + 4, to->id);
                put16(r + 8, (unsigned)missing[i].x); put16(r + 10, (unsigned)missing[i].y);
                put16(r + 12, missing[i].width); put16(r + 14, missing[i].height);
                put16(r + 18, count - i - 1); r[20] = (uint8_t)op; enqueue(r, 32);
            }
        }
        break;
    }
    case 64: case 65: { MINIMUM(12); DRAWABLE(); GC(8); if ((n - 12) % 4) { error(16, n / 4, op); break; }
        int x = 0, y = 0, previous_x = 0, previous_y = 0;
        for (unsigned off = 12; off < n; off += 4) {
            int xx = signed16(q + off), yy = signed16(q + off + 2);
            if (q[1] && off > 12) { x += xx; y += yy; } else { x = xx; y = yy; }
            if (op == 64) pixel(d, g, x, y, g->foreground); else if (off > 12) line(d, g, previous_x, previous_y, x, y);
            previous_x = x; previous_y = y;
        } break;
    }
    case 66: MINIMUM(12); DRAWABLE(); GC(8); if ((n - 12) % 8) { error(16, n / 4, op); break; }
        for (unsigned off = 12; off < n; off += 8) line(d, g, signed16(q + off), signed16(q + off + 2), signed16(q + off + 4), signed16(q + off + 6));
        break;
    case 67: case 70: MINIMUM(12); DRAWABLE(); GC(8); if ((n - 12) % 8) { error(16, n / 4, op); break; }
        for (unsigned off = 12; off < n; off += 8) {
            int x = signed16(q + off), y = signed16(q + off + 2); unsigned w = get16(q + off + 4), h = get16(q + off + 6);
            if (op == 70) fill(d, g, x, y, w, h, g->foreground);
            else { line(d, g, x, y, x + (int)w, y); line(d, g, x + (int)w, y, x + (int)w, y + (int)h);
                line(d, g, x + (int)w, y + (int)h, x, y + (int)h); line(d, g, x, y + (int)h, x, y); }
        } break;
    case 68: case 71: MINIMUM(12); DRAWABLE(); GC(8); if ((n - 12) % 12) { error(16, n / 4, op); break; }
        for (unsigned off = 12; off < n; off += 12) arc(d, g, q + off, op == 71);
        break;
    case 72: { MINIMUM(24); DRAWABLE(); GC(8); unsigned w = get16(q + 12), h = get16(q + 14), depth = q[21], left = q[20];
        int x = signed16(q + 16), y = signed16(q + 18); unsigned stride, planes = 1;
        if (q[1] > 2) { error(2, q[1], op); break; }
        if (q[1] == 2 && depth == 24 && d->depth == 24 && !left) stride = w * 4;
        else if (q[1] == 0 && depth == 1 && left < 32) stride = ((w + left + 31) / 32) * 4;
        else if (q[1] == 1 && depth == d->depth && (depth == 1 || depth == 24) && left < 32) {
            stride = ((w + left + 31) / 32) * 4;
            planes = depth;
        } else if (q[1] == 2 && depth == 1 && d->depth == 1 && !left) stride = ((w + 31) / 32) * 4;
        else { error(8, depth, op); break; }
        if (h && stride > (n - 24) / h / planes) { error(16, n / 4, op); break; }
        LENGTH(24 + stride * h * planes);
        for (unsigned yy = 0; yy < h; yy++) for (unsigned xx = 0; xx < w; xx++) {
            uint32_t color = 0;
            if (q[1] == 2 && depth == 24) color = get32(q + 24 + yy * stride + xx * 4);
            else {
                unsigned bit = xx + left;
                for (unsigned plane = 0; plane < planes; plane++) {
                    uint8_t byte = q[24 + plane * stride * h + yy * stride + bit / 8];
                    bool set = (byte & (1U << (little_endian ? bit % 8 : 7 - bit % 8))) != 0;
                    color = (color << 1) | set;
                }
                if (q[1] == 0) color = color ? g->foreground : g->background;
            }
            pixel(d, g, x + (int)xx, y + (int)yy, color);
        } break;
    }
    case 78: LENGTH(16); if (!valid_id(id)) error(14, id, op);
        else if (get32(q + 12) != VISUAL) error(8, get32(q + 12), op);
        else if (!add_resource(colormaps, id)) error(11, id, op);
        break;
    case 79: LENGTH(8); if (!remove_resource(colormaps, id) && id != COLORMAP) error(12, id, op); break;
    case 81: case 82: LENGTH(8); if (id != COLORMAP && !named_resource(colormaps, id)) error(12, id, op); break;
    case 83: LENGTH(8); WINDOW(); reply(r, 32, 0); put32(r + 4, 1); put16(r + 8, 1); enqueue(r, 32); put32(r, COLORMAP); enqueue(r, 4); break;
    case 84: LENGTH(16); if (id != COLORMAP && !named_resource(colormaps, id)) { error(12, id, op); break; }
        reply(r, 32, 0); put16(r + 8, get16(q + 8)); put16(r + 10, get16(q + 10)); put16(r + 12, get16(q + 12));
        put32(r + 16, ((uint32_t)(get16(q + 8) >> 8) << 16) | ((uint32_t)(get16(q + 10) >> 8) << 8) | (get16(q + 12) >> 8)); enqueue(r, 32); break;
    case 85: case 92: { MINIMUM(12); LENGTH(12 + padded(get16(q + 8))); if (id != COLORMAP && !named_resource(colormaps, id)) { error(12, id, op); break; }
        unsigned length = get16(q + 8); uint32_t color = 0; bool found = false;
        static const struct { const char *name; unsigned n; uint32_t rgb; } colors[] = {
            {"black",5,0}, {"white",5,0xffffff}, {"red",3,0xff0000}, {"green",5,0x00ff00},
            {"blue",4,0x0000ff}, {"yellow",6,0xffff00}, {"gray",4,0xbebebe}, {"grey",4,0xbebebe},
            {"cyan",4,0x00ffff}, {"magenta",7,0xff00ff}, {"XtDefaultForeground",19,0}, {"XtDefaultBackground",19,0xffffff}
        };
        for (unsigned i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) if (length == colors[i].n) {
            unsigned j = 0; while (j < length) {
                unsigned a = q[12 + j], b = (uint8_t)colors[i].name[j]; if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) break;
                j++;
            } if (j == length) { color = colors[i].rgb; found = true; break; }
        }
        if (!found) { error(15, 0, op); break; }
        reply(r, 32, 0); unsigned base = op == 85 ? 12 : 8; if (op == 85) put32(r + 8, color);
        for (unsigned i = 0; i < 3; i++) {
            unsigned v = ((color >> (16 - i * 8)) & 255) * 257;
            put16(r + base + i * 2, v); put16(r + base + 6 + i * 2, v);
        } enqueue(r, 32); break;
    }
    case 88: MINIMUM(12); break; /* TrueColor pixels have no allocated cells to free. */
    case 91: MINIMUM(8); if (id != COLORMAP && !named_resource(colormaps, id)) { error(12, id, op); break; }
        if ((n - 8) / 4 > (REPLY_RESERVE - 32) / 8) { error(11, n / 4, op); break; }
        reply(r, 32, 0); put32(r + 4, (n - 8) / 2); put16(r + 8, (n - 8) / 4); enqueue(r, 32);
        for (unsigned off = 8; off < n; off += 4) { uint32_t color = get32(q + off); zero(r, 8);
            put16(r, ((color >> 16) & 255) * 257); put16(r + 2, ((color >> 8) & 255) * 257); put16(r + 4, (color & 255) * 257); enqueue(r, 8); } break;
    case 97: LENGTH(12); DRAWABLE(); reply(r, 32, 0); put16(r + 8, get16(q + 8)); put16(r + 10, get16(q + 10)); enqueue(r, 32); break;
    case 98: MINIMUM(8); LENGTH(8 + padded(get16(q + 4))); reply(r, 32, 0); enqueue(r, 32); break;
    case 99: LENGTH(4); reply(r, 32, 0); enqueue(r, 32); break;
    case 101: LENGTH(8); reply(r, 32, 1); put32(r + 4, q[5]); enqueue(r, 32); zero(r, 4);
        for (unsigned i = 0; i < q[5]; i++) enqueue(r, 4);
        break;
    case 103: LENGTH(4); reply(r, 52, 0); put16(r + 12, 50); put16(r + 14, 400); put16(r + 16, 100); enqueue(r, 52); break;
    case 106: LENGTH(4); reply(r, 32, 0); put16(r + 8, 1); put16(r + 10, 1); put16(r + 12, 4); enqueue(r, 32); break;
    case 108: LENGTH(4); reply(r, 32, 0); enqueue(r, 32); break;
    case 110: LENGTH(4); reply(r, 32, 0); enqueue(r, 32); break;
    case 112: LENGTH(4); if (q[1] > 2) error(2, q[1], op); break;
    case 117: LENGTH(4); reply(r, 32, 3); put32(r + 4, 1); enqueue(r, 32); r[0] = 1; r[1] = 2; r[2] = 3; r[3] = 0; enqueue(r, 4); break;
    case 119: LENGTH(4); reply(r, 32, 0); enqueue(r, 32); break;
    case 127: break;
    default: error(1, 0, op); break;
    }
#undef LENGTH
#undef MINIMUM
#undef WINDOW
#undef DRAWABLE
#undef GC
}
static void process(void) {
    if (failed) return;
    unsigned consumed = 0;
    if (!initialized) {
        if (input_used < 12) return;
        if (input[0] != 'l' && input[0] != 'B') { failed = true; return; }
        little_endian = input[0] == 'l';
        unsigned length = 12 + padded(get16(input + 6)) + padded(get16(input + 8));
        if (input_used < length) return;
        if (get16(input + 2) != 11) { failed = true; return; }
        setup(); consumed = length;
    }
    while (input_used - consumed >= 4 && OUTPUT_SIZE - output_count >= REPLY_RESERVE) {
        unsigned length = (unsigned)get16(input + consumed + 2) * 4;
        if (length < 4) { sequence++; error(16, 0, input[consumed]); failed = true; break; }
        if (input_used - consumed < length) break;
        sequence++; dispatch(input + consumed, length); consumed += length;
    }
    if (consumed) {
        for (unsigned i = consumed; i < input_used; i++) input[i - consumed] = input[i];
        input_used -= consumed;
    }
}
void linux_x11_reset(void) {
    for (unsigned i = 0; i < OBJECTS; i++) if (drawables[i].pixels) pmm_free(drawables[i].pixels, drawables[i].pages);
    for (unsigned i = 0; i < PROPERTIES; i++) if (properties[i].data) pmm_free(properties[i].data, properties[i].pages);
    if (input) pmm_free(input, INPUT_SIZE / PMM_PAGE_SIZE);
    if (output) pmm_free(output, OUTPUT_SIZE / PMM_PAGE_SIZE);
    input = output = NULL; zero(drawables, sizeof(drawables)); zero(gcs, sizeof(gcs));
    zero(fonts, sizeof(fonts)); zero(colormaps, sizeof(colormaps));
    zero(properties, sizeof(properties)); zero(atoms, sizeof(atoms));
    zero(selection_atom, sizeof(selection_atom)); zero(selection_owner, sizeof(selection_owner));
    atom_count = input_used = output_head = output_count = sequence = 0;
    connected = initialized = failed = dirty = false; focus = ROOT;
}
int linux_x11_connect(const void *address, unsigned length) {
    const uint8_t *a = address;
    static const char path[] = "/tmp/.X11-unix/X0";
    if (length < 3 || a[0] != 1 || a[1] != 0) return -97;
    unsigned start = a[2] == 0 ? 3 : 2;
    unsigned path_length = sizeof(path) - 1;
    if (length < start + path_length) return -2;
    for (unsigned i = 0; i < path_length; i++) if (a[start + i] != (uint8_t)path[i]) return -2;
    if (length > start + path_length && a[start + path_length]) return -2;
    if (connected) return -111;
    if (!display_get_framebuffer_size(&screen_width, &screen_height)) return -111;
    input = pmm_alloc(INPUT_SIZE / PMM_PAGE_SIZE); output = pmm_alloc(OUTPUT_SIZE / PMM_PAGE_SIZE);
    if (!input || !output) { linux_x11_reset(); return -12; }
    drawables[0].id = ROOT; drawables[0].width = screen_width; drawables[0].height = screen_height;
    drawables[0].depth = 24; drawables[0].klass = 1; drawables[0].mapped = true; drawables[0].colormap = COLORMAP;
    static const char *const predefined[] = {
        "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL", "COLORMAP", "CURSOR", "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2", "CUT_BUFFER3", "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7", "DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT", "RECTANGLE", "RESOURCE_MANAGER", "RGB_COLOR_MAP", "RGB_BEST_MAP", "RGB_BLUE_MAP", "RGB_DEFAULT_MAP", "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING", "VISUALID", "WINDOW", "WM_COMMAND", "WM_HINTS", "WM_CLIENT_MACHINE", "WM_ICON_NAME", "WM_ICON_SIZE", "WM_NAME", "WM_NORMAL_HINTS", "WM_SIZE_HINTS", "WM_ZOOM_HINTS", "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE", "SUPERSCRIPT_X", "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y", "UNDERLINE_POSITION", "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT", "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT", "QUAD_WIDTH", "WEIGHT", "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE", "FONT_NAME", "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR"
    };
    for (unsigned i = 0; i < sizeof(predefined) / sizeof(predefined[0]); i++) {
        unsigned n = 0; while (predefined[i][n]) n++; intern((const uint8_t *)predefined[i], n, false);
    }
    connected = true; return 0;
}
long linux_x11_write(const void *data, unsigned long length) {
    if (!connected) return -107;
    if (failed) return -32;
    const uint8_t *bytes = data; unsigned long written = 0;
    while (written < length) {
        process();
        if (failed) break;
        unsigned room = INPUT_SIZE - input_used;
        if (!room) break;
        unsigned n = length - written > room ? room : (unsigned)(length - written);
        copy(input + input_used, bytes + written, n); input_used += n; written += n;
    }
    process();
    if (dirty) { linux_x11_refresh(); display_present(); }
    return written ? (long)written : failed ? -32 : length ? -11 : 0;
}
long linux_x11_read(void *data, unsigned long length) {
    if (!connected) return -107;
    if (!length) return 0;
    process();
    if (!output_count) return failed ? 0 : -11;
    unsigned n = length > output_count ? output_count : (unsigned)length;
    uint8_t *p = data;
    for (unsigned i = 0; i < n; i++) p[i] = output[(output_head + i) % OUTPUT_SIZE];
    output_head = (output_head + n) % OUTPUT_SIZE; output_count -= n;
    process(); return n;
}
unsigned long linux_x11_available(void) {
    if (!connected) return 0;
    process(); return output_count;
}
