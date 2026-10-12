// xrcamd - exposes every physical camera of the Samsung Galaxy XR as MJPEG
// streams over HTTP. Frames are pulled from Qualcomm's QVR camera service via
// libqvrcamera_client.qti.so (multi-client, does not disturb system tracking).
//
// Endpoints:
//   /                      HTML grid of all streams
//   /api/cameras           JSON description of streams
//   /stream/<id>           multipart/x-mixed-replace MJPEG (?q=quality&fps=max&scale=n)
//   /snapshot/<id>         single JPEG
//   /api/exposure?dev=<qvrname>&ns=<exposure ns>&gain=<gain>   manual exposure
#include <android/bitmap.h>
#include <android/data_space.h>
#include <android/hardware_buffer.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "gpu.h"
#include "qvr.h"

#define LOG(...) do { struct timespec _t; clock_gettime(CLOCK_REALTIME, &_t); \
    fprintf(stderr, "%ld.%03ld ", (long)_t.tv_sec, _t.tv_nsec / 1000000); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

static int g_port = 8080;
static int g_idle_ms = 5000;
static int g_rgb_scale = 2;  // default downscale for the 3000x3000 -> 1500x1500 rgb sub-frames
static int g_quality = 80;

/* ------------------------------------------------------------------ QVR */
static qvr_iface_t *CI, *DI;
#define COP(i) (CI->ops[i])
#define DOP(i) (DI->ops[i])

typedef void *(*fn_create)(void);
typedef void (*fn_destroy)(void *);
typedef void *(*fn_attach)(void *, const char *);
typedef void (*fn_detach)(void *);
typedef int (*fn_state)(void *, int *);
typedef int (*fn_v)(void *);
typedef int (*fn_curfn)(void *, int *);
typedef int (*fn_getframe)(void *, int *, int, int, void *);
typedef int (*fn_release)(void *, int);
typedef int (*fn_tohwb)(void *, void *, uint32_t *, qvr_hwbuf_t **);
typedef int (*fn_setexp)(void *, uint64_t, int);
typedef int (*fn_getparam)(void *, const char *, uint32_t *, char *);

static int qvr_load(void) {
    void *lib = dlopen("libqvrcamera_client.qti.so", RTLD_NOW);
    if (!lib) lib = dlopen("/vendor/lib64/libqvrcamera_client.qti.so", RTLD_NOW);
    if (!lib) { LOG("dlopen libqvrcamera_client failed: %s", dlerror()); return -1; }
    qvr_iface_t *(*gci)(void) = dlsym(lib, "getQvrCameraClientInstance");
    qvr_iface_t *(*gdi)(void) = dlsym(lib, "getQvrCameraDeviceInstance");
    if (!gci || !gdi) { LOG("symbols missing"); return -1; }
    CI = gci(); DI = gdi();
    LOG("QVR camera client iface v%d, device iface v%d", CI->api_version, DI->api_version);
    return 0;
}

/* ------------------------------------------------------------ streams */
#define MAX_SUB 2
typedef struct device device_t;

typedef struct {
    char id[48];           // url id
    char sensor[32];       // kernel sensor name
    int  sensor_slot;      // CRM sensor index
    device_t *dev;
    int  sub;              // index into hwbuf array
    // latest encoded frame
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint8_t *jpg; size_t jpg_len; size_t jpg_cap;
    uint64_t seq;
    int width, height;     // output size
    int subscribers;
    double fps;
    int boost;             // digital gain for mono streams (viewing aid)
} stream_t;

struct device {
    const char *qvr;       // QVR camera name
    const char *desc;
    const char *master;    // QVR device that must be running for this one to deliver (sync master)
    int nsub;
    stream_t *streams[MAX_SUB];
    pthread_mutex_t mu;
    int refs;              // subscribers + dependents
    int running;
    int stop_req;
    uint64_t last_use_ms;
    pthread_t th;
    // live handles
    void *client, *handle;
    // pending exposure request
    int exp_pending; uint64_t exp_ns; int exp_gain;
    char fmt[32];
};

static device_t DEVS[] = {
    {"rgb",                "Passthrough RGB stereo pair (Sony IMX564)", NULL, 2},
    {"ctrl-tracking",      "Tracking pair, upper (hand/controller, IR torch)", NULL, 2},
    {"tracking",           "Tracking pair (head tracking, synced to ctrl-tracking)", "ctrl-tracking", 2},
    {"face-tracking",      "Tracking pair, lower (hand-down/face, synced to ctrl-tracking)", "ctrl-tracking", 2},
    {"eye-tracking-left",  "Eye tracking left eye (2 cams)", NULL, 2},
    {"eye-tracking-right", "Eye tracking right eye (2 cams)", NULL, 2},
    {"d-depth",            "dToF depth sensor (Sony IMX611)", NULL, 1},
};
#define NDEV ((int)(sizeof DEVS / sizeof DEVS[0]))

// sub-camera ids (QVR camera_id lists from /vendor/etc/qvr/anorak_camera*.txt)
// and kernel sensor names from dmesg CAM_ACQUIRE_DEV[slot]
static const struct { const char *qvr; int sub; const char *id; int slot; const char *sensor; } SUBS[] = {
    {"rgb", 0, "rgb-left", 0, "imx564_vst_left"},
    {"rgb", 1, "rgb-right", 1, "imx564_vst_right"},
    {"tracking", 0, "tracking-left", 2, "s5k931_hat_ld"},
    {"tracking", 1, "tracking-right", 3, "s5k931_hat_rd"},
    {"face-tracking", 0, "face-tracking-left", 4, "s5k931_hat_lu"},
    {"face-tracking", 1, "face-tracking-right", 5, "s5k931_hat_ru"},
    {"ctrl-tracking", 0, "ctrl-tracking-left", 6, "s5k931_het_lu"},
    {"ctrl-tracking", 1, "ctrl-tracking-right", 7, "s5k931_het_ru"},
    {"eye-tracking-left", 0, "eye-left-a", 8, "s5k931_et_ld"},
    {"eye-tracking-left", 1, "eye-left-b", 10, "s5k931_et_lu"},
    {"eye-tracking-right", 0, "eye-right-a", 9, "s5k931_et_rd"},
    {"eye-tracking-right", 1, "eye-right-b", 11, "s5k931_et_ru"},
    {"d-depth", 0, "depth-tof", 12, "imx611_tof"},
};
#define NSUBS ((int)(sizeof SUBS / sizeof SUBS[0]))
static stream_t STREAMS[NSUBS];

static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static device_t *find_dev(const char *name) {
    for (int i = 0; i < NDEV; i++) if (!strcmp(DEVS[i].qvr, name)) return &DEVS[i];
    return NULL;
}
static stream_t *find_stream(const char *id) {
    for (int i = 0; i < NSUBS; i++) if (!strcmp(STREAMS[i].id, id)) return &STREAMS[i];
    return NULL;
}

// global "some stream published a frame" signal, used by the /mux endpoint
static pthread_mutex_t g_any_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_any_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_any_seq;

/* ------------------------------------------------------------ encode */
typedef struct { uint8_t *p; size_t n, cap; } buf_t;
static bool jpg_write(void *ctx, const void *data, size_t size) {
    buf_t *b = ctx;
    if (b->n + size > b->cap) {
        size_t nc = (b->n + size) * 2;
        uint8_t *np = realloc(b->p, nc);
        if (!np) return false;
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, data, size); b->n += size;
    return true;
}

static void publish(stream_t *s, const uint8_t *rgba, int w, int h, int q) {
    AndroidBitmapInfo info = {.width = (uint32_t)w, .height = (uint32_t)h, .stride = (uint32_t)w * 4,
                              .format = ANDROID_BITMAP_FORMAT_RGBA_8888,
                              .flags = ANDROID_BITMAP_FLAGS_ALPHA_OPAQUE};
    buf_t b = {0};
    int r = AndroidBitmap_compress(&info, ADATASPACE_SRGB, rgba, ANDROID_BITMAP_COMPRESS_FORMAT_JPEG, q, &b, jpg_write);
    if (r != ANDROID_BITMAP_RESULT_SUCCESS) { free(b.p); LOG("compress failed %d", r); return; }
    pthread_mutex_lock(&s->mu);
    free(s->jpg);
    s->jpg = b.p; s->jpg_len = b.n;
    s->width = w; s->height = h;
    s->seq++;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
    pthread_mutex_lock(&g_any_mu);
    g_any_seq++;
    pthread_cond_broadcast(&g_any_cv);
    pthread_mutex_unlock(&g_any_mu);
}

static uint8_t turbo(float x, int c) {  // cheap colormap for depth
    if (x < 0) x = 0; if (x > 1) x = 1;
    float r = 1.5f - 4.0f * (x > 0.75f ? x - 0.75f : 0.75f - x) * 1.0f;
    float g = 1.5f - 4.0f * (x > 0.5f ? x - 0.5f : 0.5f - x);
    float bl = 1.5f - 4.0f * (x > 0.25f ? x - 0.25f : 0.25f - x);
    float v = c == 0 ? r : c == 1 ? g : bl;
    if (v < 0) v = 0; if (v > 1) v = 1;
    return (uint8_t)(v * 255);
}

/* ------------------------------------------------------------ device worker */
static int dev_any_subscribers(device_t *d) {
    int n = 0;
    for (int i = 0; i < d->nsub; i++) if (d->streams[i]) n += d->streams[i]->subscribers;
    return n;
}

static void dev_acquire(device_t *d);
static void dev_release(device_t *d);

static void dev_session(device_t *d);
static void *dev_thread(void *arg) {
    device_t *d = arg;
    char tn[16]; snprintf(tn, sizeof tn, "xrcam-%.9s", d->qvr); pthread_setname_np(pthread_self(), tn);
    for (;;) {
        dev_session(d);
        pthread_mutex_lock(&d->mu);
        if (d->refs > 0) {  // re-acquired while shutting down
            d->stop_req = 0;
            pthread_mutex_unlock(&d->mu);
            usleep(200000);
            continue;
        }
        d->running = 0;
        pthread_mutex_unlock(&d->mu);
        return NULL;
    }
}

static void dev_session(device_t *d) {
    device_t *master = d->master ? find_dev(d->master) : NULL;
    if (master) dev_acquire(master);
    gpu_t gpu; int gpu_ok = 0;
    uint8_t *rgba = NULL; size_t rgba_cap = 0;
    LOG("[%s] starting", d->qvr);
    d->client = ((fn_create)COP(QC_CREATE))();
    if (!d->client) { LOG("[%s] client create failed", d->qvr); goto out; }
    d->handle = ((fn_attach)COP(QC_ATTACH))(d->client, d->qvr);
    if (!d->handle) { LOG("[%s] attach failed", d->qvr); goto out; }
    int r = ((fn_v)DOP(QD_START))(d->handle);
    if (r) { LOG("[%s] start failed %d", d->qvr, r); goto out; }
    {
        uint32_t len = sizeof d->fmt; memset(d->fmt, 0, sizeof d->fmt);
        ((fn_getparam)DOP(QD_GET_PARAM))(d->handle, "qvrcamdevice_frame_format", &len, d->fmt);
    }
    LOG("[%s] started, format %s", d->qvr, d->fmt);
    int fn = -1, errs = 0;
    uint64_t t_last[MAX_SUB] = {0};
    uint64_t fps_t0 = now_ms(); int fps_n = 0;
    while (!d->stop_req) {
        int encode = dev_any_subscribers(d) > 0;
        if (d->exp_pending) {
            d->exp_pending = 0;
            r = ((fn_setexp)DOP(QD_SET_EXP_GAIN))(d->handle, d->exp_ns, d->exp_gain);
            LOG("[%s] set exposure %llu ns gain %d -> %d", d->qvr, (unsigned long long)d->exp_ns, d->exp_gain, r);
        }
        if (fn < 0) {
            int cur = -1;
            r = ((fn_curfn)DOP(QD_CUR_FN))(d->handle, &cur);
            if (r || cur < 0) { usleep(20000); if (++errs % 250 == 0) LOG("[%s] waiting for frames (%d)", d->qvr, r); continue; }
            fn = cur;
        }
        qvr_frame_t fr; memset(&fr, 0, sizeof fr);
        int want = fn;
        r = ((fn_getframe)DOP(QD_GET_FRAME))(d->handle, &want, 0 /*blocking*/, 0, &fr);
        if (r == QVR_CAM_DROPPED_FRAMENUMBER || r == QVR_CAM_EXPIRED_FRAMENUMBER) { fn = -1; continue; }
        if (r) { if (++errs % 100 == 1) LOG("[%s] GetFrame(%d) -> %d", d->qvr, fn, r); fn = -1; usleep(10000); continue; }
        fn = (int)fr.fn + 1;
        if (!encode) { ((fn_release)DOP(QD_RELEASE_FRAME))(d->handle, fr.fn); continue; }
        uint32_t n = 0; qvr_hwbuf_t *hw = NULL;
        r = ((fn_tohwb)DOP(QD_FRAME_TO_HWB))(d->handle, &fr, &n, &hw);
        if (r || !hw) { ((fn_release)DOP(QD_RELEASE_FRAME))(d->handle, fr.fn); if (++errs % 100 == 1) LOG("[%s] FrameToHardwareBuffer -> %d", d->qvr, r); continue; }
        for (uint32_t i = 0; i < n && (int)i < d->nsub; i++) {
            stream_t *s = d->streams[i];
            if (!s || !s->subscribers) continue;
            uint64_t tnow = now_ms();
            t_last[i] = tnow;
            AHardwareBuffer *a = hw[i].ahb;
            AHardwareBuffer_Desc de; memset(&de, 0, sizeof de);
            AHardwareBuffer_describe(a, &de);
            int w = de.width, h = de.height;
            if (!a || !w || !h || !de.format) continue;  // first frames after start can carry empty buffers
            if (de.format == 0x123 /*QTI RAW8 (mono y8)*/ || de.format == 0x20203859 /*Y8*/ ||
                de.format == AHARDWAREBUFFER_FORMAT_BLOB) {
                void *p = NULL;
                if (AHardwareBuffer_lock(a, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &p) || !p) continue;
                size_t need = (size_t)w * h * 4;
                if (need > rgba_cap) { free(rgba); rgba = malloc(need); rgba_cap = need; }
                const uint8_t *src = p;
                for (int y = 0; y < h; y++) {
                    const uint8_t *row = src + (size_t)y * de.stride;
                    uint32_t *o = (uint32_t *)(rgba + (size_t)y * w * 4);
                    if (s->boost > 1) {
                        for (int x = 0; x < w; x++) { uint32_t v = row[x] * (uint32_t)s->boost; if (v > 255) v = 255; o[x] = 0xff000000u | v << 16 | v << 8 | v; }
                    } else
                        for (int x = 0; x < w; x++) { uint32_t v = row[x]; o[x] = 0xff000000u | v << 16 | v << 8 | v; }
                }
                AHardwareBuffer_unlock(a, NULL);
                publish(s, rgba, w, h, g_quality);
            } else if (de.format == 0x20363159 /*Y16*/ || de.format == AHARDWAREBUFFER_FORMAT_D16_UNORM) {
                void *p = NULL;
                if (AHardwareBuffer_lock(a, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &p) || !p) continue;
                // IMX611 dToF raw output: int16 samples, tightly packed with row
                // length = width (AHB stride is ignored), height/width square
                // planes (24x120 -> 5 planes of 24x24). Each plane is normalised
                // separately, colour-mapped and laid out side by side.
                const int S = 8;
                int pw = w, ph = w, np = h / w;
                if (np < 1) { np = 1; ph = h; }
                int ow = pw * S * np + (np - 1) * 4, oh = ph * S;
                size_t need = (size_t)ow * oh * 4;
                if (need > rgba_cap) { free(rgba); rgba = malloc(need); rgba_cap = need; }
                memset(rgba, 0x20, need);
                const int16_t *src = p;
                for (int k = 0; k < np; k++) {
                    const int16_t *pl = src + (size_t)k * pw * ph;
                    int mn = 32767, mx = -32768;
                    for (int q = 0; q < pw * ph; q++) if (pl[q]) { if (pl[q] < mn) mn = pl[q]; if (pl[q] > mx) mx = pl[q]; }
                    if (mx <= mn) mx = mn + 1;
                    for (int y = 0; y < oh; y++) {
                        uint32_t *o = (uint32_t *)(rgba + (size_t)y * ow * 4) + k * (pw * S + 4);
                        for (int x = 0; x < pw * S; x++) {
                            int v = pl[(y / S) * pw + x / S];
                            if (!v) { o[x] = 0xff000000u; continue; }
                            float f = (float)(v - mn) / (mx - mn);
                            o[x] = 0xff000000u | turbo(f, 2) << 16 | turbo(f, 1) << 8 | turbo(f, 0);
                        }
                    }
                }
                AHardwareBuffer_unlock(a, NULL);
                publish(s, rgba, ow, oh, g_quality);
            } else {
                // YUV / UBWC: let the GPU decode it
                if (!gpu_ok) {
                    LOG("[%s] sub %u: AHB fmt 0x%x %ux%u stride %u -> GPU path", d->qvr, i, de.format, w, h, de.stride);
                    if (gpu_init(&gpu)) { LOG("[%s] GPU init failed", d->qvr); gpu_ok = -1; }
                    else gpu_ok = 1;
                }
                if (gpu_ok != 1) continue;
                int sc = g_rgb_scale < 1 ? 1 : g_rgb_scale;
                int ow = w / sc, oh = h / sc;
                size_t need = (size_t)ow * oh * 4;
                if (need > rgba_cap) { free(rgba); rgba = malloc(need); rgba_cap = need; }
                r = gpu_convert(&gpu, a, ow, oh, rgba);
                if (r) { if (++errs % 100 == 1) LOG("[%s] gpu_convert %d (fmt 0x%x)", d->qvr, r, de.format); continue; }
                publish(s, rgba, ow, oh, g_quality);
            }
        }
        ((fn_release)DOP(QD_RELEASE_FRAME))(d->handle, fr.fn);
        fps_n++;
        uint64_t t = now_ms();
        if (t - fps_t0 >= 2000) {
            double f = fps_n * 1000.0 / (t - fps_t0);
            for (int i = 0; i < d->nsub; i++) if (d->streams[i]) d->streams[i]->fps = f;
            fps_t0 = t; fps_n = 0;
        }
    }
out:
    if (d->handle) {
        ((fn_v)DOP(QD_STOP))(d->handle);
        ((fn_detach)DOP(QD_DETACH))(d->handle);
        d->handle = NULL;
    }
    if (d->client) { ((fn_destroy)COP(QC_DESTROY))(d->client); d->client = NULL; }
    if (gpu_ok == 1) gpu_destroy(&gpu);
    free(rgba);
    if (master) dev_release(master);
    LOG("[%s] stopped", d->qvr);
}

static void dev_acquire(device_t *d) {
    pthread_mutex_lock(&d->mu);
    d->refs++;
    d->last_use_ms = now_ms();
    d->stop_req = 0;
    if (!d->running) {
        d->running = 1; d->stop_req = 0;
        pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_create(&d->th, &at, dev_thread, d);
    }
    pthread_mutex_unlock(&d->mu);
}
static void dev_release(device_t *d) {
    pthread_mutex_lock(&d->mu);
    d->refs--;
    d->last_use_ms = now_ms();
    pthread_mutex_unlock(&d->mu);
}

static void *reaper(void *arg) {
    (void)arg;
    for (;;) {
        usleep(500000);
        uint64_t t = now_ms();
        for (int i = 0; i < NDEV; i++) {
            device_t *d = &DEVS[i];
            pthread_mutex_lock(&d->mu);
            if (d->running && !d->stop_req && d->refs <= 0 && t - d->last_use_ms > (uint64_t)g_idle_ms) d->stop_req = 1;
            pthread_mutex_unlock(&d->mu);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ HTTP */
static int send_all(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        b += w; n -= (size_t)w;
    }
    return 0;
}
static int sendf(int fd, const char *fmt, ...) {
    char buf[4096]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    if (n < 0) return -1;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    return send_all(fd, buf, n);
}
static int qparam(const char *q, const char *key, char *out, size_t on) {
    if (!q) return 0;
    size_t kl = strlen(key);
    const char *p = q;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            p += kl + 1; size_t i = 0;
            while (*p && *p != '&' && i + 1 < on) out[i++] = *p++;
            out[i] = 0; return 1;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    return 0;
}

// The grid page receives ALL streams over a single /mux connection (browsers cap
// HTTP/1.1 at 6 connections per host, so 13 <img> MJPEG streams starve each
// other) and draws them on canvases, dropping frames when decoding falls behind.
static void stream_sub(stream_t *s, int d);
static const char INDEX_JS[] =
    "<script>\n"
    "const ids=[...document.querySelectorAll('canvas')].map(c=>c.dataset.id);\n"
    "const cv=[...document.querySelectorAll('canvas')], cx=cv.map(c=>c.getContext('2d'));\n"
    "const busy=new Array(ids.length).fill(false), cnt=new Array(ids.length).fill(0);\n"
    "const fpsEl=[...document.querySelectorAll('.fps')], sel=document.getElementById('fps');\n"
    "let ctl=null, gen=0;\n"
    "sel.value=localStorage.xrfps||'30';\n"
    "sel.onchange=()=>{localStorage.xrfps=sel.value;start();};\n"
    "function draw(i,jpg){ if(busy[i]) return; busy[i]=true;\n"
    "  createImageBitmap(new Blob([jpg],{type:'image/jpeg'})).then(b=>{\n"
    "    const c=cv[i]; if(c.width!==b.width||c.height!==b.height){c.width=b.width;c.height=b.height;}\n"
    "    cx[i].drawImage(b,0,0); b.close(); cnt[i]++; busy[i]=false; }).catch(()=>{busy[i]=false;}); }\n"
    "async function start(){ const my=++gen; if(ctl) ctl.abort(); ctl=new AbortController();\n"
    "  try{ const r=await fetch('/mux?ids='+ids.join(',')+'&fps='+sel.value,{signal:ctl.signal,cache:'no-store'});\n"
    "    const rd=r.body.getReader(); let buf=new Uint8Array(0);\n"
    "    for(;;){ const {done,value}=await rd.read(); if(done) break;\n"
    "      if(buf.length){const n=new Uint8Array(buf.length+value.length); n.set(buf); n.set(value,buf.length); buf=n;} else buf=value;\n"
    "      let o=0;\n"
    "      while(buf.length-o>=5){ const i=buf[o], len=buf[o+1]|buf[o+2]<<8|buf[o+3]<<16|buf[o+4]<<24;\n"
    "        if(buf.length-o-5<len) break;\n"
    "        if(i<ids.length&&len>0) draw(i, buf.slice(o+5,o+5+len));\n"
    "        o+=5+len; }\n"
    "      buf=buf.subarray(o); }\n"
    "  }catch(e){}\n"
    "  if(my===gen && !document.hidden) setTimeout(()=>{ if(my===gen) start(); },1000); }\n"
    "document.addEventListener('visibilitychange',()=>{ if(document.hidden){gen++; if(ctl) ctl.abort();} else start(); });\n"
    "setInterval(()=>{ fpsEl.forEach((e,i)=>{e.textContent=cnt[i]+' fps'; cnt[i]=0;}); },1000);\n"
    "start();\n"
    "</script>";

static void http_index(int fd) {
    sendf(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n");
    sendf(fd, "<!doctype html><html><head><meta name=viewport content='width=device-width'><title>Galaxy XR cameras</title>"
              "<style>body{background:#111;color:#ddd;font-family:sans-serif;margin:8px}"
              ".g{display:flex;flex-wrap:wrap;gap:8px}.c{background:#222;padding:6px;border-radius:6px}"
              ".c canvas{display:block;max-width:46vw;max-height:40vh;min-width:160px;min-height:80px;background:#000}"
              "a{color:#8cf}small{color:#999}.fps{color:#6c6;font-size:11px;margin-left:6px}</style></head><body>"
              "<h3>Samsung Galaxy XR - all physical cameras</h3>"
              "<p><small>Streams start on demand. Click a stream to open its full-rate MJPEG stream alone. "
              "<a href='/api/cameras'>JSON</a></small> &nbsp; Grid fps cap: <select id=fps>"
              "<option value=5>5</option><option value=15>15</option><option value=30>30</option>"
              "<option value=0>max</option></select></p><div class=g>");
    for (int i = 0; i < NSUBS; i++) {
        stream_t *s = &STREAMS[i];
        sendf(fd, "<div class=c><a href='/stream/%s' target=_blank><canvas data-id='%s'></canvas></a>"
                  "<b>%s</b><span class=fps></span><br><small>%s / slot %d / qvr '%s'[%d]</small></div>",
              s->id, s->id, s->id, s->sensor, s->sensor_slot, s->dev->qvr, s->sub);
    }
    sendf(fd, "</div>");
    send_all(fd, INDEX_JS, sizeof INDEX_JS - 1);
    sendf(fd, "</body></html>");
}

// /mux?ids=a,b,c&fps=N : one connection carrying several streams.
// Frame format: u8 index (position in ids), u32 LE length, JPEG bytes.
// index 255 with length 0 is a keepalive. Only the newest frame of each stream is
// sent; if the client reads slowly, intermediate frames are skipped (TCP backpressure).
static void http_mux(int fd, const char *q) {
    char ids[1024] = {0}, v[16];
    if (!qparam(q, "ids", ids, sizeof ids)) {
        sendf(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nusage: /mux?ids=a,b,c[&fps=N]\n");
        return;
    }
    int maxfps = qparam(q, "fps", v, sizeof v) ? atoi(v) : 0;
    stream_t *list[NSUBS]; int n = 0;
    for (char *save = NULL, *t = strtok_r(ids, ",", &save); t && n < NSUBS; t = strtok_r(NULL, ",", &save)) {
        stream_t *s = find_stream(t);
        list[n++] = s;  // keep positions stable even for unknown ids
    }
    for (int i = 0; i < n; i++) if (list[i]) { stream_sub(list[i], 1); dev_acquire(list[i]->dev); }
    sendf(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nCache-Control: no-cache, no-store\r\n"
              "Access-Control-Allow-Origin: *\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n");
    uint64_t seq[NSUBS] = {0}, last[NSUBS] = {0}, last_tx = now_ms();
    uint64_t min_iv = maxfps > 0 ? 1000 / maxfps : 0;
    int err = 0;
    while (!err) {
        int sent = 0;
        uint64_t t = now_ms();
        for (int i = 0; i < n && !err; i++) {
            stream_t *s = list[i];
            if (!s) continue;
            if (min_iv && t + 4 < last[i]) continue;  // last[] holds the next due time (4 ms jitter slack)
            pthread_mutex_lock(&s->mu);
            if (!s->jpg || s->seq == seq[i]) { pthread_mutex_unlock(&s->mu); continue; }
            size_t len = s->jpg_len;
            uint8_t *j = malloc(len + 5);
            memcpy(j + 5, s->jpg, len);
            seq[i] = s->seq;
            pthread_mutex_unlock(&s->mu);
            j[0] = (uint8_t)i; j[1] = len & 0xff; j[2] = (len >> 8) & 0xff; j[3] = (len >> 16) & 0xff; j[4] = (len >> 24) & 0xff;
            err = send_all(fd, j, len + 5);
            free(j);
            if (min_iv) last[i] = (t > last[i] + min_iv) ? t + min_iv : last[i] + min_iv;
            last_tx = now_ms(); sent = 1;
        }
        if (err) break;
        if (now_ms() - last_tx > 2000) {
            uint8_t ka[5] = {255, 0, 0, 0, 0};
            err = send_all(fd, ka, 5); last_tx = now_ms();
            continue;
        }
        if (!sent) {
            // wait for any new frame (or until a throttled stream becomes due)
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            long wait_ns = (min_iv ? (long)min_iv : 50) * 1000000L;
            ts.tv_nsec += wait_ns; while (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_mutex_lock(&g_any_mu);
            uint64_t s0 = g_any_seq;
            while (g_any_seq == s0) if (pthread_cond_timedwait(&g_any_cv, &g_any_mu, &ts) == ETIMEDOUT) break;
            pthread_mutex_unlock(&g_any_mu);
        }
    }
    for (int i = 0; i < n; i++) if (list[i]) { dev_release(list[i]->dev); stream_sub(list[i], -1); }
}

static void http_json(int fd) {
    sendf(fd, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n[");
    for (int i = 0; i < NSUBS; i++) {
        stream_t *s = &STREAMS[i];
        sendf(fd, "%s{\"id\":\"%s\",\"sensor\":\"%s\",\"slot\":%d,\"qvr_device\":\"%s\",\"qvr_sub\":%d,"
                  "\"description\":\"%s\",\"format\":\"%s\",\"running\":%s,\"width\":%d,\"height\":%d,\"fps\":%.1f,"
                  "\"subscribers\":%d,\"stream\":\"/stream/%s\",\"snapshot\":\"/snapshot/%s\"}",
              i ? "," : "", s->id, s->sensor, s->sensor_slot, s->dev->qvr, s->sub, s->dev->desc, s->dev->fmt,
              s->dev->running ? "true" : "false", s->width, s->height, s->fps, s->subscribers, s->id, s->id);
    }
    sendf(fd, "]\n");
}

static void stream_sub(stream_t *s, int d) {
    pthread_mutex_lock(&s->mu); s->subscribers += d; pthread_mutex_unlock(&s->mu);
}

// waits for a frame newer than *seq; returns copy (malloc) or NULL on timeout
static uint8_t *wait_frame(stream_t *s, uint64_t *seq, size_t *len, int timeout_ms) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000; ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&s->mu);
    while (s->seq == *seq || !s->jpg) {
        if (pthread_cond_timedwait(&s->cv, &s->mu, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&s->mu); return NULL; }
    }
    uint8_t *c = malloc(s->jpg_len);
    memcpy(c, s->jpg, s->jpg_len); *len = s->jpg_len; *seq = s->seq;
    pthread_mutex_unlock(&s->mu);
    return c;
}

static void http_stream(int fd, stream_t *s, const char *q, int snapshot) {
    char v[16]; int maxfps = 0;
    if (qparam(q, "fps", v, sizeof v)) maxfps = atoi(v);
    stream_sub(s, 1);
    dev_acquire(s->dev);
    uint64_t seq = s->seq;  // force a fresh frame
    if (snapshot) {
        size_t len; uint8_t *j = wait_frame(s, &seq, &len, 8000);
        if (j) {
            sendf(fd, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\nCache-Control: no-cache\r\n"
                      "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n", len);
            send_all(fd, j, len); free(j);
        } else sendf(fd, "HTTP/1.1 504 Gateway Timeout\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    } else {
        sendf(fd, "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; boundary=xrcamframe\r\n"
                  "Cache-Control: no-cache, no-store\r\nPragma: no-cache\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n");
        uint64_t last = 0; int timeouts = 0;
        for (;;) {
            size_t len; uint8_t *j = wait_frame(s, &seq, &len, 3000);
            if (!j) { if (++timeouts > 10) break; continue; }
            timeouts = 0;
            if (maxfps > 0) {
                uint64_t t = now_ms();
                if (t - last < (uint64_t)(1000 / maxfps)) { free(j); continue; }
                last = t;
            }
            int e = sendf(fd, "--xrcamframe\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n", len);
            if (!e) e = send_all(fd, j, len);
            if (!e) e = send_all(fd, "\r\n", 2);
            free(j);
            if (e) break;
        }
    }
    dev_release(s->dev);
    stream_sub(s, -1);
}

static void *http_conn(void *arg) {
    int fd = (int)(intptr_t)arg;
    pthread_setname_np(pthread_self(), "xrcam-http");
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    char req[4096]; size_t n = 0;
    while (n < sizeof req - 1) {
        ssize_t r = recv(fd, req + n, sizeof req - 1 - n, 0);
        if (r <= 0) goto done;
        n += (size_t)r; req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    char method[8] = {0}, path[1024] = {0};
    if (sscanf(req, "%7s %1023s", method, path) != 2) goto done;
    char *q = strchr(path, '?'); if (q) *q++ = 0;
    if (!strcmp(path, "/") || !strcmp(path, "/index.html")) http_index(fd);
    else if (!strcmp(path, "/api/cameras")) http_json(fd);
    else if (!strcmp(path, "/mux")) http_mux(fd, q);
    else if (!strncmp(path, "/stream/", 8) || !strncmp(path, "/snapshot/", 10)) {
        int snap = path[1] == 's' && path[2] == 'n';
        const char *id = path + (snap ? 10 : 8);
        char idb[64]; snprintf(idb, sizeof idb, "%s", id);
        char *dot = strrchr(idb, '.'); if (dot && (!strcmp(dot, ".jpg") || !strcmp(dot, ".mjpg"))) *dot = 0;
        stream_t *s = find_stream(idb);
        if (!s) sendf(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        else http_stream(fd, s, q, snap);
    } else if (!strcmp(path, "/api/exposure")) {
        char dv[32], ns[24], gn[16];
        device_t *d = NULL;
        if (qparam(q, "dev", dv, sizeof dv)) d = find_dev(dv);
        if (!d || !qparam(q, "ns", ns, sizeof ns) || !qparam(q, "gain", gn, sizeof gn)) {
            sendf(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nusage: /api/exposure?dev=<qvr device>&ns=<exposure ns>&gain=<gain>\n");
        } else {
            d->exp_ns = strtoull(ns, NULL, 10); d->exp_gain = atoi(gn); d->exp_pending = 1;
            sendf(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nqueued (applied while device is running)\n");
        }
    } else if (!strcmp(path, "/api/boost")) {
        char idv[64], gv[8]; stream_t *s = NULL;
        if (qparam(q, "id", idv, sizeof idv)) s = find_stream(idv);
        if (!s || !qparam(q, "gain", gv, sizeof gv))
            sendf(fd, "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nusage: /api/boost?id=<stream id>&gain=<1..32>\n");
        else {
            int g = atoi(gv); s->boost = g < 1 ? 1 : g > 32 ? 32 : g;
            sendf(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nok\n");
        }
    } else sendf(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
done:
    close(fd);
    return NULL;
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) g_rgb_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q") && i + 1 < argc) g_quality = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) g_idle_ms = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s [-p port] [-s rgb_downscale] [-q jpeg_quality] [-i idle_ms]\n", argv[0]); return 1; }
    }
    if (qvr_load()) return 1;
    for (int i = 0; i < NDEV; i++) pthread_mutex_init(&DEVS[i].mu, NULL);
    for (int i = 0; i < NSUBS; i++) {
        stream_t *s = &STREAMS[i];
        snprintf(s->id, sizeof s->id, "%s", SUBS[i].id);
        snprintf(s->sensor, sizeof s->sensor, "%s", SUBS[i].sensor);
        s->sensor_slot = SUBS[i].slot;
        s->dev = find_dev(SUBS[i].qvr);
        s->sub = SUBS[i].sub;
        s->dev->streams[s->sub] = s;
        pthread_mutex_init(&s->mu, NULL);
        pthread_cond_init(&s->cv, NULL);
    }
    pthread_t rt; pthread_create(&rt, NULL, reaper, NULL);
    int ls = socket(AF_INET6, SOCK_STREAM, 0);
    int one = 1, zero = 0;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(g_port), .sin6_addr = in6addr_any};
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 64)) { LOG("bind/listen :%d failed: %s", g_port, strerror(errno)); return 1; }
    LOG("xrcamd listening on :%d (%d streams)", g_port, NSUBS);
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; usleep(10000); continue; }
        pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, http_conn, (void *)(intptr_t)c)) close(c);
    }
}
