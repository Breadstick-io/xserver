#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma clang diagnostic ignored "-Wstrict-prototypes"
#pragma ide diagnostic ignored "cppcoreguidelines-narrowing-conversions"
#pragma ide diagnostic ignored "cert-err34-c"
#pragma ide diagnostic ignored "ConstantConditionsOC"
#pragma ide diagnostic ignored "ConstantFunctionResult"
#pragma ide diagnostic ignored "bugprone-integer-division"
#pragma clang diagnostic ignored "-Wmissing-noreturn"
#pragma clang diagnostic ignored "-Wformat-nonliteral"

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <sys/eventfd.h>
#include <sys/errno.h>
#include <libxcvt/libxcvt.h>
#include <X11/X.h>
#include <X11/Xmd.h>
#include <sys/wait.h>
#include <present.h>
#include <sys/mman.h>
#include <dri3.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include "fb.h"
#include "mipointer.h"
#include "micmap.h"
#include "miline.h"
#include "shmint.h"
#include "present_priv.h"
#include "misyncshm.h"
#include "glxserver.h"
#include "glxutil.h"
#include "fbconfigs.h"
#include "inpututils.h"
#include "exa.h"
#include "drm_fourcc.h"

#include <X11/Xatom.h>
#include "propertyst.h"
#include "client.h"
#include "damage.h"
#include "dixstruct.h"
#include "lorie.h"

#define DRM_FORMAT_MOD_LINEAR 0

extern void android_shmem_sysv_shm_force(uint8_t enable);

#define unused __attribute__((unused))
#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "LorieNative", __VA_ARGS__)

extern DeviceIntPtr lorieMouse, lorieKeyboard;

#define CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED 5

struct vblank {
    struct xorg_list link;
    uint64_t id, msc;
};

static struct present_screen_info loriePresentInfo;
static dri3_screen_info_rec lorieDri3Info;
static ExaDriverRec lorieExa;

typedef struct {
    DamagePtr damage;
    OsTimerPtr fpsTimer;

    SetWindowPixmapProcPtr SetWindowPixmap;
    CloseScreenProcPtr CloseScreen;

    int eventFd, stateFd;

    struct lorie_shared_server_state* state;
    struct {
        Bool legacyDrawing;
        uint32_t width, height;
        char name[1024];
        uint32_t framerate;
    } root;

    Bool dri3;

    uint64_t vblank_interval;
    struct xorg_list vblank_queue;
    uint64_t current_msc;
} lorieScreenInfo;

ScreenPtr pScreenPtr;
static lorieScreenInfo lorieScreen = {
        .stateFd = -1,
        .root.width = 1280,
        .root.height = 1024,
        .root.framerate = 30,
        .root.name = "screen",
        .dri3 = TRUE,
        .vblank_queue = { &lorieScreen.vblank_queue, &lorieScreen.vblank_queue },
}, *pvfb = &lorieScreen;
static char *xstartup = NULL;

typedef struct {
    LorieBuffer *buffer;
    bool flipped, wasLocked, imported;
    void *locked;
    void *mem;
} LoriePixmapPriv;

#define LORIE_PIXMAP_PRIV_FROM_PIXMAP(pixmap) (pixmap ? ((LoriePixmapPriv*) exaGetPixmapDriverPrivate(pixmap)) : NULL)
#define LORIE_BUFFER_FROM_PIXMAP(pixmap) (pixmap ? ((LoriePixmapPriv*) exaGetPixmapDriverPrivate(pixmap))->buffer : NULL)

void OsVendorInit(void) {
    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t cond_attr;

    if (lorieScreen.stateFd != -1) // already initialized
        return;

    if (-1 == (lorieScreen.stateFd = LorieBuffer_createRegion("xserver", sizeof(*lorieScreen.state)))) {
        dprintf(2, "FATAL: Failed to allocate server state.\n");
        _exit(1);
    }

    if (!(lorieScreen.state = mmap(NULL, sizeof(*lorieScreen.state), PROT_READ|PROT_WRITE, MAP_SHARED, lorieScreen.stateFd, 0))) {
        dprintf(2, "FATAL: Failed to map server state.\n");
        _exit(1);
    }

    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&lorieScreen.state->lock, &mutex_attr);
    pthread_mutex_init(&lorieScreen.state->cursor.lock, &mutex_attr);

    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&lorieScreen.state->cond, &cond_attr);
}

void lorieActivityConnected(void) {
    pvfb->state->drawRequested = pvfb->state->cursor.updated = true;
    lorieSendSharedServerState(pvfb->stateFd);
    lorieRegisterBuffer(LORIE_BUFFER_FROM_PIXMAP(pScreenPtr->devPrivate));
}

static LoriePixmapPriv* lorieRootWindowPixmapPriv(void) {
    void* devPriv = pScreenPtr ? pScreenPtr->devPrivate : NULL;
    return devPriv ? exaGetPixmapDriverPrivate(devPriv) : NULL;
}

static Bool TrueNoop() { return TRUE; }
static Bool FalseNoop() { return FALSE; }
static void VoidNoop() {}

void ddxGiveUp(unused enum ExitCode error) {
    log(ERROR, "Server stopped (%d)", error);
    CloseWellKnownConnections();
    UnlockServer();
    exit(error);
}

static void* ddxReadyThread(unused void* cookie) {
    if (xstartup && serverGeneration == 1) {
        pid_t pid = fork();

        if (!pid) {
            char DISPLAY[16] = "";
            sprintf(DISPLAY, ":%s", display);
            setenv("DISPLAY", DISPLAY, 1);

#define INHERIT_VAR(v) char *v = getenv("XSTARTUP_" #v); if (v && strlen(v)) setenv(#v, v, 1); unsetenv("XSTARTUP_" #v);
            INHERIT_VAR(CLASSPATH)
            INHERIT_VAR(LD_LIBRARY_PATH)
            INHERIT_VAR(LD_PRELOAD)
#undef INHERIT_VAR

            execlp(xstartup, xstartup, NULL);
            execlp("sh", "sh", "-c", xstartup, NULL);
            dprintf(2, "Failed to start command `sh -c \"%s\"`: %s\n", xstartup, strerror(errno));
            abort();
        } else {
            int status;
            do {
                pid_t w = waitpid(pid, &status, 0);
                if (w == -1) {
                    perror("waitpid");
                    GiveUp(SIGKILL);
                }

                if (WIFEXITED(status)) {
                    printf("%d exited, status=%d\n", w, WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    printf("%d killed by signal %d\n", w, WTERMSIG(status));
                } else if (WIFSTOPPED(status)) {
                    printf("%d stopped by signal %d\n", w, WSTOPSIG(status));
                } else if (WIFCONTINUED(status)) {
                    printf("%d continued\n", w);
                }
            } while (!WIFEXITED(status) && !WIFSIGNALED(status));
            GiveUp(SIGINT);
        }
    }

    return NULL;
}

void drawSquare(int x, int y, int l, uint32_t color, uint32_t stride, uint32_t* pixels) {
    for (int i=0; i<l; i++) for (int j=0; j<l; j++)
        pixels[(j+y)*stride + x + i] = color;
}

Bool drawSquares() {
    LoriePixmapPriv* priv = lorieRootWindowPixmapPriv();
    uint32_t* pixels = !priv ? NULL : priv->locked;
    if (pixels) {
        const LorieBuffer_Desc *d = LorieBuffer_description(priv->buffer);
        int l = min(d->width, d->height) / 4, x = (d->width - l)/2, y = (d->height - l)/2;

        drawSquare(x - l/3, y - l/3, l, 0x00FF0000, d->stride, pixels);
        drawSquare(x, y, l, 0x0000FF00, d->stride, pixels);
        drawSquare(x + l/3, y + l/3, l, 0x000000FF, d->stride, pixels);
    }

    return FALSE;
}

void ddxReady(void) {
    CursorVisible = TRUE;
    pScreenPtr->DisplayCursor(lorieMouse, pScreenPtr, rootCursor);
    if (NoListenAll)
        return;
    if (xstartup && !strlen(xstartup)) // allow overriding $TERMUX_X11_XSTARTUP with empty xstartup arg
        return;
    if (!xstartup || !strlen(xstartup))
        xstartup = getenv("TERMUX_X11_XSTARTUP");
    if (!xstartup || !strlen(xstartup))
        return;

    pthread_t t;
    pthread_create(&t, NULL, ddxReadyThread, NULL);
}

void OsVendorFatalError(unused const char *f, unused va_list args) {
    log(ERROR, f, args);
}

#if defined(DDXBEFORERESET)
void ddxBeforeReset(void) {}
#endif

#if INPUTTHREAD
/** This function is called in Xserver/os/inputthread.c when starting
    the input thread. */
void ddxInputThreadInit(void) {}
#endif

void ddxUseMsg(void) {
    ErrorF("-xstartup \"command\"    start `command` after server startup\n");
    ErrorF("-legacy-drawing        use legacy drawing, without using AHardwareBuffers\n");
    ErrorF("-force-bgra            force flipping colours (RGBA->BGRA)\n");
    ErrorF("-disable-dri3          disabling DRI3 support (to let lavapipe work)\n");
    ErrorF("-force-sysvshm         force using SysV shm syscalls\n");
    ErrorF("-check-drawing         run server only able to draw some test image (for testing if rendering root window works or not),\n");
}

int ddxProcessArgument(unused int argc, unused char *argv[], unused int i) {
    if (strcmp(argv[i], "-xstartup") == 0) {  /* -xstartup "command" */
        CHECK_FOR_REQUIRED_ARGUMENTS(1);
        xstartup = argv[++i];
        return 2;
    }

    if (strcmp(argv[i], "-legacy-drawing") == 0) {
        pvfb->root.legacyDrawing = TRUE;
        return 1;
    }

    if (strcmp(argv[i], "-force-bgra") == 0)
        return 1;

    if (strcmp(argv[i], "-disable-dri3") == 0) {
        pvfb->dri3 = FALSE;
        return 1;
    }

    if (strcmp(argv[i], "-force-sysvshm") == 0) {
        android_shmem_sysv_shm_force(1);
        return 1;
    }

    if (strcmp(argv[i], "-check-drawing") == 0) {
        NoListenAll = TRUE;
        QueueWorkProc(drawSquares, NULL, NULL);
        return 1;
    }

    return 0;
}

static RRModePtr lorieCvt(int width, int height, int framerate) {
    struct libxcvt_mode_info *info;
    char name[128];
    xRRModeInfo modeinfo = {0};
    RRModePtr mode;

    info = libxcvt_gen_mode_info(width, height, framerate, 0, 0);

    snprintf(name, sizeof name, "%dx%d", info->hdisplay, info->vdisplay);
    modeinfo.nameLength = strlen(name);
    modeinfo.width      = info->hdisplay;
    modeinfo.height     = info->vdisplay;
    modeinfo.dotClock   = info->dot_clock * 1000.0;
    modeinfo.hSyncStart = info->hsync_start;
    modeinfo.hSyncEnd   = info->hsync_end;
    modeinfo.hTotal     = info->htotal;
    modeinfo.vSyncStart = info->vsync_start;
    modeinfo.vSyncEnd   = info->vsync_end;
    modeinfo.vTotal     = info->vtotal;
    modeinfo.modeFlags  = info->mode_flags;

    mode = RRModeGet(&modeinfo, name);
    free(info);
    return mode;
}

static void lorieMoveCursor(unused DeviceIntPtr pDev, unused ScreenPtr pScr, int x, int y) {
    pvfb->state->cursor.x = x;
    pvfb->state->cursor.y = y;
    pvfb->state->cursor.moved = TRUE;
    // No need to explicitly lock the mutex, it will cause waiting for rendering to be finished.
    // We are simply signaling the renderer in the case if it sleeps.
    pthread_cond_signal(&pvfb->state->cond);
}

static void lorieConvertCursor(CursorPtr pCurs, uint32_t *data) {
    CursorBitsPtr bits = pCurs->bits;
    if (bits->argb) {
        for (int i = 0; i < bits->width * bits->height; i++) {
            /* Convert bgra to rgba */
            CARD32 p = bits->argb[i];
            data[i] = (p & 0xFF000000) | ((p & 0x00FF0000) >> 16) | (p & 0x0000FF00) | ((p & 0x000000FF) << 16);
        }
    } else {
        uint32_t d, fg, bg, *p;
        int x, y, stride, i, bit;

        p = data;
        fg = ((pCurs->foreBlue & 0xff00) << 8) | (pCurs->foreGreen & 0xff00) | (pCurs->foreRed >> 8);
        bg = ((pCurs->backBlue & 0xff00) << 8) | (pCurs->backGreen & 0xff00) | (pCurs->backRed >> 8);
        stride = BitmapBytePad(bits->width);
        for (y = 0; y < bits->height; y++)
            for (x = 0; x < bits->width; x++) {
                i = y * stride + x / 8;
                bit = 1 << (x & 7);
                d = (bits->source[i] & bit) ? fg : bg;
                d = (bits->mask[i] & bit) ? d | 0xff000000 : 0x00000000;
                *p++ = d;
            }
    }
}

static void lorieSetCursor(unused DeviceIntPtr pDev, unused ScreenPtr pScr, CursorPtr pCurs, int x0, int y0) {
    CursorBitsPtr bits = pCurs ? pCurs->bits : NULL;
    if (pCurs && (pCurs->bits->width >= 512 || pCurs->bits->height >= 512))
        // We do not have enough memory allocated for such a big cursor, let's display default "X" cursor
        pCurs = rootCursor;

    lorie_mutex_lock(&pvfb->state->cursor.lock, &pvfb->state->cursor.lockingPid);
    if (pCurs && bits) {
        pvfb->state->cursor.xhot = bits->xhot;
        pvfb->state->cursor.yhot = bits->yhot;
        pvfb->state->cursor.width = bits->width;
        pvfb->state->cursor.height = bits->height;
        lorieConvertCursor(pCurs, pvfb->state->cursor.bits);
    } else {
        pvfb->state->cursor.xhot = pvfb->state->cursor.yhot = 0;
        pvfb->state->cursor.width = pvfb->state->cursor.height = 0;
    }
    pvfb->state->cursor.updated = true;
    lorie_mutex_unlock(&pvfb->state->cursor.lock, &pvfb->state->cursor.lockingPid);

    lorieMoveCursor(NULL, NULL, x0, y0);
}

static miPointerSpriteFuncRec loriePointerSpriteFuncs = {
    .RealizeCursor = TrueNoop,
    .UnrealizeCursor = TrueNoop,
    .SetCursor = lorieSetCursor,
    .MoveCursor = lorieMoveCursor,
    .DeviceCursorInitialize = TrueNoop,
    .DeviceCursorCleanup = VoidNoop
};

static miPointerScreenFuncRec loriePointerCursorFuncs = {
    .CursorOffScreen = FalseNoop,
    .CrossScreen = VoidNoop,
    .WarpCursor = miPointerWarpCursor
};

static void loriePerformVblanks(void);
static void lorieFlushWindowDamage(void);
static void lorieSetupWmHints(void);

static Bool lorieRedraw(__unused ClientPtr pClient, __unused void *closure) {
    int status, nonEmpty;
    LoriePixmapPriv* priv;
    PixmapPtr root = pScreenPtr && pScreenPtr->root ? pScreenPtr->GetWindowPixmap(pScreenPtr->root) : NULL;

    pvfb->current_msc++;
    loriePerformVblanks();

    pvfb->state->waitForNextFrame = false;

    if (!lorieConnectionAlive())
        return TRUE;

    // Rootless windows are drawn by the app, not by this surface, so their damage goes out whether
    // or not the root's surface exists: in Floating and Enhanced mode it is often not on screen.
    lorieSetupWmHints();
    lorieFlushWindowDamage();

    if (!pvfb->state->surfaceAvailable)
        return TRUE;

    nonEmpty = RegionNotEmpty(DamageRegion(pvfb->damage));
    priv = root ? exaGetPixmapDriverPrivate(root) : NULL;

    if (!priv)
        // Impossible situation, but let's skip this step
        return TRUE;

    if (nonEmpty && priv->buffer) {
        // We should unlock and lock buffer in order to update texture content on some devices
        // In most cases AHardwareBuffer uses DMA memory which is shared between CPU and GPU
        // and this is not needed. But according to docs we should do it for any case.
        // Also according to AHardwareBuffer docs simultaneous reading in rendering thread and
        // locking for writing in other thread is fine.
        if (priv->locked) {
            LorieBuffer_unlock(priv->buffer);
            status = LorieBuffer_lock(priv->buffer, &priv->locked);
            if (status)
                FatalError("Failed to lock the surface: %d\n", status);
        }

        DamageEmpty(pvfb->damage);
        pvfb->state->drawRequested = TRUE;
    }

    if (pvfb->state->drawRequested || pvfb->state->cursor.moved || pvfb->state->cursor.updated) {
        pvfb->state->rootWindowTextureID = LorieBuffer_description(priv->buffer)->id;

        // Sending signal about pending root window changes to renderer thread.
        // We do not explicitly lock the pvfb->state->lock here because we do not want to wait
        // for all drawing operations to be finished.
        // Renderer thread will check the `drawRequested` flag right before going to sleep.
        pthread_cond_signal(&pvfb->state->cond);
    }

    return TRUE;
}

static CARD32 lorieFramecounter(unused OsTimerPtr timer, unused CARD32 time, unused void *arg) {
    if (pvfb->state->renderedFrames)
        log(INFO, "%d frames in 5.0 seconds = %.1f FPS",
            pvfb->state->renderedFrames, ((float) pvfb->state->renderedFrames) / 5);
    pvfb->state->renderedFrames = 0;
    return 5000;
}

static Bool lorieCreateScreenResources(ScreenPtr pScreen) {
    pScreen->devPrivate = pScreen->CreatePixmap(pScreen, pScreen->width, pScreen->height, pScreen->rootDepth, CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED);

    pvfb->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE, pScreen, NULL);
    if (!pvfb->damage)
        FatalError("Couldn't setup damage\n");

    DamageRegister(&(*pScreen->GetScreenPixmap)(pScreen)->drawable, pvfb->damage);
    pvfb->fpsTimer = TimerSet(NULL, 0, 5000, lorieFramecounter, pScreen);

    lorieRegisterBuffer(LORIE_BUFFER_FROM_PIXMAP(pScreenPtr->devPrivate));

    return TRUE;
}

static Bool lorieCloseScreen(ScreenPtr pScreen) {
    pScreenPtr = NULL;
    pScreen->DestroyPixmap(pScreen->devPrivate);
    pScreen->devPrivate = NULL;
    pScreen->CloseScreen = pvfb->CloseScreen;
    return pScreen->CloseScreen(pScreen);
}

// Forward decls (defined below) so a resized top-level window can be re-exported here.
static int lorieRootlessEnabled(void);
static Bool lorieIsTopLevel(WindowPtr pWin);
static void lorieReportWindow(WindowPtr pWin, uint8_t mapped);

void lorieSetWindowPixmap(WindowPtr pWindow, PixmapPtr newPixmap) {
    bool isRoot = pWindow == pScreenPtr->root;
    PixmapPtr oldPixmap = isRoot ? pScreenPtr->GetWindowPixmap(pWindow) : NULL;
    LoriePixmapPriv *old, *new;
    if (isRoot) {
        old = LORIE_PIXMAP_PRIV_FROM_PIXMAP(oldPixmap);
        new = LORIE_PIXMAP_PRIV_FROM_PIXMAP(newPixmap);
        if (old && old->buffer && old->locked) {
            LorieBuffer_unlock(old->buffer);
            old->locked = NULL;
            old->wasLocked = false;
        }
        if (new && new->buffer && !new->locked) {
            LorieBuffer_lock(new->buffer, &new->locked);
            new->wasLocked = false;
        }
    }

    pScreenPtr->SetWindowPixmap = pvfb->SetWindowPixmap;
    (*pScreenPtr->SetWindowPixmap) (pWindow, newPixmap);
    pvfb->SetWindowPixmap = pScreenPtr->SetWindowPixmap;
    pScreenPtr->SetWindowPixmap = lorieSetWindowPixmap;

    // Breadstick rootless: a top-level window's backing pixmap just changed — typically because
    // it was resized (Composite reallocated it). Re-export the new buffer + geometry so the
    // host (Android) window redraws at the new size instead of stretching a stale buffer.
    if (!isRoot && lorieRootlessEnabled() && pWindow->realized && lorieIsTopLevel(pWindow))
        lorieReportWindow(pWindow, 1);
}

static int lorieSetPixmapVisitWindow(WindowPtr window, void *data) {
    ScreenPtr screen = window->drawable.pScreen;

    if (screen->GetWindowPixmap(window) == data) {
        screen->SetWindowPixmap(window, screen->GetScreenPixmap(screen));
        return WT_WALKCHILDREN;
    }

    return WT_DONTWALKCHILDREN;
}

static Bool lorieRRScreenSetSize(ScreenPtr pScreen, CARD16 width, CARD16 height, unused CARD32 mmWidth, unused CARD32 mmHeight) {
    PixmapPtr oldPixmap, newPixmap;
    BoxRec box = { 0, 0, width, height };

    // Drain all pending vblanks.
    loriePerformVblanks();

    // Restore root window pixmap.
    present_restore_screen_pixmap(pScreenPtr);

    SetRootClip(pScreen, ROOT_CLIP_NONE);

    pScreen->root->drawable.width = pvfb->root.width = pScreen->width = width;
    pScreen->root->drawable.height = pvfb->root.height = pScreen->height = height;
    pScreen->mmWidth = ((double) (width)) * 25.4 / monitorResolution;
    pScreen->mmHeight = ((double) (height)) * 25.4 / monitorResolution;

    oldPixmap = pScreen->GetScreenPixmap(pScreen);
    newPixmap = pScreen->CreatePixmap(pScreen, width, height, pScreen->rootDepth, CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED);
    pScreen->SetScreenPixmap(newPixmap);
    if (pvfb->damage) {
        DamageUnregister(pvfb->damage);
        DamageDestroy(pvfb->damage);
    }

    pvfb->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE, pScreen, NULL);
    if (!pvfb->damage)
        FatalError("Couldn't setup damage\n");

    DamageRegister(&newPixmap->drawable, pvfb->damage);

    if (oldPixmap) {
        GCPtr gc = GetScratchGC(newPixmap->drawable.depth, pScreen);
        if (gc) {
            ValidateGC(&newPixmap->drawable, gc);
            gc->ops->CopyArea(&oldPixmap->drawable, &newPixmap->drawable, gc, 0, 0, min(oldPixmap->drawable.width, newPixmap->drawable.width), min(oldPixmap->drawable.height, newPixmap->drawable.height), 0, 0);
            FreeScratchGC(gc);
        }
        TraverseTree(pScreen->root, lorieSetPixmapVisitWindow, oldPixmap);
        pScreen->DestroyPixmap(oldPixmap);
    }

    lorieRegisterBuffer(LORIE_BUFFER_FROM_PIXMAP(pScreenPtr->devPrivate));

    pScreen->ResizeWindow(pScreen->root, 0, 0, width, height, NULL);
    RegionReset(&pScreen->root->winSize, &box);

    SetRootClip(pScreen, ROOT_CLIP_FULL);

    RRScreenSizeNotify(pScreen);
    update_desktop_dimensions();
    pvfb->state->cursor.moved = TRUE;

    return TRUE;
}

static Bool lorieRRCrtcSet(unused ScreenPtr pScreen, RRCrtcPtr crtc, RRModePtr mode, int x, int y,
               Rotation rotation, int numOutput, RROutputPtr *outputs) {
    return (crtc && mode) ? RRCrtcNotify(crtc, mode, x, y, rotation, NULL, numOutput, outputs) : FALSE;
}

static Bool lorieRRGetInfo(unused ScreenPtr pScreen, Rotation *rotations) {
    *rotations = RR_Rotate_0;
    return TRUE;
}

static Bool lorieRandRInit(ScreenPtr pScreen) {
    rrScrPrivPtr pScrPriv;
    RROutputPtr output;
    RRCrtcPtr crtc;
    RRModePtr mode;

    if (!RRScreenInit(pScreen))
       return FALSE;

    pScrPriv = rrGetScrPriv(pScreen);
    pScrPriv->rrGetInfo = lorieRRGetInfo;
    pScrPriv->rrCrtcSet = lorieRRCrtcSet;
    pScrPriv->rrScreenSetSize = lorieRRScreenSetSize;

    RRScreenSetSizeRange(pScreen, 1, 1, 32767, 32767);

    if (FALSE
        || !(mode = lorieCvt(pScreen->width, pScreen->height, pvfb->root.framerate))
        || !(crtc = RRCrtcCreate(pScreen, NULL))
        || !RRCrtcGammaSetSize(crtc, 256)
        || !(output = RROutputCreate(pScreen, pvfb->root.name, sizeof(pvfb->root.name), NULL))
        || (output->nameLength = strlen(output->name), FalseNoop())
        || !RROutputSetClones(output, NULL, 0)
        || !RROutputSetModes(output, &mode, 1, 0)
        || !RROutputSetCrtcs(output, &crtc, 1)
        || !RROutputSetConnection(output, RR_Connected)
        || !RRCrtcNotify(crtc, mode, 0, 0, RR_Rotate_0, NULL, 1, &output))
        return FALSE;
    return TRUE;
}

void lorieWakeServer(void) {
    // Wake the server if it sleeps.
    eventfd_write(pvfb->eventFd, 1);
}

static void lorieWorkingQueueCallback(int fd, int __unused ready, void __unused *data) {
    // Nothing to do here. It is needed to interrupt ospoll_wait.
    eventfd_t dummy;
    eventfd_read(fd, &dummy);
}

void lorieChoreographerFrameCallback(__unused long t, AChoreographer* d) {
    AChoreographer_postFrameCallback(d, (AChoreographer_frameCallback) lorieChoreographerFrameCallback, d);
    if (pScreenPtr) {
        QueueWorkProc(lorieRedraw, NULL, NULL);
        lorieWakeServer();
    }
}

// --- Breadstick rootless: track top-level windows + export each one's pixmap to Android. ---
extern ClientPtr serverClient;
extern int compRedirectSubwindows(ClientPtr pClient, WindowPtr pWin, int update);
#define BREADSTICK_REDIRECT_MANUAL 1   // CompositeRedirectManual

static Bool (*lorieRealizeWindow)(WindowPtr);
static Bool (*lorieUnrealizeWindow)(WindowPtr);
static Bool (*lorieDestroyWindow)(WindowPtr);
static Bool (*loriePositionWindow)(WindowPtr, int, int);

static int lorieRootlessEnabled(void) {
    static int v = -1;
    if (v == -1) v = getenv("BREADSTICK_ROOTLESS") ? 1 : 0;
    return v;
}

static Bool lorieIsTopLevel(WindowPtr pWin) {
    return pWin && pWin->parent && pWin->drawable.pScreen && pWin->parent == pWin->drawable.pScreen->root;
}

// Redirect every top-level window once (Composite manual) so each gets its own backing
// pixmap — and therefore its own AHardwareBuffer — that we can hand to Android per-window.
static void lorieEnsureRedirect(ScreenPtr s) {
    static int done = 0;
    if (done || !s || !s->root)
        return;
    done = 1;
    compRedirectSubwindows(serverClient, s->root, BREADSTICK_REDIRECT_MANUAL);
}

// Make sure the window's backing pixmap is AHardwareBuffer-backed (shippable), register it,
// and return its buffer id. 0 if the window has no usable buffer yet.
static uint64_t lorieWindowBufferId(WindowPtr pWin) {
    ScreenPtr s = pWin->drawable.pScreen;
    if (!s || !s->GetWindowPixmap)
        return 0;
    PixmapPtr pix = (*s->GetWindowPixmap)(pWin);
    if (!pix)
        return 0;
    LoriePixmapPriv* priv = (LoriePixmapPriv*) exaGetPixmapDriverPrivate(pix);
    if (!priv || !priv->buffer)
        return 0;
    const LorieBuffer_Desc* desc = LorieBuffer_description(priv->buffer);
    if (desc->type == LORIEBUFFER_REGULAR) {
        // Convert to a shareable AHardwareBuffer, then — critically — repoint the pixmap at the
        // new buffer and re-lock it. Skipping this leaves the server drawing into the freed
        // REGULAR buffer (use-after-free → SIGSEGV). Mirrors loriePresentFlip exactly.
        LorieBuffer_convert(priv->buffer, LORIEBUFFER_AHARDWAREBUFFER, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
        if (desc->type != LORIEBUFFER_REGULAR) {
            pScreenPtr->ModifyPixmapHeader(pix, 0, 0, 0, 0, desc->stride * 4, NULL);
            LorieBuffer_lock(priv->buffer, &priv->locked);
        }
    }
    if (desc->type != LORIEBUFFER_AHARDWAREBUFFER)
        return 0;
    lorieRegisterBuffer(priv->buffer);
    return desc->id;
}

// Append the UTF-8 in [src, src + n) to out (cap bytes, NUL included), cutting only between
// characters and turning anything that is not well-formed UTF-8 into '?'. A title cut in the middle
// of a character (a 95-byte cap landing inside "—") reached NewStringUTF on the renderer side, and
// CheckJNI aborts the whole app on that (2026-10-05, the Googlebook pass). Returns the new length.
static size_t lorieAppendUtf8(char *out, size_t len, size_t cap, const uint8_t *src, size_t n) {
    size_t i = 0;
    while (i < n && src[i] && len + 1 < cap) {
        uint8_t c = src[i];
        size_t k = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 && c <= 0xF4 ? 4 : 0;
        bool ok = k != 0 && !(k == 2 && c < 0xC2) && i + k <= n;
        for (size_t j = 1; ok && j < k; j++)
            ok = (src[i + j] & 0xC0) == 0x80;
        if (!ok) {
            out[len++] = '?';
            i++;
            continue;
        }
        if (len + k >= cap)
            break; // the next character does not fit whole: stop before it, never inside it
        memcpy(out + len, src + i, k);
        len += k;
        i += k;
    }
    out[len] = 0;
    return len;
}

// Append ISO 8859-1 text (what STRING properties hold: WM_NAME, WM_CLASS) as UTF-8.
static size_t lorieAppendLatin1(char *out, size_t len, size_t cap, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n && src[i]; i++) {
        uint8_t c = src[i];
        if (c < 0x80) {
            if (len + 1 >= cap) break;
            out[len++] = (char) c;
        } else {
            if (len + 2 >= cap) break;
            out[len++] = (char) (0xC0 | (c >> 6));
            out[len++] = (char) (0x80 | (c & 0x3F));
        }
    }
    out[len] = 0;
    return len;
}

// The window's user-visible title: _NET_WM_NAME (UTF-8) preferred, WM_NAME as fallback. Always
// valid UTF-8, truncated between characters.
static void lorieWindowTitle(WindowPtr pWin, char *out, size_t cap) {
    static Atom netWmName, utf8String;
    PropertyPtr prop;
    out[0] = 0;
    if (!netWmName) {
        netWmName = MakeAtom("_NET_WM_NAME", 12, TRUE);
        utf8String = MakeAtom("UTF8_STRING", 11, TRUE);
    }
    if (dixLookupProperty(&prop, pWin, netWmName, serverClient, DixReadAccess) == Success
            && prop && prop->type == utf8String && prop->format == 8 && prop->size) {
        lorieAppendUtf8(out, 0, cap, prop->data, prop->size);
        return;
    }
    if (dixLookupProperty(&prop, pWin, XA_WM_NAME, serverClient, DixReadAccess) == Success
            && prop && prop->format == 8 && prop->size) {
        if (prop->type == utf8String) {
            lorieAppendUtf8(out, 0, cap, prop->data, prop->size);
        } else if (prop->type == XA_STRING) {
            lorieAppendLatin1(out, 0, cap, prop->data, prop->size);
        } else {
            // COMPOUND_TEXT and anything else: its plain ASCII, without the escape sequences.
            size_t len = 0;
            const uint8_t *d = prop->data;
            for (size_t i = 0; i < (size_t) prop->size && d[i] && len + 1 < cap; i++)
                if (d[i] >= 0x20 && d[i] < 0x7F) out[len++] = (char) d[i];
            out[len] = 0;
        }
    }
}

// WM_CLASS as "instance\tclass". The property holds two NUL-terminated ISO 8859-1 strings; the app
// matches the pair against desktop entries (StartupWMClass, then the entry's name), which is how it
// knows which installed app a window belongs to without guessing from the title.
static void lorieWindowClass(WindowPtr pWin, char *out, size_t cap) {
    PropertyPtr prop;
    out[0] = 0;
    if (dixLookupProperty(&prop, pWin, XA_WM_CLASS, serverClient, DixReadAccess) != Success
            || !prop || prop->format != 8 || !prop->size)
        return;
    const uint8_t *d = prop->data;
    size_t n = prop->size;
    size_t first = strnlen((const char *) d, n);
    size_t len = lorieAppendLatin1(out, 0, cap, d, first);
    if (first < n && len + 1 < cap) {
        out[len++] = '\t';
        out[len] = 0;
        lorieAppendLatin1(out, len, cap, d + first + 1, n - first - 1);
    }
}

// What kind of window this is, from _NET_WM_WINDOW_TYPE (the first type it lists that we know):
// 0 normal (or none given), 1 dialog, 2 splash, 4 tooltip, 5 notification, 3 anything else that is
// not an app's main window (utility, toolbar, menu, ...). The app uses it to keep splash screens and
// dialogs from being remembered as the app's place, and the Enhanced Desktop to tell a menu, which
// holds the pointer while it is open, from a tooltip, which does not.
static uint8_t lorieWindowKind(WindowPtr pWin) {
    static Atom wmType, tNormal, tDialog, tSplash, tTooltip, tNotification;
    PropertyPtr prop;
    if (!wmType) {
        wmType = MakeAtom("_NET_WM_WINDOW_TYPE", 19, TRUE);
        tNormal = MakeAtom("_NET_WM_WINDOW_TYPE_NORMAL", 26, TRUE);
        tDialog = MakeAtom("_NET_WM_WINDOW_TYPE_DIALOG", 26, TRUE);
        tSplash = MakeAtom("_NET_WM_WINDOW_TYPE_SPLASH", 26, TRUE);
        tTooltip = MakeAtom("_NET_WM_WINDOW_TYPE_TOOLTIP", 27, TRUE);
        tNotification = MakeAtom("_NET_WM_WINDOW_TYPE_NOTIFICATION", 32, TRUE);
    }
    if (dixLookupProperty(&prop, pWin, wmType, serverClient, DixReadAccess) != Success
            || !prop || prop->format != 32 || prop->type != XA_ATOM || !prop->size)
        return 0;
    const CARD32 *atoms = (const CARD32 *) prop->data;
    for (unsigned long i = 0; i < prop->size; i++) {
        if (atoms[i] == tNormal) return 0;
        if (atoms[i] == tDialog) return 1;
        if (atoms[i] == tSplash) return 2;
        if (atoms[i] == tTooltip) return 4;
        if (atoms[i] == tNotification) return 5;
    }
    return 3;
}

// WM_TRANSIENT_FOR: the window this one belongs to (a dialog's parent), 0 when none.
static uint32_t lorieWindowTransientFor(WindowPtr pWin) {
    PropertyPtr prop;
    if (dixLookupProperty(&prop, pWin, XA_WM_TRANSIENT_FOR, serverClient, DixReadAccess) != Success
            || !prop || prop->format != 32 || !prop->size)
        return 0;
    return ((const CARD32 *) prop->data)[0];
}

// The smallest and largest size the window accepts (WM_NORMAL_HINTS), 0 where it says nothing.
// The base size stands in for a missing minimum, as ICCCM says.
static void lorieWindowSizeHints(WindowPtr pWin, uint16_t *minW, uint16_t *minH, uint16_t *maxW, uint16_t *maxH) {
    PropertyPtr prop;
    *minW = *minH = *maxW = *maxH = 0;
    if (dixLookupProperty(&prop, pWin, XA_WM_NORMAL_HINTS, serverClient, DixReadAccess) != Success
            || !prop || prop->format != 32 || prop->size < 9)
        return;
    const CARD32 *h = (const CARD32 *) prop->data;
    CARD32 flags = h[0];
    #define LORIE_HINT(v) ((uint16_t) ((int32_t) (v) <= 0 ? 0 : (v) > 65535 ? 65535 : (v)))
    if (flags & (1L << 4)) { // PMinSize
        *minW = LORIE_HINT(h[5]);
        *minH = LORIE_HINT(h[6]);
    } else if ((flags & (1L << 8)) && prop->size >= 17) { // PBaseSize
        *minW = LORIE_HINT(h[15]);
        *minH = LORIE_HINT(h[16]);
    }
    if (flags & (1L << 5)) { // PMaxSize
        *maxW = LORIE_HINT(h[7]);
        *maxH = LORIE_HINT(h[8]);
    }
    #undef LORIE_HINT
}

// The process that owns the window, as the kernel reported it for the client's socket. proot does
// not renumber processes, so this is the real pid, and its TracerPid is the proot the app started.
static int32_t lorieWindowPid(WindowPtr pWin) {
    ClientPtr client = wClient(pWin);
    if (!client || client == serverClient)
        return -1;
    pid_t pid = GetClientPid(client);
    return pid > 0 ? (int32_t) pid : -1;
}

// The atoms the window manager side reads and writes, made once.
static struct {
    Atom motifHints, gtkFrameExtents, netWmState, stateMaxV, stateMaxH, stateFullscreen, stateHidden, stateAbove;
    Atom moveResize, moveResizeWindow, activeWindow, closeWindow, wmChangeState, wmProtocols, wmDeleteWindow;
    Atom supported, supportingWmCheck, netWmName, utf8String;
} lorieAtoms;

static void lorieMakeAtoms(void) {
    if (lorieAtoms.netWmState)
        return;
#define A(name) MakeAtom(name, strlen(name), TRUE)
    lorieAtoms.motifHints = A("_MOTIF_WM_HINTS");
    lorieAtoms.gtkFrameExtents = A("_GTK_FRAME_EXTENTS");
    lorieAtoms.stateMaxV = A("_NET_WM_STATE_MAXIMIZED_VERT");
    lorieAtoms.stateMaxH = A("_NET_WM_STATE_MAXIMIZED_HORZ");
    lorieAtoms.stateFullscreen = A("_NET_WM_STATE_FULLSCREEN");
    lorieAtoms.stateHidden = A("_NET_WM_STATE_HIDDEN");
    lorieAtoms.stateAbove = A("_NET_WM_STATE_ABOVE");
    lorieAtoms.moveResize = A("_NET_WM_MOVERESIZE");
    lorieAtoms.moveResizeWindow = A("_NET_MOVERESIZE_WINDOW");
    lorieAtoms.activeWindow = A("_NET_ACTIVE_WINDOW");
    lorieAtoms.closeWindow = A("_NET_CLOSE_WINDOW");
    lorieAtoms.wmChangeState = A("WM_CHANGE_STATE");
    lorieAtoms.wmProtocols = A("WM_PROTOCOLS");
    lorieAtoms.wmDeleteWindow = A("WM_DELETE_WINDOW");
    lorieAtoms.supported = A("_NET_SUPPORTED");
    lorieAtoms.supportingWmCheck = A("_NET_SUPPORTING_WM_CHECK");
    lorieAtoms.netWmName = A("_NET_WM_NAME");
    lorieAtoms.utf8String = A("UTF8_STRING");
    lorieAtoms.netWmState = A("_NET_WM_STATE"); // last: it is the "made" marker
#undef A
}

// LORIE_STATE_* bits for one _NET_WM_STATE atom, 0 for one the app does not handle.
static uint32_t lorieStateBit(Atom a) {
    if (!a) return 0;
    if (a == lorieAtoms.stateMaxV) return LORIE_STATE_MAXIMIZED_VERT;
    if (a == lorieAtoms.stateMaxH) return LORIE_STATE_MAXIMIZED_HORZ;
    if (a == lorieAtoms.stateFullscreen) return LORIE_STATE_FULLSCREEN;
    if (a == lorieAtoms.stateHidden) return LORIE_STATE_HIDDEN;
    if (a == lorieAtoms.stateAbove) return LORIE_STATE_ABOVE;
    return 0;
}

// What the window says about its frame and state (LORIE_WINDOW_* bits), and the margin a
// client-decorated window draws around itself (_GTK_FRAME_EXTENTS: left, right, top, bottom).
//
// The Enhanced Desktop draws a title bar only for windows that have none of their own. GTK's header
// bars, Chromium, Electron and libadwaita draw their own and say so with _MOTIF_WM_HINTS (no
// decorations); GTK adds _GTK_FRAME_EXTENTS, the invisible border it resizes from, which the app
// must leave out of the window's visible shape.
static uint8_t lorieWindowFlags(WindowPtr pWin, uint16_t *frame) {
    PropertyPtr prop;
    uint8_t flags = 0;
    frame[0] = frame[1] = frame[2] = frame[3] = 0;
    lorieMakeAtoms();
    if (dixLookupProperty(&prop, pWin, lorieAtoms.motifHints, serverClient, DixReadAccess) == Success
            && prop && prop->format == 32 && prop->size >= 3) {
        const CARD32 *h = (const CARD32 *) prop->data;
        if ((h[0] & (1L << 1)) && h[2] == 0) // MWM_HINTS_DECORATIONS, and none of them
            flags |= LORIE_WINDOW_UNDECORATED;
    }
    if (dixLookupProperty(&prop, pWin, lorieAtoms.gtkFrameExtents, serverClient, DixReadAccess) == Success
            && prop && prop->format == 32 && prop->size >= 4) {
        const CARD32 *f = (const CARD32 *) prop->data;
        flags |= LORIE_WINDOW_FRAME_EXTENTS;
        for (int i = 0; i < 4; i++)
            frame[i] = f[i] > 1024 ? 1024 : (uint16_t) f[i];
    }
    if (dixLookupProperty(&prop, pWin, lorieAtoms.netWmState, serverClient, DixReadAccess) == Success
            && prop && prop->format == 32 && prop->type == XA_ATOM) {
        uint32_t bits = 0;
        for (unsigned long i = 0; i < prop->size; i++)
            bits |= lorieStateBit(((const CARD32 *) prop->data)[i]);
        if (bits & LORIE_STATE_FULLSCREEN) flags |= LORIE_WINDOW_FULLSCREEN;
        if ((bits & LORIE_STATE_MAXIMIZED_VERT) && (bits & LORIE_STATE_MAXIMIZED_HORZ)) flags |= LORIE_WINDOW_MAXIMIZED;
        if (bits & LORIE_STATE_HIDDEN) flags |= LORIE_WINDOW_HIDDEN;
    }
    return flags;
}

static void lorieReportWindow(WindowPtr pWin, uint8_t mapped) {
    char title[96];
    char wmClass[64];
    uint16_t minW, minH, maxW, maxH;
    uint16_t frame[4];
    if (!lorieIsTopLevel(pWin))
        return;
    uint64_t buffer = mapped ? lorieWindowBufferId(pWin) : 0;
    lorieWindowTitle(pWin, title, sizeof(title));
    lorieWindowClass(pWin, wmClass, sizeof(wmClass));
    lorieWindowSizeHints(pWin, &minW, &minH, &maxW, &maxH);
    uint8_t flags = lorieWindowFlags(pWin, frame);
    lorieSendWindowState((uint32_t) pWin->drawable.id, pWin->drawable.x, pWin->drawable.y,
                         pWin->drawable.width, pWin->drawable.height, mapped,
                         pWin->overrideRedirect ? 1 : 0, buffer, title,
                         lorieWindowPid(pWin), wmClass, minW, minH, maxW, maxH,
                         lorieWindowTransientFor(pWin), lorieWindowKind(pWin), flags, frame);
}

// --- Breadstick rootless: per-window damage. ---
//
// The app used to redraw every Linux window on a 40 ms timer, changed or not, which kept the GPU
// and the CPU awake for a still desktop. Each mapped top-level window now carries a Damage; the
// first drawing after a frame marks it, and the frame clock (lorieRedraw, once a vsync) tells the
// app which windows changed and clears them. So the app hears about a window at most once a frame,
// and never about one that did not draw.
#define LORIE_MAX_DAMAGE 256
static struct {
    WindowPtr win;
    DamagePtr damage;
    volatile uint8_t dirty;
} lorieDamage[LORIE_MAX_DAMAGE];
static volatile Bool lorieDamagePending;

static void lorieWindowDamaged(unused DamagePtr pDamage, unused RegionPtr pRegion, void *closure) {
    intptr_t slot = (intptr_t) closure;
    if (slot >= 0 && slot < LORIE_MAX_DAMAGE) {
        lorieDamage[slot].dirty = 1;
        lorieDamagePending = TRUE;
    }
}

// The damage layer destroys a window's Damage itself when the window goes: forget it then, so the
// slot never points at freed memory.
static void lorieWindowDamageGone(unused DamagePtr pDamage, void *closure) {
    intptr_t slot = (intptr_t) closure;
    if (slot >= 0 && slot < LORIE_MAX_DAMAGE) {
        lorieDamage[slot].win = NULL;
        lorieDamage[slot].damage = NULL;
        lorieDamage[slot].dirty = 0;
    }
}

static void lorieTrackDamage(WindowPtr pWin) {
    int free_slot = -1;
    for (int i = 0; i < LORIE_MAX_DAMAGE; i++) {
        if (lorieDamage[i].win == pWin)
            return;
        if (free_slot < 0 && !lorieDamage[i].damage)
            free_slot = i;
    }
    if (free_slot < 0) {
        log(ERROR, "lorieDamage full (%d windows): window 0x%x redraws only when it moves", LORIE_MAX_DAMAGE, (unsigned) pWin->drawable.id);
        return;
    }
    DamagePtr d = DamageCreate(lorieWindowDamaged, lorieWindowDamageGone, DamageReportNonEmpty, FALSE,
                               pWin->drawable.pScreen, (void *) (intptr_t) free_slot);
    if (!d)
        return;
    lorieDamage[free_slot].win = pWin;
    lorieDamage[free_slot].damage = d;
    lorieDamage[free_slot].dirty = 1; // its first frame
    lorieDamagePending = TRUE;
    DamageRegister(&pWin->drawable, d);
}

static void lorieUntrackDamage(WindowPtr pWin) {
    for (int i = 0; i < LORIE_MAX_DAMAGE; i++) {
        if (lorieDamage[i].win == pWin && lorieDamage[i].damage) {
            DamageDestroy(lorieDamage[i].damage); // calls lorieWindowDamageGone, which clears the slot
            lorieDamage[i].win = NULL;
            lorieDamage[i].damage = NULL;
            lorieDamage[i].dirty = 0;
            return;
        }
    }
}

static void lorieFlushWindowDamage(void) {
    if (!lorieDamagePending || !lorieRootlessEnabled())
        return;
    lorieDamagePending = FALSE;
    for (int i = 0; i < LORIE_MAX_DAMAGE; i++) {
        WindowPtr pWin = lorieDamage[i].win;
        if (!lorieDamage[i].dirty || !pWin || !lorieDamage[i].damage)
            continue;
        lorieDamage[i].dirty = 0;
        DamageEmpty(lorieDamage[i].damage);
        // As lorieRedraw does for the root: unlocking a CPU-written AHardwareBuffer is what makes
        // its new contents visible to the GPU on devices whose buffers are not coherent.
        PixmapPtr pix = pScreenPtr->GetWindowPixmap ? pScreenPtr->GetWindowPixmap(pWin) : NULL;
        LoriePixmapPriv *priv = pix ? exaGetPixmapDriverPrivate(pix) : NULL;
        if (priv && priv->buffer && priv->locked && LorieBuffer_description(priv->buffer)->type == LORIEBUFFER_AHARDWAREBUFFER) {
            LorieBuffer_unlock(priv->buffer);
            if (LorieBuffer_lock(priv->buffer, &priv->locked))
                priv->locked = NULL; // loriePrepareAccess locks it again for the next drawing
        }
        lorieSendWindowDamage((uint32_t) pWin->drawable.id);
    }
}

// --- Breadstick rootless: the window manager's half of EWMH, for the Enhanced Desktop. ---
//
// There is no X window manager in Floating or Enhanced mode. In Enhanced mode the app is the window
// manager (it draws the frames, moves, resizes and stacks), so what clients ask a window manager
// for goes to it: a GTK header bar dragged, Chromium's maximize button, an app presenting itself.
// Clients send those as ClientMessages to the root; with nobody selecting SubstructureRedirect
// there, they reached no one. SendEvent is wrapped to pass them on.

static int lorieWmEnabled(void) {
    static int v = -1;
    if (v == -1) v = getenv("BREADSTICK_WM") ? 1 : 0;
    return v;
}

static int (*lorieProcSendEventOrig)(ClientPtr);

static WindowPtr lorieTopLevelOf(XID id) {
    WindowPtr pWin;
    if (dixLookupWindow(&pWin, id, serverClient, DixGetAttrAccess) != Success || !pWin)
        return NULL;
    while (pWin && pWin->parent && !lorieIsTopLevel(pWin))
        pWin = pWin->parent;
    return lorieIsTopLevel(pWin) ? pWin : NULL;
}

static void lorieClientMessage(const xEvent *ev) {
    lorieMakeAtoms();
    Atom type = ev->u.clientMessage.u.l.type;
    const INT32 *l = &ev->u.clientMessage.u.l.longs0;
    if (type != lorieAtoms.moveResize && type != lorieAtoms.netWmState && type != lorieAtoms.activeWindow
            && type != lorieAtoms.closeWindow && type != lorieAtoms.wmChangeState && type != lorieAtoms.moveResizeWindow)
        return;
    WindowPtr pWin = lorieTopLevelOf(ev->u.clientMessage.window);
    if (!pWin)
        return;
    uint32_t id = (uint32_t) pWin->drawable.id;
    if (type == lorieAtoms.moveResize) {
        lorieSendWindowRequest(id, LORIE_REQUEST_MOVERESIZE, l[0], l[1], l[2], l[3]);
    } else if (type == lorieAtoms.netWmState) {
        uint32_t bits = lorieStateBit((Atom) l[1]) | lorieStateBit((Atom) l[2]);
        if (bits)
            lorieSendWindowRequest(id, LORIE_REQUEST_STATE, l[0], (int32_t) bits, 0, 0);
    } else if (type == lorieAtoms.activeWindow) {
        lorieSendWindowRequest(id, LORIE_REQUEST_ACTIVATE, l[0], 0, 0, 0);
    } else if (type == lorieAtoms.closeWindow) {
        lorieSendWindowRequest(id, LORIE_REQUEST_CLOSE, 0, 0, 0, 0);
    } else if (type == lorieAtoms.wmChangeState) {
        if (l[0] == 3) // IconicState (ICCCM 4.1.4)
            lorieSendWindowRequest(id, LORIE_REQUEST_ICONIFY, 0, 0, 0, 0);
    } else if (type == lorieAtoms.moveResizeWindow) {
        // flags in l[0]: bits 8-11 say which of x, y, width, height are given.
        int32_t f = l[0];
        lorieSendWindowRequest(id, LORIE_REQUEST_MOVERESIZE_WINDOW,
                               (f & (1 << 8)) ? l[1] : -1, (f & (1 << 9)) ? l[2] : -1,
                               (f & (1 << 10)) ? l[3] : -1, (f & (1 << 11)) ? l[4] : -1);
    }
}

static int lorieProcSendEvent(ClientPtr client) {
    REQUEST(xSendEventReq);
    if (client->req_len == bytes_to_int32(sizeof(xSendEventReq)) && (stuff->event.u.u.type & 0x7F) == ClientMessage
            && stuff->event.u.u.detail == 32)
        lorieClientMessage(&stuff->event);
    return lorieProcSendEventOrig(client);
}

// Tell clients a window manager is here, and which of its requests it answers. GTK sends a
// header-bar drag to the window manager only when _NET_SUPPORTED lists _NET_WM_MOVERESIZE (without
// one it moves the window itself, which the app then only follows a step behind, and nothing could
// snap it to an edge). Only in Enhanced mode (BREADSTICK_WM): in Floating mode Android moves the
// windows and nothing could answer these, and the Classic desktop has xfwm4.
static void lorieSetupWmHints(void) {
    static Bool done = FALSE;
    if (done || !lorieWmEnabled() || !lorieRootlessEnabled() || !pScreenPtr || !pScreenPtr->root)
        return;
    done = TRUE;
    lorieMakeAtoms();
    int err = Success;
    XID id = FakeClientID(0);
    WindowPtr check = CreateWindow(id, pScreenPtr->root, -100, -100, 1, 1, 0, InputOnly, 0, NULL, 0,
                                   serverClient, CopyFromParent, &err);
    if (!check || err != Success || !AddResource(id, RT_WINDOW, check)) {
        log(ERROR, "window manager hints: could not create the check window (%d)", err);
        return;
    }
    CARD32 cid = (CARD32) id;
    dixChangeWindowProperty(serverClient, check, lorieAtoms.supportingWmCheck, XA_WINDOW, 32, PropModeReplace, 1, &cid, FALSE);
    dixChangeWindowProperty(serverClient, check, lorieAtoms.netWmName, lorieAtoms.utf8String, 8, PropModeReplace, 10, "Breadstick", FALSE);
    dixChangeWindowProperty(serverClient, pScreenPtr->root, lorieAtoms.supportingWmCheck, XA_WINDOW, 32, PropModeReplace, 1, &cid, FALSE);
    CARD32 supported[] = {
        lorieAtoms.supported, lorieAtoms.supportingWmCheck, lorieAtoms.moveResize, lorieAtoms.moveResizeWindow,
        lorieAtoms.netWmState, lorieAtoms.stateMaxV, lorieAtoms.stateMaxH, lorieAtoms.stateFullscreen,
        lorieAtoms.stateHidden, lorieAtoms.activeWindow, lorieAtoms.closeWindow,
    };
    dixChangeWindowProperty(serverClient, pScreenPtr->root, lorieAtoms.supported, XA_ATOM, 32, PropModeReplace,
                            sizeof(supported) / sizeof(supported[0]), supported, FALSE);
    log(INFO, "window manager hints set (check window 0x%x)", (unsigned) id);
}

// Write [bits] (LORIE_STATE_*) as the window's _NET_WM_STATE, which the window manager owns: GTK
// draws its maximize button as "restore" and drops its rounded corners only once it reads it there.
static void lorieSetWindowState(WindowPtr pWin, uint32_t bits) {
    CARD32 atoms[5];
    int n = 0;
    lorieMakeAtoms();
    if (bits & LORIE_STATE_MAXIMIZED_VERT) atoms[n++] = lorieAtoms.stateMaxV;
    if (bits & LORIE_STATE_MAXIMIZED_HORZ) atoms[n++] = lorieAtoms.stateMaxH;
    if (bits & LORIE_STATE_FULLSCREEN) atoms[n++] = lorieAtoms.stateFullscreen;
    if (bits & LORIE_STATE_HIDDEN) atoms[n++] = lorieAtoms.stateHidden;
    if (bits & LORIE_STATE_ABOVE) atoms[n++] = lorieAtoms.stateAbove;
    dixChangeWindowProperty(serverClient, pWin, lorieAtoms.netWmState, XA_ATOM, 32, PropModeReplace, n, atoms, TRUE);
}

// Ask the window's program to close it, the way a window manager does: WM_DELETE_WINDOW, when the
// window lists it in WM_PROTOCOLS. A window that does not take it is left alone (the app's bsl-close
// decides what to do with those).
static void lorieAskToClose(WindowPtr pWin) {
    PropertyPtr prop;
    ClientPtr owner = wClient(pWin);
    lorieMakeAtoms();
    if (!owner || owner == serverClient)
        return;
    if (dixLookupProperty(&prop, pWin, lorieAtoms.wmProtocols, serverClient, DixReadAccess) != Success
            || !prop || prop->format != 32)
        return;
    Bool takes = FALSE;
    for (unsigned long i = 0; i < prop->size; i++)
        if (((const CARD32 *) prop->data)[i] == lorieAtoms.wmDeleteWindow)
            takes = TRUE;
    if (!takes)
        return;
    xEvent ev = { .u.u.type = ClientMessage, .u.u.detail = 32 };
    ev.u.clientMessage.window = pWin->drawable.id;
    ev.u.clientMessage.u.l.type = lorieAtoms.wmProtocols;
    ev.u.clientMessage.u.l.longs0 = (INT32) lorieAtoms.wmDeleteWindow;
    ev.u.clientMessage.u.l.longs1 = (INT32) GetTimeInMillis();
    WriteEventsToClient(owner, 1, &ev);
}

// EVENT_WINDOW_COMMAND, run by the main loop (queued by the input thread, which must not touch
// the window tree). Coordinates are the X screen's; ConfigureWindow keeps the protocol's own rules
// (a size of 0 is refused), and Composite gives a resized window its new pixmap, which
// lorieSetWindowPixmap reports.
Bool lorieWindowCommand(unused ClientPtr pClient, void *closure) {
    lorieEvent *e = closure;
    WindowPtr pWin;
    if (!e)
        return TRUE;
    lorieInputOrderDone();
    if (!lorieRootlessEnabled() || !pScreenPtr
            || dixLookupWindow(&pWin, e->windowCommand.window, serverClient, DixManageAccess) != Success
            || !lorieIsTopLevel(pWin)) {
        free(e);
        return TRUE;
    }
    int32_t x = e->windowCommand.x, y = e->windowCommand.y;
    int32_t w = e->windowCommand.w, h = e->windowCommand.h;
    x = x < -32768 ? -32768 : x > 32767 ? 32767 : x;
    y = y < -32768 ? -32768 : y > 32767 ? 32767 : y;
    w = w < 1 ? 1 : w > 32767 ? 32767 : w;
    h = h < 1 ? 1 : h > 32767 ? 32767 : h;
    switch (e->windowCommand.op) {
        case LORIE_COMMAND_MOVE: {
            XID v[] = { (XID) (CARD32) x, (XID) (CARD32) y };
            ConfigureWindow(pWin, CWX | CWY, v, serverClient);
            break;
        }
        case LORIE_COMMAND_RESIZE: {
            XID v[] = { (XID) w, (XID) h };
            ConfigureWindow(pWin, CWWidth | CWHeight, v, serverClient);
            break;
        }
        case LORIE_COMMAND_MOVE_RESIZE: {
            XID v[] = { (XID) (CARD32) x, (XID) (CARD32) y, (XID) w, (XID) h };
            ConfigureWindow(pWin, CWX | CWY | CWWidth | CWHeight, v, serverClient);
            break;
        }
        case LORIE_COMMAND_RAISE: {
            XID v[] = { Above };
            ConfigureWindow(pWin, CWStackMode, v, serverClient);
            break;
        }
        case LORIE_COMMAND_LOWER: {
            XID v[] = { Below };
            ConfigureWindow(pWin, CWStackMode, v, serverClient);
            break;
        }
        case LORIE_COMMAND_FOCUS: {
            XID v[] = { Above };
            ConfigureWindow(pWin, CWStackMode, v, serverClient);
            // Only a viewable window can have the focus (X refuses the rest with BadMatch).
            if (pWin->realized) {
                if (lorieKeyboard)
                    SetInputFocus(serverClient, lorieKeyboard, pWin->drawable.id, RevertToParent, CurrentTime, FALSE);
                if (lorieWmEnabled()) {
                    CARD32 active = (CARD32) pWin->drawable.id;
                    lorieMakeAtoms();
                    dixChangeWindowProperty(serverClient, pScreenPtr->root, lorieAtoms.activeWindow, XA_WINDOW, 32,
                                            PropModeReplace, 1, &active, TRUE);
                }
            }
            break;
        }
        case LORIE_COMMAND_CLOSE:
            lorieAskToClose(pWin);
            break;
        case LORIE_COMMAND_SET_STATE:
            lorieSetWindowState(pWin, (uint32_t) e->windowCommand.x);
            break;
    }
    free(e);
    return TRUE;
}

// A mapped top-level window changed a property the app shows or decides by (its title, its class,
// its kind, its size limits, its parent): report it again. Until this, only a map, a move or a new
// pixmap reported a window, so a retitle that did not resize never reached the taskbar, and
// LibreOffice's frame kept the class "soffice" it mapped with although it became
// libreoffice-writer 0.14 s later. Collected and sent from a timer, 30 ms on: properties often
// change in bursts, and a delete is announced while the old value is still there.
#define LORIE_DIRTY_MAX 32
static XID lorieDirty[LORIE_DIRTY_MAX];
static int lorieDirtyCount;
static OsTimerPtr lorieDirtyTimer;
static Bool lorieDirtyArmed;

static CARD32 lorieReportDirty(unused OsTimerPtr timer, unused CARD32 time, unused void *arg) {
    int n = lorieDirtyCount;
    lorieDirtyCount = 0;
    lorieDirtyArmed = FALSE;
    for (int i = 0; i < n; i++) {
        WindowPtr pWin;
        if (dixLookupWindow(&pWin, lorieDirty[i], serverClient, DixGetAttrAccess) == Success
                && pWin && pWin->realized && lorieIsTopLevel(pWin))
            lorieReportWindow(pWin, 1);
    }
    return 0;
}

static void loriePropertyChanged(unused CallbackListPtr *pcbl, unused void *closure, void *data) {
    static Atom netWmName, wmType;
    PropertyStateRec *rec = data;
    if (!rec || !rec->win || !rec->prop || !rec->win->realized || !lorieIsTopLevel(rec->win))
        return;
    if (!netWmName) {
        netWmName = MakeAtom("_NET_WM_NAME", 12, TRUE);
        wmType = MakeAtom("_NET_WM_WINDOW_TYPE", 19, TRUE);
    }
    Atom a = rec->prop->propertyName;
    lorieMakeAtoms();
    if (a != netWmName && a != XA_WM_NAME && a != XA_WM_CLASS && a != wmType
            && a != XA_WM_NORMAL_HINTS && a != XA_WM_TRANSIENT_FOR
            && a != lorieAtoms.motifHints && a != lorieAtoms.gtkFrameExtents && a != lorieAtoms.netWmState)
        return;
    XID id = rec->win->drawable.id;
    for (int i = 0; i < lorieDirtyCount; i++)
        if (lorieDirty[i] == id)
            return;
    if (lorieDirtyCount < LORIE_DIRTY_MAX)
        lorieDirty[lorieDirtyCount++] = id;
    // Armed once per batch, never pushed back: a program retitling itself every few milliseconds
    // (a terminal, a progress counter) would otherwise keep the report from ever going out.
    if (!lorieDirtyArmed) {
        lorieDirtyArmed = TRUE;
        lorieDirtyTimer = TimerSet(lorieDirtyTimer, 0, 30, lorieReportDirty, NULL);
    }
}

static Bool lorieRealizeWindowWrap(WindowPtr pWin) {
    if (lorieIsTopLevel(pWin))
        lorieEnsureRedirect(pWin->drawable.pScreen);
    Bool ret = lorieRealizeWindow ? lorieRealizeWindow(pWin) : TRUE;
    if (lorieIsTopLevel(pWin))
        lorieTrackDamage(pWin);
    lorieReportWindow(pWin, 1);
    return ret;
}

static Bool lorieUnrealizeWindowWrap(WindowPtr pWin) {
    if (lorieIsTopLevel(pWin))
        lorieUntrackDamage(pWin);
    lorieReportWindow(pWin, 0);
    return lorieUnrealizeWindow ? lorieUnrealizeWindow(pWin) : TRUE;
}

static Bool lorieDestroyWindowWrap(WindowPtr pWin) {
    if (lorieIsTopLevel(pWin))
        lorieUntrackDamage(pWin);
    lorieReportWindow(pWin, 0);
    return lorieDestroyWindow ? lorieDestroyWindow(pWin) : TRUE;
}

static Bool loriePositionWindowWrap(WindowPtr pWin, int x, int y) {
    Bool ret = loriePositionWindow ? loriePositionWindow(pWin, x, y) : TRUE;
    if (pWin && pWin->realized)
        lorieReportWindow(pWin, 1);
    return ret;
}

static Bool lorieScreenInit(ScreenPtr pScreen, unused int argc, unused char **argv) {
    static int eventFd = -1;
    pScreenPtr = pScreen;

    if (eventFd == -1)
        eventFd = eventfd(0, EFD_CLOEXEC);

    pvfb->eventFd = eventFd;
    SetNotifyFd(eventFd, lorieWorkingQueueCallback, X_NOTIFY_READ, NULL);

    miSetZeroLineBias(pScreen, 0);
    pScreen->blackPixel = 0;
    pScreen->whitePixel = 1;

    pvfb->vblank_interval = 1000000 / pvfb->root.framerate;

    if (FALSE
          || !miSetVisualTypesAndMasks(24, ((1 << TrueColor) | (1 << DirectColor)), 8, TrueColor, 0xFF0000, 0x00FF00, 0x0000FF)
          || !miSetPixmapDepths()
          || !fbScreenInit(pScreen, NULL, pvfb->root.width, pvfb->root.height, monitorResolution, monitorResolution, 0, 32)
          || !(pScreen->CreateScreenResources = lorieCreateScreenResources) // Simply replace unneeded function
          || !(!pvfb->dri3 || dri3_screen_init(pScreen, &lorieDri3Info))
          || !fbPictureInit(pScreen, 0, 0)
          || !exaDriverInit(pScreen, &lorieExa)
          || !lorieRandRInit(pScreen)
          || !miPointerInitialize(pScreen, &loriePointerSpriteFuncs, &loriePointerCursorFuncs, TRUE)
          || !fbCreateDefColormap(pScreen)
          || !present_screen_init(pScreen, &loriePresentInfo))
        return FALSE;

    pvfb->CloseScreen = pScreen->CloseScreen;
    pvfb->SetWindowPixmap = pScreenPtr->SetWindowPixmap;
    pScreen->CloseScreen = lorieCloseScreen;
    pScreen->SetWindowPixmap = lorieSetWindowPixmap;

    if (lorieRootlessEnabled()) {
        lorieRealizeWindow = pScreen->RealizeWindow;
        pScreen->RealizeWindow = lorieRealizeWindowWrap;
        lorieUnrealizeWindow = pScreen->UnrealizeWindow;
        pScreen->UnrealizeWindow = lorieUnrealizeWindowWrap;
        loriePositionWindow = pScreen->PositionWindow;
        pScreen->PositionWindow = loriePositionWindowWrap;
        lorieDestroyWindow = pScreen->DestroyWindow;
        pScreen->DestroyWindow = lorieDestroyWindowWrap;
        AddCallback(&PropertyStateCallback, loriePropertyChanged, NULL);
        // Once per process: the dispatch table outlives a server reset, and wrapping it twice
        // would pass every message on twice.
        if (!lorieProcSendEventOrig) {
            lorieProcSendEventOrig = ProcVector[X_SendEvent];
            ProcVector[X_SendEvent] = lorieProcSendEvent;
        }
    }

    ShmRegisterFbFuncs(pScreen);
    miSyncShmScreenInit(pScreen);

    return TRUE;
}                               /* end lorieScreenInit */

void lorieConfigureNotify(int width, int height, int framerate, size_t name_size, char* name) {
    ScreenPtr pScreen = pScreenPtr;
    RROutputPtr output = RRFirstOutput(pScreen);
    framerate = framerate ? framerate : 30;

    if (output && name) {
        // We should save this name in pvfb to make sure the name will be restored in the case if the server is being reset.
        memset(pvfb->root.name, 0, 1024);
        memset(output->name, 0, 1024);
        strncpy(pvfb->root.name, name, name_size < 1024 ? name_size : 1024);
        strncpy(output->name, name, name_size < 1024 ? name_size : 1024);
        output->name[1023] = '\0';
        output->nameLength = strlen(output->name);
    }

    if (output && width && height && (pScreen->width != width || pScreen->height != height || pvfb->root.framerate != framerate)) {
        CARD32 mmWidth, mmHeight;
        RRModePtr mode = lorieCvt(width, height, framerate);
        mmWidth = ((double) (mode->mode.width)) * 25.4 / monitorResolution;
        mmHeight = ((double) (mode->mode.width)) * 25.4 / monitorResolution;
        RROutputSetModes(output, &mode, 1, 0);
        RRCrtcNotify(RRFirstEnabledCrtc(pScreen), mode, 0, 0, RR_Rotate_0, NULL, 1, &output);
        RRScreenSizeSet(pScreen, mode->mode.width, mode->mode.height, mmWidth, mmHeight);

        log(VERBOSE, "New reported framerate is %d", framerate);
        pvfb->root.framerate = framerate;
        pvfb->vblank_interval = 1000000 / pvfb->root.framerate;
    }
}

void InitOutput(ScreenInfo * screen_info, int argc, char **argv) {
    int depths[] = { 1, 4, 8, 15, 16, 24, 32 };
    int bpp[] =    { 1, 8, 8, 16, 16, 32, 32 };
    int i;

    if (monitorResolution == 0)
        monitorResolution = 96;

    for(i = 0; i < ARRAY_SIZE(depths); i++) {
        screen_info->formats[i].depth = depths[i];
        screen_info->formats[i].bitsPerPixel = bpp[i];
        screen_info->formats[i].scanlinePad = BITMAP_SCANLINE_PAD;
    }

    screen_info->imageByteOrder = IMAGE_BYTE_ORDER;
    screen_info->bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    screen_info->bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    screen_info->bitmapBitOrder = BITMAP_BIT_ORDER;
    screen_info->numPixmapFormats = ARRAY_SIZE(depths);

    rendererTestCapabilities(&pvfb->root.legacyDrawing);
    xorgGlxCreateVendor();
    lorieInitClipboard();

    if (-1 == AddScreen(lorieScreenInit, argc, argv)) {
        FatalError("Couldn't add screen\n");
    }
}

// This Present implementation mostly copies the one from `present/present_fake.c`
// The only difference is performing vblanks right before redrawing root window (in lorieRedraw) instead of using timers.
static RRCrtcPtr loriePresentGetCrtc(WindowPtr w) {
    return RRFirstEnabledCrtc(w->drawable.pScreen);
}

static int loriePresentGetUstMsc(__unused RRCrtcPtr crtc, uint64_t *ust, uint64_t *msc) {
    *ust = GetTimeInMicros();
    *msc = pvfb->current_msc;
    return Success;
}

static Bool loriePresentQueueVblank(__unused RRCrtcPtr crtc, uint64_t event_id, uint64_t msc) {
#pragma clang diagnostic push
#pragma ide diagnostic ignored "MemoryLeak" // it is not leaked, it is destroyed in lorieRedraw
    struct vblank* vblank = calloc (1, sizeof (*vblank));
    if (!vblank)
        return BadAlloc;

    *vblank = (struct vblank) { .id = event_id, .msc = msc };
    xorg_list_add(&vblank->link, &pvfb->vblank_queue);

    return Success;
#pragma clang diagnostic pop
}

static void loriePresentAbortVblank(__unused RRCrtcPtr crtc, uint64_t id, __unused uint64_t msc) {
    struct vblank *vblank, *tmp;

    xorg_list_for_each_entry_safe(vblank, tmp, &pvfb->vblank_queue, link) {
        if (vblank->id == id) {
            xorg_list_del(&vblank->link);
            free (vblank);
            break;
        }
    }
}

static void loriePerformVblanks(void) {
    struct vblank *vblank, *tmp;
    xorg_list_for_each_entry_safe(vblank, tmp, &pvfb->vblank_queue, link) {
        if (vblank->msc <= pvfb->current_msc) {
            present_event_notify(vblank->id, GetTimeInMicros(), pvfb->current_msc);
            xorg_list_del(&vblank->link);
            free (vblank);
        }
    }
}

Bool loriePresentFlip(__unused RRCrtcPtr crtc, __unused uint64_t event_id, __unused uint64_t target_msc, PixmapPtr pixmap, __unused Bool sync_flip) {
    LoriePixmapPriv* priv = (LoriePixmapPriv*) exaGetPixmapDriverPrivate(pixmap);
    if (!priv || !priv->buffer || priv->mem || pvfb->root.width != pixmap->drawable.width || pvfb->root.width != pixmap->drawable.height)
        return FALSE;

    const LorieBuffer_Desc *desc = LorieBuffer_description(priv->buffer);
    char *forceFlip = getenv("TERMUX_X11_FORCE_FLIP");
    if (desc->type == LORIEBUFFER_FD && priv->imported && !(forceFlip && strcmp(forceFlip, "1") == 0))
        return FALSE; // For some reason it does not work fine with turnip.

    if (desc->type == LORIEBUFFER_REGULAR) {
        // Regular buffers can not be shared to activity, we must explicitly convert LorieBuffer to FD or AHardwareBuffer
        int8_t type = pvfb->root.legacyDrawing ? LORIEBUFFER_FD : LORIEBUFFER_AHARDWAREBUFFER;
        LorieBuffer_convert(priv->buffer, type, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM);
        if (desc->type != LORIEBUFFER_REGULAR) {
            // LorieBuffer_convert does not report status but it does not let the type change in the case of error.
            pScreenPtr->ModifyPixmapHeader(pixmap, 0, 0, 0, 0, desc->stride * 4, NULL);
            LorieBuffer_lock(priv->buffer, &priv->locked);
        }
    }

    if (desc->type != LORIEBUFFER_FD && desc->type != LORIEBUFFER_AHARDWAREBUFFER)
        return FALSE;

    lorieRegisterBuffer(priv->buffer);
    return TRUE;
}

void loriePresentAfterFlip(__unused RRCrtcPtr crtc, uint64_t event_id, uint64_t ust, uint64_t target_msc, __unused PixmapPtr pixmap) {
    // X server was patched to call this function right after finishing all present_flip shenanigans
    // Since we do not invoke DRM API or anything similar we do not need to implement this as callback
    // For some reason calling present_event_notify in BlockHandler or as QueueWorkProc/eventfd callback
    // adds some delay which may be easily avoided this way.
    static BoxRec box = { 0, 0, 1, 1 }; // lorieRedraw only checks if it is empty or not.
    RegionReset(DamageRegion(pvfb->damage), &box);
    pvfb->current_msc = min(pvfb->current_msc + 1, target_msc);
    present_event_notify(event_id, ust, pvfb->current_msc);
}

void loriePresentUnflip(__unused ScreenPtr screen, uint64_t event_id) {
    present_event_notify(event_id, 0, 0);
}

static struct present_screen_info loriePresentInfo = {
        .get_crtc = loriePresentGetCrtc,
        .get_ust_msc = loriePresentGetUstMsc,
        .queue_vblank = loriePresentQueueVblank,
        .abort_vblank = loriePresentAbortVblank,
        // check_flip is called only in present_check_flip_window during window reconfiguration.
        // The function should tell if pixmap can be used for flipping window.
        // Since there are no other drivers involved here we assume it always fits.
        .check_flip = TrueNoop,
        .flip = loriePresentFlip,
        .after_flip = loriePresentAfterFlip,
        .unflip = loriePresentUnflip,
};

void exaDDXDriverInit(__unused ScreenPtr pScreen) {}

void *lorieCreatePixmap(__unused ScreenPtr pScreen, int width, int height, __unused int depth, int usage_hint, __unused int bpp, int *new_fb_pitch) {
    LoriePixmapPriv *priv;
    size_t size = sizeof(LoriePixmapPriv);
    *new_fb_pitch = 0;

    priv = calloc(1, size);
    if (!priv)
        return NULL;

    if (width == 0 || height == 0)
        return priv;

    uint8_t type = usage_hint != CREATE_PIXMAP_USAGE_LORIEBUFFER_BACKED ? LORIEBUFFER_REGULAR : pvfb->root.legacyDrawing ? LORIEBUFFER_FD : LORIEBUFFER_AHARDWAREBUFFER;
    priv->buffer = LorieBuffer_allocate(width, height, AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, type);
    *new_fb_pitch = LorieBuffer_description(priv->buffer)->stride * 4;

    LorieBuffer_lock(priv->buffer, &priv->locked);
    if (!priv->buffer) {
        free(priv);
        return NULL;
    }

    return priv;
}

void lorieExaDestroyPixmap(__unused ScreenPtr pScreen, void *driverPriv) {
    LoriePixmapPriv *priv = driverPriv;
    if (priv->buffer) {
        if (priv->locked)
            LorieBuffer_unlock(priv->buffer);
        lorieUnregisterBuffer(priv->buffer);
        LorieBuffer_release(priv->buffer);
    }
    free(priv);
}

Bool lorieModifyPixmapHeader(PixmapPtr pPix, __unused int w, __unused int h, __unused int depth, __unused int bitsPerbppPixel, __unused int devKind, __unused void *data) {
    LoriePixmapPriv *priv = exaGetPixmapDriverPrivate(pPix);
    if (priv && data)
        priv->mem = data;
    return FALSE;
}

Bool loriePrepareAccess(PixmapPtr pPix, int index) {
    LoriePixmapPriv *priv = exaGetPixmapDriverPrivate(pPix);
    if (index == EXA_PREPARE_DEST && pScreenPtr->GetScreenPixmap(pScreenPtr) == pPix)
        lorie_mutex_lock(&pvfb->state->lock, &pvfb->state->lockingPid);

    if (!priv->locked && !priv->mem) {
        int err = LorieBuffer_lock(priv->buffer, &priv->locked);
        if (err) {
            dprintf(2, "Failed to lock buffer, err %d\n", err);
            return FALSE;
        }
        priv->wasLocked = FALSE;
    } else
        priv->wasLocked = TRUE;

    pPix->devPrivate.ptr = priv->locked ?: priv->mem;
    return TRUE;
}

void lorieFinishAccess(PixmapPtr pPix, int index) {
    LoriePixmapPriv *priv = exaGetPixmapDriverPrivate(pPix);
    if (index == EXA_PREPARE_DEST && pScreenPtr->GetScreenPixmap(pScreenPtr) == pPix)
        lorie_mutex_unlock(&pvfb->state->lock, &pvfb->state->lockingPid);

    if (!priv->wasLocked) {
        LorieBuffer_unlock(priv->buffer);
        priv->locked = NULL;
        priv->wasLocked = FALSE;
    }
}

static ExaDriverRec lorieExa = {
        .exa_major = EXA_VERSION_MAJOR, .exa_minor = EXA_VERSION_MINOR, .maxX = 32767, .maxY = 32767,
        .flags = EXA_OFFSCREEN_PIXMAPS | EXA_HANDLES_PIXMAPS, .pixmapPitchAlign = 32,
        .PrepareSolid = FalseNoop, .PrepareCopy = FalseNoop, .PrepareComposite = FalseNoop,
        .PixmapIsOffscreen = TrueNoop, .WaitMarker = VoidNoop,
        .PrepareAccess = loriePrepareAccess, .FinishAccess = lorieFinishAccess,
        .CreatePixmap2 = lorieCreatePixmap, .DestroyPixmap = lorieExaDestroyPixmap,
        .ModifyPixmapHeader = lorieModifyPixmapHeader,
};

static PixmapPtr loriePixmapFromFds(ScreenPtr screen, CARD8 num_fds, const int *fds, CARD16 width, CARD16 height,
                                    const CARD32 *strides, const CARD32 *offsets, CARD8 depth, __unused CARD8 bpp, CARD64 modifier) {
#define fail(msg, ...) do { log(ERROR, msg, ##__VA_ARGS__); goto fail; } while(0)
#define check(cond, msg, ...) if ((cond)) fail(msg, ##__VA_ARGS__)
    const CARD64 AHARDWAREBUFFER_SOCKET_FD = 1255;
    const CARD64 AHARDWAREBUFFER_FLIPPED_SOCKET_FD = 1256;
    const CARD64 RAW_MMAPPABLE_FD = 1274;
    AHardwareBuffer_Desc desc = {0};
    PixmapPtr pixmap = NullPixmap;
    LoriePixmapPriv *priv = NULL;

    check(num_fds > 1, "DRI3: More than 1 fd");
    check(modifier != RAW_MMAPPABLE_FD && modifier != AHARDWAREBUFFER_SOCKET_FD && modifier != AHARDWAREBUFFER_FLIPPED_SOCKET_FD &&
          modifier != DRM_FORMAT_MOD_INVALID && modifier != DRM_FORMAT_MOD_LINEAR, "DRI3: Modifier is not RAW_MMAPPABLE_FD or AHARDWAREBUFFER_SOCKET_FD");

    pixmap = screen->CreatePixmap(screen, 0, 0, depth, 0);
    check(!pixmap, "DRI3: failed to create pixmap");

    priv = exaGetPixmapDriverPrivate(pixmap);
    check(!priv, "DRI3: failed to obtain pixmap private");

    priv->imported = true;

    if (modifier == DRM_FORMAT_MOD_INVALID || modifier == DRM_FORMAT_MOD_LINEAR || modifier == RAW_MMAPPABLE_FD) {
        check(!(priv->buffer = LorieBuffer_wrapFileDescriptor(width, strides[0]/4, height, AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM, fds[0], offsets[0])), "DRI3: LorieBuffer_wrapAHardwareBuffer failed.");
        screen->ModifyPixmapHeader(pixmap, width, height, 0, 0, strides[0], NULL);
        return pixmap;
    }

    if (modifier == AHARDWAREBUFFER_SOCKET_FD || modifier == AHARDWAREBUFFER_FLIPPED_SOCKET_FD) {
        AHardwareBuffer* buffer;
        struct stat info;
        uint8_t buf = 1;
        int r;

        priv->flipped = modifier == AHARDWAREBUFFER_FLIPPED_SOCKET_FD;
        check(fstat(fds[0], &info) != 0, "DRI3: fstat failed: %s", strerror(errno));
        check(!S_ISSOCK(info.st_mode), "DRI3: modifier is AHARDWAREBUFFER_SOCKET_FD but fd is not a socket");
        // Sending signal to other end of socket to send buffer.
        check(write(fds[0], &buf, 1) != 1, "DRI3: AHARDWAREBUFFER_SOCKET_FD: failed to write to socket: %s", strerror(errno));
        check((r = AHardwareBuffer_recvHandleFromUnixSocket(fds[0], &buffer)) != 0,
              "DRI3: AHARDWAREBUFFER_SOCKET_FD: failed to obtain AHardwareBuffer from socket: %d", r);
        check(!buffer, "DRI3: AHARDWAREBUFFER_SOCKET_FD: did not receive AHardwareSocket from buffer");
        AHardwareBuffer_describe(buffer, &desc);
        check(desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM
            && desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM
            && desc.format != AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM,
            "DRI3: AHARDWAREBUFFER_SOCKET_FD: wrong format of AHardwareBuffer. Must be one of: AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM (stands for 5).");
        check(!(priv->buffer = LorieBuffer_wrapAHardwareBuffer(buffer)), "DRI3: LorieBuffer_wrapAHardwareBuffer failed.");

        screen->ModifyPixmapHeader(pixmap, desc.width, desc.height, 0, 0, desc.stride * 4, NULL);
    }

    return pixmap;

    fail:
    if (pixmap)
        screen->DestroyPixmap(pixmap);

    return NULL;
}

static int lorieGetFormats(__unused ScreenPtr screen, CARD32 *num_formats, CARD32 **formats) {
    static CARD32 format = DRM_FORMAT_ARGB8888;
    *num_formats = 1;
    *formats = &format;
    return TRUE;
}

static int lorieGetModifiers(__unused ScreenPtr screen, uint32_t format, uint32_t *num_modifiers, uint64_t **modifiers) {
    static uint64_t modifier = DRM_FORMAT_MOD_LINEAR;

    if (format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_XRGB8888) {
        *num_modifiers = 0;
        *modifiers = NULL;
        return TRUE;
    }

    *num_modifiers = 1;
    *modifiers = &modifier;
    return TRUE;
}

static dri3_screen_info_rec lorieDri3Info = {
        .version = 2,
        .fds_from_pixmap = FalseNoop,
        .pixmap_from_fds = loriePixmapFromFds,
        .get_formats = lorieGetFormats,
        .get_modifiers = lorieGetModifiers,
        .get_drawable_modifiers = FalseNoop
};

static GLboolean drawableSwapBuffers(unused ClientPtr client, unused __GLXdrawable * drawable) { return TRUE; }
static void drawableCopySubBuffer(unused __GLXdrawable * basePrivate, unused int x, unused int y, unused int w, unused int h) {}
static __GLXdrawable * createDrawable(unused ClientPtr client, __GLXscreen * screen, DrawablePtr pDraw,
                                      unused XID drawId, int type, XID glxDrawId, __GLXconfig * glxConfig) {
    __GLXdrawable *private = calloc(1, sizeof *private);
    if (private == NULL)
        return NULL;

    if (!__glXDrawableInit(private, screen, pDraw, type, glxDrawId, glxConfig)) {
        free(private);
        return NULL;
    }

    private->destroy = (void (*)(__GLXdrawable *)) free;
    private->swapBuffers = drawableSwapBuffers;
    private->copySubBuffer = drawableCopySubBuffer;

    return private;
}

static void glXDRIscreenDestroy(__GLXscreen *baseScreen) {
    free(baseScreen->GLXextensions);
    free(baseScreen->GLextensions);
    free(baseScreen->visuals);
    free(baseScreen);
}

static __GLXscreen *glXDRIscreenProbe(ScreenPtr pScreen) {
    __GLXscreen *screen;

    screen = calloc(1, sizeof *screen);
    if (screen == NULL)
        return NULL;

    screen->destroy = glXDRIscreenDestroy;
    screen->createDrawable = createDrawable;
    screen->pScreen = pScreen;
    screen->fbconfigs = configs;
    screen->glvnd = "mesa";

    __glXInitExtensionEnableBits(screen->glx_enable_bits);
    /* There is no real GLX support, but anyways swrast reports it. */
    __glXEnableExtension(screen->glx_enable_bits, "GLX_MESA_copy_sub_buffer");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_no_config_context");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_create_context");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_create_context_no_error");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_create_context_profile");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_create_context_es_profile");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_create_context_es2_profile");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_framebuffer_sRGB");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_fbconfig_float");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_fbconfig_packed_float");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_texture_from_pixmap");
    __glXScreenInit(screen, pScreen);

    return screen;
}

__GLXprovider __glXDRISWRastProvider = {
        glXDRIscreenProbe,
        "DRISWRAST",
        NULL
};
