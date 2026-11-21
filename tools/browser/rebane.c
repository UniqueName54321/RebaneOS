/* SPDX-License-Identifier: MIT
 * RebaneOS libnsfb surface. Pixels are rendered by NetSurf's software plotters;
 * the kernel device merely copies them to its native graphical window.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "libnsfb.h"
#include "libnsfb_plot.h"
#include "libnsfb_event.h"
#include "nsfb.h"
#include "surface.h"
#include "plot.h"
#include "cursor.h"

#define REBANE_FB_INFO 0x524201
#define REBANE_FB_PRESENT 0x524202

struct rebane_surface {
    int fb, input;
    unsigned pitch, buttons, modifiers;
    nsfb_event_t queued[8];
    unsigned next, count;
};

static int rebane_geometry(nsfb_t *nsfb, int w, int h,
                           enum nsfb_format_e format)
{
    if (nsfb->ptr != NULL)
        return (w == nsfb->width && h == nsfb->height) ? 0 : -1;
    if (w > 0) nsfb->width = w;
    if (h > 0) nsfb->height = h;
    (void)format;
    nsfb->format = NSFB_FMT_XRGB8888;
    return select_plotters(nsfb) ? 0 : -1;
}

static int rebane_finalise(nsfb_t *nsfb)
{
    struct rebane_surface *s = nsfb->surface_priv;
    if (s != NULL) {
        if (s->fb >= 0) close(s->fb);
        if (s->input >= 0) close(s->input);
        free(s);
    }
    free(nsfb->ptr);
    nsfb->ptr = NULL;
    nsfb->surface_priv = NULL;
    return 0;
}

static int rebane_initialise(nsfb_t *nsfb)
{
    struct rebane_surface *s = calloc(1, sizeof(*s));
    uint32_t info[4];
    if (s == NULL) return -1;
    s->fb = s->input = -1;
    nsfb->surface_priv = s;
    s->fb = open("/dev/rebane-fb", O_RDWR);
    s->input = open("/dev/rebane-input", O_RDONLY | O_NONBLOCK);
    if (s->fb < 0 || s->input < 0 ||
        ioctl(s->fb, REBANE_FB_INFO, info) < 0 || info[3] != 1 ||
        info[0] == 0 || info[1] == 0 || info[0] > 8192 ||
        info[1] > 8192 || info[2] < info[0] * 4) goto fail;
    nsfb->width = info[0];
    nsfb->height = info[1];
    nsfb->format = NSFB_FMT_XRGB8888;
    if (!select_plotters(nsfb)) goto fail;
    nsfb->linelen = nsfb->width * 4;
    s->pitch = info[2];
    nsfb->ptr = calloc(nsfb->height, nsfb->linelen);
    if (nsfb->ptr == NULL) goto fail;
    return 0;
fail:
    rebane_finalise(nsfb);
    return -1;
}

static int rebane_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct rebane_surface *s = nsfb->surface_priv;
    nsfb_bbox_t b = {0, 0, nsfb->width, nsfb->height};
    int y;
    if (box != NULL) b = *box;
    if (nsfb->cursor != NULL && !nsfb->cursor->plotted) {
        nsfb_bbox_t c;
        if (!nsfb_cursor_plot(nsfb, nsfb->cursor)) return -1;
        c = nsfb->cursor->savloc;
        if (c.x0 < b.x0) b.x0 = c.x0;
        if (c.y0 < b.y0) b.y0 = c.y0;
        if (c.x1 > b.x1) b.x1 = c.x1;
        if (c.y1 > b.y1) b.y1 = c.y1;
    }
    if (b.x0 < 0) b.x0 = 0;
    if (b.y0 < 0) b.y0 = 0;
    if (b.x1 > nsfb->width) b.x1 = nsfb->width;
    if (b.y1 > nsfb->height) b.y1 = nsfb->height;
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return 0;
    for (y = b.y0; y < b.y1; ++y) {
        const uint8_t *p = nsfb->ptr + y * nsfb->linelen + b.x0 * 4;
        size_t left = (b.x1 - b.x0) * 4;
        off_t pos = (off_t)y * s->pitch + b.x0 * 4;
        while (left != 0) {
            ssize_t n = pwrite(s->fb, p, left, pos);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return -1;
            left -= n; p += n; pos += n;
        }
    }
    return ioctl(s->fb, REBANE_FB_PRESENT, 0);
}

static int rebane_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
    struct nsfb_cursor_s *c = nsfb->cursor;
    if (c != NULL && c->plotted &&
        (box == NULL || (box->x0 < c->savloc.x1 &&
         box->x1 > c->savloc.x0 && box->y0 < c->savloc.y1 &&
         box->y1 > c->savloc.y0)))
        nsfb_cursor_clear(nsfb, c);
    return 0;
}

static int rebane_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
    nsfb_bbox_t b = cursor->savloc;
    nsfb_bbox_t c;
    bool was_plotted = cursor->plotted;
    if (was_plotted) nsfb_cursor_clear(nsfb, cursor);
    if (!nsfb_cursor_plot(nsfb, cursor)) return -1;
    c = cursor->savloc;
    if (!was_plotted) b = c;
    if (c.x0 < b.x0) b.x0 = c.x0;
    if (c.y0 < b.y0) b.y0 = c.y0;
    if (c.x1 > b.x1) b.x1 = c.x1;
    if (c.y1 > b.y1) b.y1 = c.y1;
    return rebane_update(nsfb, &b);
}

static enum nsfb_key_code_e rebane_key(uint32_t code)
{
    if (code >= 'A' && code <= 'Z') return (enum nsfb_key_code_e)(code + 32);
    if (code == '%') return NSFB_KEY_5;
    if (code == '^') return NSFB_KEY_6;
    if (code == '{') return NSFB_KEY_LEFTBRACKET;
    if (code == '|') return NSFB_KEY_BACKSLASH;
    if (code == '}') return NSFB_KEY_RIGHTBRACKET;
    if (code < 0x10000) return (enum nsfb_key_code_e)code;
    switch (code & 0xffff) {
    case 42: return NSFB_KEY_LSHIFT;
    case 54: return NSFB_KEY_RSHIFT;
    case 29: return NSFB_KEY_LCTRL;
    case 97: return NSFB_KEY_RCTRL;
    case 56: return NSFB_KEY_LALT;
    case 100: return NSFB_KEY_RALT;
    case 102: return NSFB_KEY_HOME;
    case 103: return NSFB_KEY_UP;
    case 104: return NSFB_KEY_PAGEUP;
    case 105: return NSFB_KEY_LEFT;
    case 106: return NSFB_KEY_RIGHT;
    case 107: return NSFB_KEY_END;
    case 108: return NSFB_KEY_DOWN;
    case 109: return NSFB_KEY_PAGEDOWN;
    case 110: return NSFB_KEY_INSERT;
    case 111: return NSFB_KEY_DELETE;
    default: return NSFB_KEY_UNKNOWN;
    }
}

static bool rebane_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
    struct rebane_surface *s = nsfb->surface_priv;
    struct { uint32_t type, code; int32_t x, y; } input;
    struct pollfd fd = {s->input, POLLIN, 0};
    unsigned i;
    if (s->next != s->count) {
        *event = s->queued[s->next++];
        return true;
    }
    s->next = s->count = 0;
    if (poll(&fd, 1, timeout) <= 0) {
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_TIMEOUT;
        return false;
    }
    if (read(s->input, &input, sizeof(input)) != sizeof(input)) return false;
    if (input.type == 1) {
        unsigned desired = input.y & 6;
        enum nsfb_key_code_e key = rebane_key(input.code);
        static const enum nsfb_key_code_e modifiers[3] = {
            NSFB_KEY_LSHIFT, NSFB_KEY_LCTRL, NSFB_KEY_LALT
        };
        if ((input.code >= 'A' && input.code <= 'Z') ||
            input.code == '%' || input.code == '^' || input.code == '{' ||
            input.code == '|' || input.code == '}') desired |= 1;
        for (i = 0; i < 3; ++i) {
            unsigned mask = 1u << i;
            if ((s->modifiers ^ desired) & mask) {
                nsfb_event_t *q = &s->queued[s->count++];
                q->type = desired & mask ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
                q->value.keycode = modifiers[i];
            }
        }
        s->modifiers = desired;
        if (key != NSFB_KEY_LSHIFT && key != NSFB_KEY_RSHIFT &&
            key != NSFB_KEY_LCTRL && key != NSFB_KEY_RCTRL &&
            key != NSFB_KEY_LALT && key != NSFB_KEY_RALT &&
            key != NSFB_KEY_UNKNOWN) {
            nsfb_event_t *q = &s->queued[s->count++];
            q->type = input.x ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
            q->value.keycode = key;
        }
        if (s->count == 0) return false;
        *event = s->queued[s->next++];
        return true;
    }
    if (input.type != 2) return false;
    /* Deliver position before button transitions, so clicks hit the right DOM node. */
    event->type = NSFB_EVENT_MOVE_ABSOLUTE;
    event->value.vector.x = input.x;
    event->value.vector.y = input.y;
    event->value.vector.z = 0;
    for (i = 0; i < 3; ++i) {
        unsigned mask = 1u << i;
        static const enum nsfb_key_code_e keys[3] = {
            NSFB_KEY_MOUSE_1, NSFB_KEY_MOUSE_3, NSFB_KEY_MOUSE_2
        };
        if ((s->buttons ^ input.code) & mask) {
            nsfb_event_t *q = &s->queued[s->count++];
            q->type = input.code & mask ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
            q->value.keycode = keys[i];
        }
    }
    s->buttons = input.code;
    return true;
}

static const nsfb_surface_rtns_t rebane_rtns = {
    .initialise = rebane_initialise,
    .finalise = rebane_finalise,
    .geometry = rebane_geometry,
    .input = rebane_input,
    .update = rebane_update,
    .claim = rebane_claim,
    .cursor = rebane_cursor,
};

NSFB_SURFACE_DEF(rebane, NSFB_SURFACE_LINUX, &rebane_rtns)
