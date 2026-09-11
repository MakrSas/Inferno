/*
 * Guest display and input for an in-process embedder. See
 * include/ui/inferno-embed.h for why this exists.
 *
 * Copyright (c) 2026 Inferno iOS port.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "ui/console.h"
#include "ui/inferno-embed.h"
#include "ui/input.h"
#include "ui/surface.h"

/*
 * The app reads frames from its own thread while QEMU redraws them on the main
 * loop, so everything below is behind one mutex. It is deliberately not the big
 * lock: a read must not stop the vCPUs, and taking the big lock here while the
 * main loop waits for this one would be a deadlock waiting to happen. The
 * listener callbacks already run under the big lock, which is why they may take
 * this one, and the reader never takes any lock but this.
 */
typedef struct InfernoDisplay
{
    QemuMutex       lock;
    bool            attached;
    DisplaySurface* surface;
    uint32_t        generation;
    /* The union of everything redrawn since the app last looked. */
    bool     dirty;
    uint32_t x0, y0, x1, y1;
    /* Mirrors the emulated panel so button changes can be sent only once. */
    uint32_t buttons;
} InfernoDisplay;

static InfernoDisplay             inferno_display;
static DisplayChangeListener      inferno_dcl;

/* Counted under the display's own lock; see InfernoDisplayStats. */
static uint64_t inferno_presents;
static uint64_t inferno_refreshes;

static void damage_all_locked(InfernoDisplay* d)
{
    if (d->surface == NULL) {
        d->dirty = false;
        return;
    }
    d->dirty = true;
    d->x0    = 0;
    d->y0    = 0;
    d->x1    = surface_width(d->surface);
    d->y1    = surface_height(d->surface);
}

static void inferno_gfx_switch(DisplayChangeListener* dcl, DisplaySurface* surface)
{
    InfernoDisplay* d = &inferno_display;

    QEMU_LOCK_GUARD(&d->lock);
    d->surface = surface;
    d->generation++;
    damage_all_locked(d);
}

static void inferno_gfx_update(DisplayChangeListener* dcl, int x, int y, int w, int h)
{
    InfernoDisplay* d = &inferno_display;

    if (w <= 0 || h <= 0) { return; }

    QEMU_LOCK_GUARD(&d->lock);
    if (d->surface == NULL) { return; }

    if (!d->dirty) {
        d->dirty = true;
        d->x0    = x;
        d->y0    = y;
        d->x1    = x + w;
        d->y1    = y + h;
        return;
    }
    d->x0 = MIN(d->x0, (uint32_t)x);
    d->y0 = MIN(d->y0, (uint32_t)y);
    d->x1 = MAX(d->x1, (uint32_t)(x + w));
    d->y1 = MAX(d->y1, (uint32_t)(y + h));
}

/*
 * Nothing here draws; this is what drives the machine's own redraw, exactly as
 * a window or a VNC client would.
 */
static void inferno_refresh(DisplayChangeListener* dcl)
{
    InfernoDisplay* d = &inferno_display;

    WITH_QEMU_LOCK_GUARD(&d->lock) { inferno_refreshes++; }
    graphic_hw_update(dcl->con);
}

void inferno_display_note_present(void)
{
    InfernoDisplay* d = &inferno_display;

    if (!d->attached) { return; }
    WITH_QEMU_LOCK_GUARD(&d->lock) { inferno_presents++; }
}

void inferno_display_stats(InfernoDisplayStats* out)
{
    InfernoDisplay* d = &inferno_display;

    if (out == NULL) { return; }
    memset(out, 0, sizeof(*out));
    if (!d->attached) { return; }

    QEMU_LOCK_GUARD(&d->lock);
    out->presents     = inferno_presents;
    out->refreshes    = inferno_refreshes;
    inferno_presents  = 0;
    inferno_refreshes = 0;
}

static const DisplayChangeListenerOps inferno_dcl_ops = {
    .dpy_name       = "inferno-embed",
    .dpy_refresh    = inferno_refresh,
    .dpy_gfx_update = inferno_gfx_update,
    .dpy_gfx_switch = inferno_gfx_switch,
};

void inferno_display_attach(void)
{
    InfernoDisplay* d = &inferno_display;

    if (d->attached) { return; }

    qemu_mutex_init(&d->lock);
    d->attached = true;

    inferno_dcl.ops = &inferno_dcl_ops;
    inferno_dcl.con = qemu_console_lookup_by_index(0);
    register_displaychangelistener(&inferno_dcl);

    /*
     * The console already has a surface by now — the machine drew its boot
     * splash into it long before the app got here — so pick it up rather than
     * waiting for a switch that may never come.
     */
    WITH_QEMU_LOCK_GUARD(&d->lock)
    {
        if (inferno_dcl.con != NULL) { d->surface = qemu_console_surface(inferno_dcl.con); }
        damage_all_locked(d);
    }
}

void inferno_display_detach(void)
{
    InfernoDisplay* d = &inferno_display;

    if (!d->attached) { return; }
    unregister_displaychangelistener(&inferno_dcl);
    WITH_QEMU_LOCK_GUARD(&d->lock)
    {
        d->surface = NULL;
        d->dirty   = false;
    }
    d->attached = false;
}

void inferno_display_invalidate(void)
{
    InfernoDisplay* d = &inferno_display;

    if (!d->attached) { return; }
    QEMU_LOCK_GUARD(&d->lock);
    damage_all_locked(d);
}

InfernoFrameResult inferno_display_read(void* dst, size_t dst_size, InfernoFrameInfo* info)
{
    InfernoDisplay* d = &inferno_display;
    uint32_t        width, height, x0, y0, x1, y1, row;
    const uint8_t*  src;
    uint8_t*        out;
    int             src_stride;
    size_t          need, span;

    if (info == NULL) { return INFERNO_FRAME_NONE; }
    memset(info, 0, sizeof(*info));
    if (!d->attached) { return INFERNO_FRAME_NONE; }

    QEMU_LOCK_GUARD(&d->lock);
    if (d->surface == NULL) { return INFERNO_FRAME_NONE; }

    width  = surface_width(d->surface);
    height = surface_height(d->surface);

    info->width      = width;
    info->height     = height;
    info->stride     = width * 4;
    info->generation = d->generation;

    need = (size_t)width * height * 4;
    if (dst == NULL || dst_size < need) {
        /* Report the whole screen: the caller is about to start from nothing. */
        info->w = width;
        info->h = height;
        damage_all_locked(d);
        return INFERNO_FRAME_RESIZE;
    }
    if (!d->dirty) { return INFERNO_FRAME_NONE; }

    x0 = MIN(d->x0, width);
    y0 = MIN(d->y0, height);
    x1 = MIN(d->x1, width);
    y1 = MIN(d->y1, height);
    if (x0 >= x1 || y0 >= y1) {
        d->dirty = false;
        return INFERNO_FRAME_NONE;
    }

    src        = surface_data(d->surface);
    src_stride = surface_stride(d->surface);
    out        = dst;
    span       = (size_t)(x1 - x0) * 4;

    for (row = y0; row < y1; row++) {
        memcpy(out + (size_t)row * info->stride + (size_t)x0 * 4, src + (size_t)row * src_stride + (size_t)x0 * 4,
               span);
    }

    info->x  = x0;
    info->y  = y0;
    info->w  = x1 - x0;
    info->h  = y1 - y0;
    d->dirty = false;
    return INFERNO_FRAME_OK;
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

void inferno_input_touch(int32_t x, int32_t y, bool pressed)
{
    InfernoDisplay* d = &inferno_display;
    QemuConsole*    con;
    uint32_t        width = 0, height = 0;
    bool            was = false;

    if (!d->attached) { return; }

    WITH_QEMU_LOCK_GUARD(&d->lock)
    {
        if (d->surface == NULL) { return; }
        width  = surface_width(d->surface);
        height = surface_height(d->surface);
        was    = d->buttons != 0;
        d->buttons = pressed ? 1 : 0;
    }
    if (width == 0 || height == 0) { return; }

    x = MAX(0, MIN((int32_t)width - 1, x));
    y = MAX(0, MIN((int32_t)height - 1, y));

    con = inferno_dcl.con;
    bql_lock();
    qemu_input_queue_abs(con, INPUT_AXIS_X, x, 0, width);
    qemu_input_queue_abs(con, INPUT_AXIS_Y, y, 0, height);
    /*
     * Only when it actually changes: the panel tells a fresh touch from a
     * finger being dragged by whether the button state moved, and repeating
     * the press would restart the contact on every move.
     */
    if (was != pressed) { qemu_input_queue_btn(con, INPUT_BUTTON_LEFT, pressed); }
    qemu_input_event_sync();
    bql_unlock();
}

void inferno_input_function_key(uint32_t number, bool pressed)
{
    QKeyCode code;

    if (number < 1 || number > 12) { return; }
    code = Q_KEY_CODE_F1 + (number - 1);

    bql_lock();
    qemu_input_event_send_key_qcode(NULL, code, pressed);
    bql_unlock();
}
