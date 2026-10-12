// GPU path: converts any GPU-sampleable AHardwareBuffer (incl. Qualcomm UBWC
// compressed NV12 used by the passthrough RGB cameras) to linear RGBA.
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <stdio.h>
#include <string.h>
#include "gpu.h"

#define LOG(...) do { fprintf(stderr, "[gpu] " __VA_ARGS__); fputc('\n', stderr); } while (0)

static const char *VS =
    "#version 300 es\n"
    "out vec2 uv;\n"
    "void main(){ vec2 p = vec2(float((gl_VertexID<<1)&2), float(gl_VertexID&2));\n"
    " uv = vec2(p.x, p.y); gl_Position = vec4(p*2.0-1.0, 0.0, 1.0); }\n";
static const char *FS =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES tex;\n"
    "in vec2 uv; out vec4 o;\n"
    "void main(){ o = texture(tex, uv); }\n";

static GLuint compile(GLenum t, const char *s) {
    GLuint sh = glCreateShader(t);
    glShaderSource(sh, 1, &s, NULL);
    glCompileShader(sh);
    GLint ok; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char b[512]; glGetShaderInfoLog(sh, sizeof b, NULL, b); LOG("shader: %s", b); }
    return sh;
}

int gpu_init(gpu_t *g) {
    memset(g, 0, sizeof *g);
    g->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g->dpy == EGL_NO_DISPLAY || !eglInitialize(g->dpy, NULL, NULL)) { LOG("eglInitialize failed"); return -1; }
    EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(g->dpy, ca, &cfg, 1, &n) || n < 1) { LOG("eglChooseConfig failed"); return -1; }
    EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g->ctx = eglCreateContext(g->dpy, cfg, EGL_NO_CONTEXT, xa);
    if (g->ctx == EGL_NO_CONTEXT) { LOG("eglCreateContext failed 0x%x", eglGetError()); return -1; }
    EGLint pa[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    g->surf = eglCreatePbufferSurface(g->dpy, cfg, pa);
    if (!eglMakeCurrent(g->dpy, g->surf, g->surf, g->ctx)) { LOG("eglMakeCurrent failed 0x%x", eglGetError()); return -1; }
    g->prog = glCreateProgram();
    glAttachShader(g->prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(g->prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(g->prog);
    GLint ok; glGetProgramiv(g->prog, GL_LINK_STATUS, &ok);
    if (!ok) { LOG("link failed"); return -1; }
    glGenTextures(1, &g->src);
    glGenTextures(1, &g->dst);
    glGenFramebuffers(1, &g->fbo);
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    LOG("EGL/GLES ready: %s", glGetString(GL_RENDERER));
    return 0;
}

// Renders ahb into a w x h RGBA buffer (out must hold w*h*4 bytes).
int gpu_convert(gpu_t *g, void *ahb, int w, int h, uint8_t *out) {
    EGLClientBuffer cb = eglGetNativeClientBufferANDROID((AHardwareBuffer *)ahb);
    if (!cb) return -1;
    EGLint ia[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR img = eglCreateImageKHR(g->dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, ia);
    if (img == EGL_NO_IMAGE_KHR) { LOG("eglCreateImageKHR 0x%x", eglGetError()); return -1; }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, g->src);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)img);
    if (g->dw != w || g->dh != h) {
        glBindTexture(GL_TEXTURE_2D, g->dst);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g->dst, 0);
        g->dw = w; g->dh = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glViewport(0, 0, w, h);
    glUseProgram(g->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, g->src);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    eglDestroyImageKHR(g->dpy, img);
    GLenum e = glGetError();
    return e == GL_NO_ERROR ? 0 : -(int)e;
}

void gpu_destroy(gpu_t *g) {
    if (g->dpy == EGL_NO_DISPLAY || !g->dpy) return;
    eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g->surf) eglDestroySurface(g->dpy, g->surf);
    if (g->ctx) eglDestroyContext(g->dpy, g->ctx);
    eglReleaseThread();
    memset(g, 0, sizeof *g);
}
