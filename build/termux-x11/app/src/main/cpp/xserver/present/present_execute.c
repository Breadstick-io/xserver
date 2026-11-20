/*
 * Copyright © 2013 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "present_priv.h"

/*
 * Called when the wait fence is triggered; just gets the current msc/ust and
 * calls the proper execute again. That will re-check the fence and pend the
 * request again if it's still not actually ready
 */
static void
present_wait_fence_triggered(void *param)
{
    present_vblank_ptr      vblank = param;
    ScreenPtr               screen = vblank->screen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    screen_priv->re_execute(vblank);
}

/* Breadstick: lorie/InitOutput.c */
extern Bool lorieFrameNotReady(PixmapPtr pixmap, void (*cb)(void *param), void *param);

Bool
present_execute_wait(present_vblank_ptr vblank, uint64_t crtc_msc)
{
    WindowPtr                   window = vblank->window;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    /* We may have to requeue for the next MSC if check_flip_window prevented
     * using a flip.
     */
    if (vblank->exec_msc == crtc_msc + 1 &&
        screen_priv->queue_vblank(screen, window, vblank->crtc, vblank->event_id,
                                  vblank->exec_msc) == Success)
        return TRUE;

    if (vblank->wait_fence) {
        if (!present_fence_check_triggered(vblank->wait_fence)) {
            present_fence_set_callback(vblank->wait_fence, present_wait_fence_triggered, vblank);
            return TRUE;
        }
    }
    /* Breadstick (lorie/InitOutput.c): the GPU has not finished the frame in this pixmap yet. lorie
     * watches the GPU's fence, which came on the buffer's own socket, and runs the frame when it
     * signals, the way a wait fence would. */
    if (vblank->pixmap && lorieFrameNotReady(vblank->pixmap, present_wait_fence_triggered, vblank))
        return TRUE;
    return FALSE;
}

/* Breadstick: lorie/InitOutput.c */
extern Bool lorieWindowFlip(WindowPtr window, PixmapPtr pixmap, RegionPtr update, int16_t x_off, int16_t y_off,
                            CARD32 serial, Bool has_idle_fence);

void
present_execute_copy(present_vblank_ptr vblank, uint64_t crtc_msc)
{
    WindowPtr                   window = vblank->window;
    ScreenPtr                   screen = window->drawable.pScreen;
    present_screen_priv_ptr screen_priv = present_screen_priv(screen);

    /* If present_flip failed, we may have to requeue for the next MSC */
    if (vblank->exec_msc == crtc_msc + 1 &&
        Success == screen_priv->queue_vblank(screen,
                                             window,
                                             vblank->crtc,
                                             vblank->event_id,
                                             vblank->exec_msc)) {
        vblank->queued = TRUE;
        return;
    }

    /* Breadstick: a redirected top-level window can take the pixmap as its own instead of a copy
     * (lorie, rootless). lorie then sends the idle notification itself, when the pixmap is no longer
     * the window's and the app has had time to stop showing it. */
    if (lorieWindowFlip(window, vblank->pixmap, vblank->update, vblank->x_off, vblank->y_off,
                        vblank->serial, vblank->idle_fence != NULL))
        return;

    present_copy_region(&window->drawable, vblank->pixmap, vblank->update, vblank->x_off, vblank->y_off);

    /* present_copy_region sticks the region into a scratch GC,
     * which is then freed, freeing the region
     */
    vblank->update = NULL;
    screen_priv->flush(window);

    present_pixmap_idle(vblank->pixmap, vblank->window, vblank->serial, vblank->idle_fence);
}

void
present_execute_post(present_vblank_ptr vblank, uint64_t ust, uint64_t crtc_msc)
{
    uint8_t mode;

    /* Compute correct CompleteMode
     */
    if (vblank->kind == PresentCompleteKindPixmap) {
        if (vblank->pixmap && vblank->window) {
            if (vblank->has_suboptimal && vblank->reason == PRESENT_FLIP_REASON_BUFFER_FORMAT)
                mode = PresentCompleteModeSuboptimalCopy;
            else
                mode = PresentCompleteModeCopy;
        } else {
            mode = PresentCompleteModeSkip;
        }
    }
    else
        mode = PresentCompleteModeCopy;

    present_vblank_notify(vblank, vblank->kind, mode, ust, crtc_msc);
    present_vblank_destroy(vblank);
}
