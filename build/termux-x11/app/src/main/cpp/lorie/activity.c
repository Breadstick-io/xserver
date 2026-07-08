#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <errno.h>
#include <jni.h>
#include <android/looper.h>
#include <wchar.h>
#include <linux/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <android/hardware_buffer_jni.h>
#include "lorie.h"
#include "buffer.h"

#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma ide diagnostic ignored "cppcoreguidelines-narrowing-conversions"
#pragma ide diagnostic ignored "ConstantFunctionResult"
#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "LorieNative", __VA_ARGS__)

// Breadstick runs the X server AND the renderer in ONE process (upstream uses two), so the
// renderer must NOT share the server's conn_fd: sharing makes connect_ overwrite it and
// close the server's socket end, breaking the resize/event channel. Give the renderer its
// own private fd — its end of the getXConnection socketpair.
static volatile int conn_fd = -1;

// Breadstick rootless: remember received buffers by id (read-loop thread only — no lock
// needed) so a window-state event can hand the matching AHardwareBuffer to Java.
#define LORIE_MAX_WIN_BUFFERS 256
static struct { uint64_t id; AHardwareBuffer* ahb; } lorieWinBufs[LORIE_MAX_WIN_BUFFERS];

static void lorieRememberBuffer(uint64_t id, AHardwareBuffer* ahb) {
    if (!id || !ahb) return;
    int slot = -1;
    for (int i = 0; i < LORIE_MAX_WIN_BUFFERS; i++) {
        if (lorieWinBufs[i].id == id) { lorieWinBufs[i].ahb = ahb; return; }
        if (slot < 0 && lorieWinBufs[i].ahb == NULL) slot = i;
    }
    if (slot >= 0) { lorieWinBufs[slot].id = id; lorieWinBufs[slot].ahb = ahb; }
    else log(ERROR, "lorieWinBufs full (%d entries) — dropping buffer %llu; its window will not render",
             LORIE_MAX_WIN_BUFFERS, (unsigned long long) id);
}

static void lorieForgetBuffer(uint64_t id) {
    for (int i = 0; i < LORIE_MAX_WIN_BUFFERS; i++)
        if (lorieWinBufs[i].id == id) { lorieWinBufs[i].id = 0; lorieWinBufs[i].ahb = NULL; return; }
}

static AHardwareBuffer* lorieFindBuffer(uint64_t id) {
    if (!id) return NULL;
    for (int i = 0; i < LORIE_MAX_WIN_BUFFERS; i++)
        if (lorieWinBufs[i].id == id) return lorieWinBufs[i].ahb;
    return NULL;
}

static struct {
    jclass self;
    jmethodID getInstance, clientConnectedStateChanged, resetIme;
} MainActivity = {0};

static struct {
    jclass self;
    jmethodID forName;
    jmethodID decode;
} Charset = {0};

static struct {
    jclass self;
    jmethodID toString;
} CharBuffer = {0};

static JNIEnv *guienv = NULL; // Must be used only in GUI thread.
static jobject globalThiz = NULL;

static jclass FindClassOrDie(JNIEnv *env, const char* name) {
    jclass clazz = (*env)->FindClass(env, name);
    if (!clazz) {
        char buffer[1024] = {0};
        sprintf(buffer, "class %s not found", name);
        log(ERROR, "%s", buffer);
        (*env)->FatalError(env, buffer);
        return NULL;
    }

    return (*env)->NewGlobalRef(env, clazz);
}

static jclass FindMethodOrDie(JNIEnv *env, jclass clazz, const char* name, const char* signature, jboolean isStatic) {
    __typeof__((*env)->GetMethodID) getMethodID = isStatic ? (*env)->GetStaticMethodID : (*env)->GetMethodID;
    jmethodID method = getMethodID(env, clazz, name, signature);
    if (!method) {
        char buffer[1024] = {0};
        sprintf(buffer, "method %s %s not found", name, signature);
        log(ERROR, "%s", buffer);
        (*env)->FatalError(env, buffer);
        return NULL;
    }

    return method;
}

static jboolean requestConnection(__unused JNIEnv *env, __unused jclass clazz) {
#define check(cond, fmt, ...) if ((cond)) do { __android_log_print(ANDROID_LOG_ERROR, "requestConnection", fmt, ## __VA_ARGS__); goto end; } while (0)
    bool sent = JNI_FALSE;
    // We do not want to block GUI thread for a long time so we will set timeout to 20 msec.
    struct sockaddr_in server = { .sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = inet_addr("127.0.0.1") };
    int so_error, sock = socket(AF_INET, SOCK_STREAM, 0);
    check(sock < 0, "Could not create socket: %s", strerror(errno));
    check(fcntl(sock, F_SETFL, O_NONBLOCK) < 0, "failed to set socket non-block: %s", strerror(errno));
    int r = connect(sock, (struct sockaddr *)&server, sizeof(server));
    check(r < 0 && errno != EINPROGRESS, "failed to connect socket: %s", strerror(errno));
    if (r < 0 && errno == EINPROGRESS) {
        // Connection is in progress; use poll to wait for it
        struct pollfd pfd = { .fd = sock, .events = POLLOUT };
        r = poll(&pfd, 1, 20);  // timeout set to 50ms
        if (!r) goto end;
        // check(!r, "Connection timed out after 20ms."); // We do not want to flood logcat with this message
        check(r < 0, "poll failed: %s", strerror(errno));
        socklen_t len = sizeof(so_error);
        check(getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0, "getsockopt failed: %s", strerror(errno));
        if (so_error == ECONNREFUSED) goto end; // Regular situation which happens often if server is not started. No need to spam logcat with this.
        check(so_error != 0, "Connection failed: %s", strerror(so_error));

        check(write(sock, MAGIC, sizeof(MAGIC)) < 0, "failed to send message: %s", strerror(errno));
        sent = JNI_TRUE;
        goto end;
    }

    check(1, "something went wrong: %s, %s", strerror(errno), strerror(r));

    end: if (sock >= 0) close(sock);
    return sent;
#undef errorReturn
}

static void connect_(__unused JNIEnv* env, __unused jobject cls, jint fd);
static void nativeInit(JNIEnv *env, jobject thiz) {
    JavaVM* vm;
    if (!Charset.self) {
        // Init clipboard-related JNI stuff
        Charset.self = FindClassOrDie(env, "java/nio/charset/Charset");
        Charset.forName = FindMethodOrDie(env, Charset.self, "forName", "(Ljava/lang/String;)Ljava/nio/charset/Charset;", JNI_TRUE);
        Charset.decode = FindMethodOrDie(env, Charset.self, "decode", "(Ljava/nio/ByteBuffer;)Ljava/nio/CharBuffer;", JNI_FALSE);

        CharBuffer.self = FindClassOrDie(env,  "java/nio/CharBuffer");
        CharBuffer.toString = FindMethodOrDie(env, CharBuffer.self, "toString", "()Ljava/lang/String;", JNI_FALSE);

        MainActivity.self = FindClassOrDie(env,  "io/breadstick/x11/MainActivity");
        MainActivity.getInstance = FindMethodOrDie(env, MainActivity.self, "getInstance", "()Lio/breadstick/x11/MainActivity;", JNI_TRUE);
        MainActivity.clientConnectedStateChanged = FindMethodOrDie(env, MainActivity.self, "clientConnectedStateChanged", "()V", JNI_FALSE);
        MainActivity.resetIme = FindMethodOrDie(env, (*env)->GetObjectClass(env, thiz), "resetIme", "()V", JNI_FALSE);
    }

    rendererInit(env);

    (*env)->GetJavaVM(env, &vm);
    (*vm)->AttachCurrentThread(vm, &guienv, NULL);
    globalThiz = (*guienv)->NewGlobalRef(env, thiz);
    connect_(NULL, NULL, -1);
}

static int xcallback(int fd, int events, __unused void* data) {
    JNIEnv *env = guienv;
    jobject thiz = globalThiz;

    if (events & (ALOOPER_EVENT_ERROR | ALOOPER_EVENT_HANGUP)) {
        jobject instance = (*env)->CallStaticObjectMethod(env, MainActivity.self, MainActivity.getInstance);
        if (instance)
            (*env)->CallVoidMethod(env, instance, MainActivity.clientConnectedStateChanged);

        ALooper_removeFd(ALooper_forThread(), fd);
        close(conn_fd);
        conn_fd = -1;
        rendererSetSharedState(NULL);
        rendererRemoveAllBuffers();
        log(DEBUG, "disconnected");
        return 1;
    }

    if (conn_fd != -1) {
        lorieEvent e = {0};

        again:
        if (read(conn_fd, &e, sizeof(e)) == sizeof(e)) {
            switch(e.type) {
                case EVENT_CLIPBOARD_SEND: {
                    if (!e.clipboardSend.count)
                        break;
                    char clipboard[e.clipboardSend.count + 1];
                    memset(clipboard, 0, e.clipboardSend.count + 1);
                    read(conn_fd, clipboard, sizeof(clipboard));
                    clipboard[e.clipboardSend.count] = 0;
                    log(DEBUG, "Clipboard content (%zu symbols) is %s", strlen(clipboard), clipboard);
                    jmethodID id = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, thiz), "setClipboardText","(Ljava/lang/String;)V");
                    jobject bb = (*env)->NewDirectByteBuffer(env, clipboard, strlen(clipboard));
                    jobject charset = (*env)->CallStaticObjectMethod(env, Charset.self, Charset.forName, (*env)->NewStringUTF(env, "UTF-8"));
                    jobject cb = (*env)->CallObjectMethod(env, charset, Charset.decode, bb);
                    (*env)->DeleteLocalRef(env, bb);

                    jstring str = (*env)->CallObjectMethod(env, cb, CharBuffer.toString);
                    (*env)->CallVoidMethod(env, thiz, id, str);
                    break;
                }
                case EVENT_CLIPBOARD_REQUEST: {
                    (*env)->CallVoidMethod(env, thiz, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, thiz), "requestClipboard", "()V"));
                    break;
                }
                case EVENT_SHARED_SERVER_STATE: {
                    struct lorie_shared_server_state* state = NULL;
                    int stateFd = ancil_recv_fd(conn_fd);

                    if (stateFd < 0)
                        break;

                    state = mmap(NULL, sizeof(*state), PROT_READ|PROT_WRITE, MAP_SHARED, stateFd, 0);
                    if (!state || state == MAP_FAILED) {
                        log(ERROR, "Failed to map server state: %s", strerror(errno));
                        state = NULL;
                    }

                    rendererSetSharedState(state);

                    close(stateFd); // Closing file descriptor does not unmmap shared memory fragment.
                    break;
                }
                case EVENT_ADD_BUFFER: {
                    static LorieBuffer* buffer = NULL;
                    const LorieBuffer_Desc* desc;
                    LorieBuffer_recvHandleFromUnixSocket(conn_fd, &buffer);
                    desc = LorieBuffer_description(buffer);
                    log(INFO, "Received shared buffer width %d stride %d height %d format %d type %d id %llu", desc->width, desc->stride, desc->height, desc->format, desc->type, desc->id);
                    rendererAddBuffer(buffer);
                    lorieRememberBuffer(desc->id, desc->buffer); // rootless: window pixels for Java
                    break;
                }
                case EVENT_REMOVE_BUFFER: {
                    lorieForgetBuffer(e.removeBuffer.id);
                    rendererRemoveBuffer(e.removeBuffer.id);
                    break;
                }
                case EVENT_WINDOW_FOCUS_CHANGED: {
                    (*env)->CallVoidMethod(env, thiz, MainActivity.resetIme);
                    break;
                }
                case EVENT_WINDOW_STATE: {
                    // Geometry / mapped state — drives the live window list.
                    jmethodID ms = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, thiz), "onWindowState", "(IIIIIZZ)V");
                    if (ms)
                        (*env)->CallVoidMethod(env, thiz, ms, (jint) e.windowState.window, (jint) e.windowState.x,
                                               (jint) e.windowState.y, (jint) e.windowState.width,
                                               (jint) e.windowState.height, (jboolean) e.windowState.mapped,
                                               (jboolean) e.windowState.popup);
                    // Pixels — hand the window's AHardwareBuffer to Java as a HardwareBuffer.
                    if (e.windowState.mapped && e.windowState.buffer) {
                        AHardwareBuffer* ahb = lorieFindBuffer(e.windowState.buffer);
                        if (ahb) {
                            jobject hb = AHardwareBuffer_toHardwareBuffer(env, ahb);
                            if (hb) {
                                jmethodID mb = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, thiz),
                                    "onWindowBuffer", "(ILandroid/hardware/HardwareBuffer;IIII)V");
                                if (mb)
                                    (*env)->CallVoidMethod(env, thiz, mb, (jint) e.windowState.window, hb,
                                                           (jint) e.windowState.x, (jint) e.windowState.y,
                                                           (jint) e.windowState.width, (jint) e.windowState.height);
                                (*env)->DeleteLocalRef(env, hb);
                            }
                        }
                    }
                    break;
                }
            }
        }

        int n;
        if (ioctl(conn_fd, FIONREAD, &n) >= 0 && n > sizeof(e))
            goto again;
    }

    return 1;
}

static void connect_(__unused JNIEnv* env, __unused jobject cls, jint fd) {
    if (conn_fd != -1) {
        ALooper_removeFd(ALooper_forThread(), conn_fd);
        close(conn_fd);
        rendererSetSharedState(NULL);
        rendererRemoveAllBuffers();
        log(DEBUG, "disconnected");
    }

    if ((conn_fd = fd) != -1) {
        ALooper_addFd(ALooper_forThread(), fd, 0, ALOOPER_EVENT_INPUT | ALOOPER_EVENT_ERROR | ALOOPER_EVENT_HANGUP, xcallback, NULL);
        log(DEBUG, "XCB connection is successfull");
    }
}

static jboolean connected(__unused JNIEnv* env,__unused jclass clazz) {
    return conn_fd != -1;
}

static void startLogcat(JNIEnv *env, __unused jobject cls, jint fd) {
    log(DEBUG, "Starting logcat with output to given fd");

    switch(fork()) {
        case -1:
            log(ERROR, "fork: %s", strerror(errno));
            return;
        case 0:
            dup2(fd, 1);
            dup2(fd, 2);
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            char buf[64] = {0};
            sprintf(buf, "--pid=%d", getppid());
            execl("/system/bin/logcat", "logcat", buf, NULL);
            log(ERROR, "exec logcat: %s", strerror(errno));
            (*env)->FatalError(env, "Exiting");
    }
}

static void setClipboardSyncEnabled(__unused JNIEnv* env, __unused jobject cls, jboolean enable, __unused jboolean ignored) {
    if (conn_fd != -1) {
        lorieEvent e = { .clipboardEnable = { .t = EVENT_CLIPBOARD_ENABLE, .enable = enable } };
        write(conn_fd, &e, sizeof(e));
    }
}

static void sendClipboardAnnounce(__unused JNIEnv *env, __unused jobject thiz) {
    if (conn_fd != -1) {
        lorieEvent e = { .type = EVENT_CLIPBOARD_ANNOUNCE };
        write(conn_fd, &e, sizeof(e));
    }
}

static void sendClipboardEvent(JNIEnv *env, __unused jobject thiz, jbyteArray text) {
    if (conn_fd != -1 && text) {
        jsize length = (*env)->GetArrayLength(env, text);
        jbyte* str = (*env)->GetByteArrayElements(env, text, NULL);
        lorieEvent e = { .clipboardSend = { .t = EVENT_CLIPBOARD_SEND, .count = length } };
        write(conn_fd, &e, sizeof(e));
        write(conn_fd, str, length);
        (*env)->ReleaseByteArrayElements(env, text, str, JNI_ABORT);
    }
}

static void sendWindowChange(__unused JNIEnv* env, __unused jobject cls, jint width, jint height, jint framerate, jstring jname) {
    if (conn_fd != -1) {
        const char *name = (!jname || width <= 0 || height <= 0) ? NULL : (*env)->GetStringUTFChars(env, jname, JNI_FALSE);
        lorieEvent e = { .screenSize = { .t = EVENT_SCREEN_SIZE, .width = width, .height = height, .framerate = framerate, .name_size = (name ? strlen(name) : 0) } };
        write(conn_fd, &e, sizeof(e));
        if (name) {
            write(conn_fd, name, strlen(name));
            (*env)->ReleaseStringUTFChars(env, jname, name);
        }
    }
}

static void sendMouseEvent(__unused JNIEnv* env, __unused jobject cls, jfloat x, jfloat y, jint which_button, jboolean button_down, jboolean relative) {
    if (conn_fd != -1) {
        if (which_button > 0)
            (*env)->CallVoidMethod(env, globalThiz, MainActivity.resetIme);
        lorieEvent e = { .mouse = { .t = EVENT_MOUSE, .x = x, .y = y, .detail = which_button, .down = button_down, .relative = relative } };
        write(conn_fd, &e, sizeof(e));
    }
}

static void sendTouchEvent(__unused JNIEnv* env, __unused jobject cls, jint action, jint id, jint x, jint y) {
    if (conn_fd != -1 && action != -1) {
        lorieEvent e = { .touch = { .t = EVENT_TOUCH, .type = action, .id = id, .x = x, .y = y } };
        write(conn_fd, &e, sizeof(e));
    }
}

static void sendStylusEvent(__unused JNIEnv *env, __unused jobject thiz, jfloat x, jfloat y,
                            jint pressure, jint tilt_x, jint tilt_y,
                            jint orientation, jint buttons, jboolean eraser, jboolean mouse) {
    if (conn_fd != -1) {
        (*env)->CallVoidMethod(env, globalThiz, MainActivity.resetIme);
        lorieEvent e = { .stylus = { .t = EVENT_STYLUS, .x = x, .y = y, .pressure = pressure, .tilt_x = tilt_x, .tilt_y = tilt_y, .orientation = orientation, .buttons = buttons, .eraser = eraser, .mouse = mouse } };
        write(conn_fd, &e, sizeof(e));
    }
}

static void requestStylusEnabled(__unused JNIEnv *env, __unused jclass clazz, jboolean enabled) {
    if (conn_fd != -1) {
        lorieEvent e = { .stylusEnable = { .t = EVENT_STYLUS_ENABLE, .enable = enabled } };
        write(conn_fd, &e, sizeof(e));
    }
}

static jboolean sendKeyEvent(__unused JNIEnv* env, __unused jobject cls, jint scan_code, jint key_code, jboolean key_down) {
    if (conn_fd != -1) {
        int code = (scan_code) ?: android_to_linux_keycode[key_code];
        log(DEBUG, "Sending key: %d (%d %d %d)", code + 8, scan_code, key_code, key_down);
        lorieEvent e = { .key = { .t = EVENT_KEY, .key = code + 8, .state = key_down } };
        write(conn_fd, &e, sizeof(e));
    }

    return true;
}

static void sendTextEvent(JNIEnv *env, __unused jobject thiz, jbyteArray text) {
    if (conn_fd != -1 && text) {
        jsize length = (*env)->GetArrayLength(env, text);
        jbyte *str = (*env)->GetByteArrayElements(env, text, NULL);
        char *p = (char*) str;
        mbstate_t mbstate = { 0 };
        if (!length)
            return;

        log(DEBUG, "Parsing text: %.*s", length, str);

        while (*p) {
            wchar_t wc;
            size_t len = mbrtowc(&wc, p, MB_CUR_MAX, &mbstate);

            if (len == (size_t)-1 || len == (size_t)-2) {
                log(ERROR, "Invalid UTF-8 sequence encountered");
                break;
            }

            if (len == 0)
                break;

            log(DEBUG, "Sending unicode event: %lc (U+%X)", wc, wc);
            lorieEvent e = { .unicode = { .t = EVENT_UNICODE, .code = wc } };
            write(conn_fd, &e, sizeof(e));
            p += len;
            if (p - (char*) str >= length)
                break;
            usleep(2500);
        }

        (*env)->ReleaseByteArrayElements(env, text, str, JNI_ABORT);
    }
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, __unused void *reserved) {
    JNIEnv* env;
    static JNINativeMethod methods[] = {
            {"nativeInit", "()V", (void *)&nativeInit},
            {"surfaceChanged", "(Landroid/view/Surface;)V", (void *)&rendererSetWindow},
            {"setViewport", "(IIIIII)V", (void *)&rendererSetViewport},
            {"setRendererZoom", "(I)V", (void *)&rendererSetZoom},
            {"setFiltering", "(I)V", (void *)&rendererSetFiltering},
            {"connect", "(I)V", (void *)&connect_},
            {"connected", "()Z", (void *)&connected},
            {"startLogcat", "(I)V", (void *)&startLogcat},
            {"setClipboardSyncEnabled", "(ZZ)V", (void *)&setClipboardSyncEnabled},
            {"sendClipboardAnnounce", "()V", (void *)&sendClipboardAnnounce},
            {"sendClipboardEvent", "([B)V", (void *)&sendClipboardEvent},
            {"sendWindowChange", "(IIILjava/lang/String;)V", (void *)&sendWindowChange},
            {"sendMouseEvent", "(FFIZZ)V", (void *)&sendMouseEvent},
            {"sendTouchEvent", "(IIII)V", (void *)&sendTouchEvent},
            {"sendStylusEvent", "(FFIIIIIZZ)V", (void *)&sendStylusEvent},
            {"requestStylusEnabled", "(Z)V", (void *)&requestStylusEnabled},
            {"sendKeyEvent", "(IIZI)Z", (void *)&sendKeyEvent},
            {"sendTextEvent", "([B)V", (void *)&sendTextEvent},
            {"requestConnection", "()Z", (void *)&requestConnection},
    };
    (*vm)->AttachCurrentThread(vm, &env, NULL);
    jclass cls = (*env)->FindClass(env, "io/breadstick/x11/LorieView");
    (*env)->RegisterNatives(env, cls, methods, sizeof(methods)/sizeof(methods[0]));

    return JNI_VERSION_1_6;
}


// It is needed to redirect stderr to logcat
static void* stderrToLogcatThread(__unused void* cookie) {
    FILE *fp, *logf = NULL;
    int p[2];
    size_t len;
    char *line = NULL;
    const char *logpath = getenv("XLORIE_LOG_FILE");
    if (logpath)
        logf = fopen(logpath, "a");
    pipe(p);

    fp = fdopen(p[0], "r");

    dup2(p[1], 2);
    dup2(p[1], 1);
    while ((getline(&line, &len, fp)) != -1) {
        log(DEBUG, "%s%s", line, (line[len - 1] == '\n') ? "" : "\n");
        if (logf) {
            fputs(line, logf);
            fflush(logf);
        }
    }

    return NULL;
}

extern char* __progname;
__attribute__((constructor)) static void init(void) {
    pthread_t t;
    // Tee the X server's stdout/stderr to logcat and (when set) to XLORIE_LOG_FILE, which
    // survives a crash so Breadstick can show why the server died.
    if (!strcmp(__progname, "io.breadstick") || getenv("XLORIE_LOG_FILE"))
        pthread_create(&t, NULL, stderrToLogcatThread, NULL);
}
