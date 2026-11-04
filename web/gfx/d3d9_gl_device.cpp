// d3d9_gl_device.cpp -- IDirect3D9 / IDirect3DDevice9 implemented on WebGL2 (via Emscripten's GLES3 layer).
//
// Conventions (see shader_xlate.h): D3D render targets are stored in D3D row order (row 0 = top), achieved by flipping y in
// every vertex shader; clip-space z is remapped [0,w] -> [-w,w]; the backbuffer is an FBO that Present() blits (flipped) to the canvas.
#include "d3d9_gl_internal.h"

#include <emscripten/html5.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <set>

namespace d3dgl {

// ====================================================================== misc globals
static std::atomic<uint32_t> g_surfaceIds{1};
static std::atomic<uint64_t> g_shaderUids{1};
static bool g_glDebug = false;
static bool g_s3tc = false, g_floatLinear = false, g_colorBufferFloat = false, g_aniso = false;
static float g_maxAniso = 1.0f;
static int g_activeUnit = 0;
static const int kScratchUnit = 31;
struct Device;
static Device *g_device = nullptr;

void Log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[d3d9gl] %s\n", buf);
    fflush(stderr);
}

uint32_t NextSurfaceId() { return g_surfaceIds++; }
bool S3tcAvailable() { return g_s3tc; }
bool S3tcSupported() { return g_s3tc; }

// ---- vertex attribute location allocation: one global location per (usage,index) semantic
static std::map<uint16_t, int> g_attribLoc;
static int AttribLoc(uint16_t key)
{
    auto it = g_attribLoc.find(key);
    if (it != g_attribLoc.end()) return it->second;
    int loc = (int)g_attribLoc.size();
    if (loc >= 16) { Log("out of vertex attribute locations for semantic %d/%d", key >> 4, key & 15); loc = 15; }
    g_attribLoc[key] = loc;
    return loc;
}

// ====================================================================== enum translation
static GLenum CmpFunc(DWORD f)
{
    switch (f) {
    case D3DCMP_NEVER: return GL_NEVER; case D3DCMP_LESS: return GL_LESS; case D3DCMP_EQUAL: return GL_EQUAL;
    case D3DCMP_LESSEQUAL: return GL_LEQUAL; case D3DCMP_GREATER: return GL_GREATER; case D3DCMP_NOTEQUAL: return GL_NOTEQUAL;
    case D3DCMP_GREATEREQUAL: return GL_GEQUAL; default: return GL_ALWAYS;
    }
}
static GLenum BlendFactor(DWORD b)
{
    switch (b) {
    case 1: return GL_ZERO; case 2: return GL_ONE; case 3: return GL_SRC_COLOR; case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA; case 6: return GL_ONE_MINUS_SRC_ALPHA; case 7: return GL_DST_ALPHA; case 8: return GL_ONE_MINUS_DST_ALPHA;
    case 9: return GL_DST_COLOR; case 10: return GL_ONE_MINUS_DST_COLOR; case 11: return GL_SRC_ALPHA_SATURATE;
    case 14: return GL_CONSTANT_COLOR; case 15: return GL_ONE_MINUS_CONSTANT_COLOR; default: return GL_ONE;
    }
}
static GLenum BlendOp(DWORD o)
{
    switch (o) { case 2: return GL_FUNC_SUBTRACT; case 3: return GL_FUNC_REVERSE_SUBTRACT; case 4: return GL_MIN; case 5: return GL_MAX; default: return GL_FUNC_ADD; }
}
static GLenum StencilOp(DWORD o)
{
    switch (o) {
    case D3DSTENCILOP_ZERO: return GL_ZERO; case D3DSTENCILOP_REPLACE: return GL_REPLACE; case D3DSTENCILOP_INCRSAT: return GL_INCR;
    case D3DSTENCILOP_DECRSAT: return GL_DECR; case D3DSTENCILOP_INVERT: return GL_INVERT; case D3DSTENCILOP_INCR: return GL_INCR_WRAP;
    case D3DSTENCILOP_DECR: return GL_DECR_WRAP; default: return GL_KEEP;
    }
}
static GLenum WrapMode(DWORD a)
{
    switch (a) { case D3DTADDRESS_MIRROR: return GL_MIRRORED_REPEAT; case D3DTADDRESS_CLAMP: return GL_CLAMP_TO_EDGE;
    case D3DTADDRESS_BORDER: return GL_CLAMP_TO_EDGE; case D3DTADDRESS_MIRRORONCE: return GL_MIRRORED_REPEAT; default: return GL_REPEAT; }
}

struct SamplerState {
    DWORD addrU = D3DTADDRESS_WRAP, addrV = D3DTADDRESS_WRAP, addrW = D3DTADDRESS_WRAP;
    DWORD mag = D3DTEXF_POINT, min = D3DTEXF_POINT, mip = D3DTEXF_NONE;
    DWORD maxAniso = 1, srgb = 0;
};

struct Program {
    GLuint prog = 0;
    bool failed = false;
    GLint uHalfPixel = -1, uBgra = -1, uAlpha = -1, uSrgbWrite = -1, uSrgbRead = -1;
    float lastHalf[2] = {-1, -1}, lastAlpha[2] = {-1, -1};
    int lastBgra = -12345, lastSrgbW = -1, lastSrgbR = -12345;
    uint32_t samplerMask = 0;
    uint8_t samplerKind[16] = {};
    VertexShader *vs = nullptr; PixelShader *ps = nullptr;
};

struct FboKey {
    uint32_t c[4]; uint32_t d;
    bool operator<(const FboKey &o) const { return memcmp(this, &o, sizeof *this) < 0; }
    bool operator==(const FboKey &o) const { return memcmp(this, &o, sizeof *this) == 0; }
};
static std::map<FboKey, GLuint> g_fbos;
void PurgeFBOs(uint32_t id)
{
    if (!g_device) return;
    for (auto it = g_fbos.begin(); it != g_fbos.end();) {
        const FboKey &k = it->first;
        if (k.c[0] == id || k.c[1] == id || k.c[2] == id || k.c[3] == id || k.d == id) { glDeleteFramebuffers(1, &it->second); it = g_fbos.erase(it); }
        else ++it;
    }
}

// ====================================================================== Device
struct Device : RefImpl<IDirect3DDevice9> {
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = 0;
    D3DPRESENT_PARAMETERS pp = {};
    Surface *backbuffer = nullptr, *autoDepth = nullptr;
    GLuint vao = 0, ubo[2] = {0, 0}, dynVB = 0, blitRead = 0, blitDraw = 0, stagingTex = 0;
    GLuint defTex[3] = {0, 0, 0};   // 2D, cube, 3D
    DWORD rs[256];
    SamplerState ss[16];
    IDirect3DBaseTexture9 *tex[16] = {};
    struct Stream { VertexBuffer *vb = nullptr; UINT offset = 0, stride = 0; } streams[16];
    IndexBuffer *ib = nullptr;
    VertexDecl *decl = nullptr;
    VertexShader *vs = nullptr;
    PixelShader *ps = nullptr;
    Surface *rt[4] = {}, *ds = nullptr;
    D3DVIEWPORT9 vp = {0, 0, 0, 0, 0.f, 1.f};
    RECT scissor = {0, 0, 0, 0};
    float vsConst[256 * 4], psConst[224 * 4];
    int vsLo = 0, vsHi = 256, psLo = 0, psHi = 224;   // dirty register range [lo,hi)
    std::map<std::pair<uint64_t, std::pair<uint64_t, uint32_t>>, Program *> programs;
    Program *curProgram = nullptr;
    std::unordered_map<uint64_t, GLuint> samplerObjs;
    FboKey curFbo = {{~0u, ~0u, ~0u, ~0u}, ~0u};
    bool inScene = false;
    uint32_t drawCount = 0, glErrCount = 0, frame = 0;

    // cached GL state
    struct GLCache {
        int depthTest = -1, depthMask = -1, cull = -1, blend = -1, scissor = -1, stencil = -1, colorMask = -1, polyOff = -1, sepBlend = -1;
        GLenum depthFunc = 0, cullFace = 0, sfactor = 0, dfactor = 0, sfactorA = 0, dfactorA = 0, eq = 0, eqA = 0;
        float polyU = 1e30f, polyS = 1e30f;
        int vpx = -1, vpy = -1, vpw = -1, vph = -1; float zn = -1, zf = -1;
        int sx = -1, sy = -1, sw = -1, sh = -1;
        uint32_t blendColor = 0xdeadbeef;
        uint64_t stencilHash = ~0ull;
        GLuint program = ~0u; GLuint ubo = 0;
        uint32_t attribMask = 0;
    } gl;

    Device() { memset(rs, 0, sizeof rs); memset(vsConst, 0, sizeof vsConst); memset(psConst, 0, sizeof psConst); }
    ~Device() override {}

    // ---------------------------------------------------------------- setup
    bool init(const D3DPRESENT_PARAMETERS *p)
    {
        pp = *p;
        EmscriptenWebGLContextAttributes a;
        emscripten_webgl_init_context_attributes(&a);
        a.majorVersion = 2; a.minorVersion = 0;
        a.alpha = 0; a.depth = 0; a.stencil = 0; a.antialias = 0;
        a.premultipliedAlpha = 0; a.preserveDrawingBuffer = 0; a.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
        a.explicitSwapControl = 1; a.renderViaOffscreenBackBuffer = 1; a.enableExtensionsByDefault = 1;
        ctx = emscripten_webgl_create_context("#canvas", &a);
        if (ctx <= 0) { Log("emscripten_webgl_create_context failed (%d)", (int)ctx); return false; }
        emscripten_webgl_make_context_current(ctx);
        for (const char *ext : {"EXT_color_buffer_float", "EXT_color_buffer_half_float", "OES_texture_float_linear", "WEBGL_compressed_texture_s3tc",
                                "EXT_texture_filter_anisotropic", "WEBGL_debug_renderer_info"})
            emscripten_webgl_enable_extension(ctx, ext);
        g_s3tc = emscripten_webgl_enable_extension(ctx, "WEBGL_compressed_texture_s3tc") != 0;
        g_floatLinear = emscripten_webgl_enable_extension(ctx, "OES_texture_float_linear") != 0;
        g_colorBufferFloat = emscripten_webgl_enable_extension(ctx, "EXT_color_buffer_float") != 0;
        g_aniso = emscripten_webgl_enable_extension(ctx, "EXT_texture_filter_anisotropic") != 0;
        if (g_aniso) glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT_, &g_maxAniso);
        g_glDebug = getenv("KISAK_GLDEBUG") != nullptr;
        Log("WebGL2: %s | %s | s3tc=%d floatLinear=%d colorBufferFloat=%d aniso=%.0f glDebug=%d", glGetString(GL_VENDOR), glGetString(GL_RENDERER),
            (int)g_s3tc, (int)g_floatLinear, (int)g_colorBufferFloat, g_maxAniso, (int)g_glDebug);
        g_device = this;

        emscripten_set_canvas_element_size("#canvas", (int)pp.BackBufferWidth, (int)pp.BackBufferHeight);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(2, ubo);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo[0]); glBufferData(GL_UNIFORM_BUFFER, sizeof vsConst, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo[1]); glBufferData(GL_UNIFORM_BUFFER, sizeof psConst, nullptr, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo[0]);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1, ubo[1]);
        glGenBuffers(1, &dynVB);
        glGenFramebuffers(1, &blitRead);
        glGenFramebuffers(1, &blitDraw);
        // 1x1 black default textures for unbound samplers
        uint8_t black[4] = {0, 0, 0, 255};
        glGenTextures(3, defTex);
        glActiveTexture(GL_TEXTURE0 + kScratchUnit); g_activeUnit = kScratchUnit;
        glBindTexture(GL_TEXTURE_2D, defTex[0]); glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
        glBindTexture(GL_TEXTURE_CUBE_MAP, defTex[1]); glTexStorage2D(GL_TEXTURE_CUBE_MAP, 1, GL_RGBA8, 1, 1);
        for (int f = 0; f < 6; ++f) glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
        glBindTexture(GL_TEXTURE_3D, defTex[2]); glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA8, 1, 1, 1); glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);

        resetStateDefaults();
        createBackbuffer();
        return true;
    }

    void resetStateDefaults()
    {
        memset(rs, 0, sizeof rs);
        rs[D3DRS_ZENABLE] = 1; rs[D3DRS_ZWRITEENABLE] = 1; rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL; rs[D3DRS_CULLMODE] = D3DCULL_CCW;
        rs[D3DRS_SRCBLEND] = D3DBLEND_ONE; rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO; rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
        rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE; rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO; rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
        rs[D3DRS_COLORWRITEENABLE] = 0xF; rs[D3DRS_COLORWRITEENABLE1] = rs[D3DRS_COLORWRITEENABLE2] = rs[D3DRS_COLORWRITEENABLE3] = 0xF;
        rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS; rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
        rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
        rs[D3DRS_CCW_STENCILFAIL] = rs[D3DRS_CCW_STENCILZFAIL] = rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
        rs[D3DRS_STENCILMASK] = 0xFFFFFFFF; rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
        rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS; rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
        rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
        for (auto &s : ss) s = SamplerState();
    }

    void createBackbuffer()
    {
        if (backbuffer) { backbuffer->Release(); backbuffer = nullptr; }
        if (autoDepth) { autoDepth->Release(); autoDepth = nullptr; }
        UINT w = pp.BackBufferWidth ? pp.BackBufferWidth : 1280, h = pp.BackBufferHeight ? pp.BackBufferHeight : 720;
        backbuffer = new Surface(this);
        backbuffer->kind = Surface::K_RENDERBUFFER; backbuffer->isBackbuffer = true;
        backbuffer->w = w; backbuffer->h = h; backbuffer->fmt = D3DFMT_A8R8G8B8; backbuffer->usage = D3DUSAGE_RENDERTARGET;
        if (pp.EnableAutoDepthStencil) {
            autoDepth = new Surface(this);
            autoDepth->kind = Surface::K_RENDERBUFFER;
            autoDepth->w = w; autoDepth->h = h; autoDepth->fmt = pp.AutoDepthStencilFormat ? pp.AutoDepthStencilFormat : D3DFMT_D24S8;
            autoDepth->usage = D3DUSAGE_DEPTHSTENCIL;
        }
        rt[0] = backbuffer; backbuffer->AddRef();
        for (int i = 1; i < 4; ++i) { if (rt[i]) { rt[i]->Release(); rt[i] = nullptr; } }
        if (ds) ds->Release();
        ds = autoDepth; if (ds) ds->AddRef();
        vp.X = 0; vp.Y = 0; vp.Width = w; vp.Height = h; vp.MinZ = 0.f; vp.MaxZ = 1.f;
        scissor = {0, 0, (LONG)w, (LONG)h};
        curFbo = {{~0u, ~0u, ~0u, ~0u}, ~0u};
    }

    // ---------------------------------------------------------------- GL helpers
    void checkGL(const char *what)
    {
        if (!g_glDebug) return;
        GLenum e;
        while ((e = glGetError()) != GL_NO_ERROR) {
            if (glErrCount++ < 40) Log("GL error 0x%x after %s (draw #%u)", e, what, drawCount);
        }
    }

    GLuint getFBO(Surface *const *colors, Surface *depth)
    {
        FboKey k;
        for (int i = 0; i < 4; ++i) k.c[i] = colors[i] ? colors[i]->id : 0;
        k.d = depth ? depth->id : 0;
        auto it = g_fbos.find(k);
        if (it != g_fbos.end()) return it->second;
        GLuint f = 0;
        glGenFramebuffers(1, &f);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f);
        GLenum bufs[4] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE};
        auto attach = [&](GLenum att, Surface *s) {
            s->ensureGL();
            if (s->kind == Surface::K_TEXLEVEL) {
                if (!s->tex || !s->tex->tex) return;
                GLenum tgt = s->tex->faces == 6 ? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + s->face) : GL_TEXTURE_2D;
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, att, tgt, s->tex->tex, (GLint)s->level);
            } else {
                glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, att, GL_RENDERBUFFER, s->rbo);
            }
        };
        for (int i = 0; i < 4; ++i) if (colors[i]) { attach(GL_COLOR_ATTACHMENT0 + i, colors[i]); bufs[i] = GL_COLOR_ATTACHMENT0 + i; }
        if (depth) attach(depth->hasStencil() ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, depth);
        glDrawBuffers(4, bufs);
        if (g_glDebug) {
            GLenum st = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
            if (st != GL_FRAMEBUFFER_COMPLETE) Log("FBO incomplete: 0x%x (rt0 %s fmt %d, ds %s)", st, colors[0] ? "set" : "none", colors[0] ? (int)colors[0]->fmt : 0, depth ? "set" : "none");
        }
        g_fbos[k] = f;
        return f;
    }

    void applyRenderTargets()
    {
        FboKey k;
        for (int i = 0; i < 4; ++i) k.c[i] = rt[i] ? rt[i]->id : 0;
        k.d = ds ? ds->id : 0;
        if (k == curFbo) return;
        GLuint f = getFBO(rt, ds);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        curFbo = k;
        gl.attribMask = gl.attribMask;  // unchanged
        // viewport & scissor depend on the target: force re-issue
        gl.vpw = -1; gl.sw = -1;
    }

    void rtSize(UINT *w, UINT *h)
    {
        Surface *s = rt[0] ? rt[0] : ds;
        *w = s ? s->w : pp.BackBufferWidth; *h = s ? s->h : pp.BackBufferHeight;
    }

    void applyViewportScissor()
    {
        UINT rw, rh; rtSize(&rw, &rh);
        int x = (int)vp.X, y = (int)vp.Y, w = (int)vp.Width, h = (int)vp.Height;
        if (x != gl.vpx || y != gl.vpy || w != gl.vpw || h != gl.vph) { glViewport(x, y, w, h); gl.vpx = x; gl.vpy = y; gl.vpw = w; gl.vph = h; }
        if (vp.MinZ != gl.zn || vp.MaxZ != gl.zf) { glDepthRangef(vp.MinZ, vp.MaxZ); gl.zn = vp.MinZ; gl.zf = vp.MaxZ; }
        int sc = rs[D3DRS_SCISSORTESTENABLE] ? 1 : 0;
        if (sc != gl.scissor) { if (sc) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST); gl.scissor = sc; }
        if (sc) {
            int sx = scissor.left, sy = scissor.top, sw = scissor.right - scissor.left, sh = scissor.bottom - scissor.top;
            if (sx != gl.sx || sy != gl.sy || sw != gl.sw || sh != gl.sh) { glScissor(sx, sy, std::max(sw, 0), std::max(sh, 0)); gl.sx = sx; gl.sy = sy; gl.sw = sw; gl.sh = sh; }
        }
    }

    void applyState()
    {
        int dt = (rs[D3DRS_ZENABLE] != 0 && ds) ? 1 : 0;
        if (dt != gl.depthTest) { if (dt) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST); gl.depthTest = dt; }
        int dm = rs[D3DRS_ZWRITEENABLE] ? 1 : 0;
        if (dm != gl.depthMask) { glDepthMask(dm ? GL_TRUE : GL_FALSE); gl.depthMask = dm; }
        GLenum df = CmpFunc(rs[D3DRS_ZFUNC]);
        if (df != gl.depthFunc) { glDepthFunc(df); gl.depthFunc = df; }

        // culling: D3D "front" (CW in y-down screen space) is GL's CCW because of the y flip
        int cull = rs[D3DRS_CULLMODE] != D3DCULL_NONE ? 1 : 0;
        if (cull != gl.cull) { if (cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE); gl.cull = cull; }
        if (cull) {
            GLenum cf = rs[D3DRS_CULLMODE] == D3DCULL_CW ? GL_FRONT : GL_BACK;
            if (cf != gl.cullFace) { glCullFace(cf); gl.cullFace = cf; }
        }

        int cm = (int)(rs[D3DRS_COLORWRITEENABLE] & 0xF);
        if (cm != gl.colorMask) { glColorMask(cm & 1, (cm >> 1) & 1, (cm >> 2) & 1, (cm >> 3) & 1); gl.colorMask = cm; }

        int be = rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0;
        if (be != gl.blend) { if (be) glEnable(GL_BLEND); else glDisable(GL_BLEND); gl.blend = be; }
        if (be) {
            bool sep = rs[D3DRS_SEPARATEALPHABLENDENABLE] != 0;
            GLenum s = BlendFactor(rs[D3DRS_SRCBLEND]), d = BlendFactor(rs[D3DRS_DESTBLEND]);
            GLenum sa = sep ? BlendFactor(rs[D3DRS_SRCBLENDALPHA]) : s, da = sep ? BlendFactor(rs[D3DRS_DESTBLENDALPHA]) : d;
            if (s != gl.sfactor || d != gl.dfactor || sa != gl.sfactorA || da != gl.dfactorA) {
                glBlendFuncSeparate(s, d, sa, da); gl.sfactor = s; gl.dfactor = d; gl.sfactorA = sa; gl.dfactorA = da; }
            GLenum e = BlendOp(rs[D3DRS_BLENDOP]), ea = sep ? BlendOp(rs[D3DRS_BLENDOPALPHA]) : e;
            if (e != gl.eq || ea != gl.eqA) { glBlendEquationSeparate(e, ea); gl.eq = e; gl.eqA = ea; }
            uint32_t bc = rs[D3DRS_BLENDFACTOR];
            if (bc != gl.blendColor) { glBlendColor(((bc >> 16) & 255) / 255.f, ((bc >> 8) & 255) / 255.f, (bc & 255) / 255.f, ((bc >> 24) & 255) / 255.f); gl.blendColor = bc; }
        }

        int se = (rs[D3DRS_STENCILENABLE] && ds && ds->hasStencil()) ? 1 : 0;
        if (se != gl.stencil) { if (se) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST); gl.stencil = se; }
        if (se) {
            bool two = rs[D3DRS_TWOSIDEDSTENCILMODE] != 0;
            uint64_t h = 1469598103934665603ull;
            auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
            for (int i : {D3DRS_STENCILFUNC, D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL,
                          D3DRS_STENCILPASS, D3DRS_CCW_STENCILFUNC, D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL, D3DRS_CCW_STENCILPASS})
                mix(rs[i]);
            mix(two);
            if (h != gl.stencilHash) {
                gl.stencilHash = h;
                glStencilFuncSeparate(GL_FRONT, CmpFunc(rs[D3DRS_STENCILFUNC]), (GLint)(rs[D3DRS_STENCILREF] & 0xFF), rs[D3DRS_STENCILMASK] & 0xFF);
                glStencilOpSeparate(GL_FRONT, StencilOp(rs[D3DRS_STENCILFAIL]), StencilOp(rs[D3DRS_STENCILZFAIL]), StencilOp(rs[D3DRS_STENCILPASS]));
                glStencilMaskSeparate(GL_FRONT_AND_BACK, rs[D3DRS_STENCILWRITEMASK] & 0xFF);
                if (two) {
                    glStencilFuncSeparate(GL_BACK, CmpFunc(rs[D3DRS_CCW_STENCILFUNC]), (GLint)(rs[D3DRS_STENCILREF] & 0xFF), rs[D3DRS_STENCILMASK] & 0xFF);
                    glStencilOpSeparate(GL_BACK, StencilOp(rs[D3DRS_CCW_STENCILFAIL]), StencilOp(rs[D3DRS_CCW_STENCILZFAIL]), StencilOp(rs[D3DRS_CCW_STENCILPASS]));
                } else {
                    glStencilFuncSeparate(GL_BACK, CmpFunc(rs[D3DRS_STENCILFUNC]), (GLint)(rs[D3DRS_STENCILREF] & 0xFF), rs[D3DRS_STENCILMASK] & 0xFF);
                    glStencilOpSeparate(GL_BACK, StencilOp(rs[D3DRS_STENCILFAIL]), StencilOp(rs[D3DRS_STENCILZFAIL]), StencilOp(rs[D3DRS_STENCILPASS]));
                }
            }
        }

        float bias, slope;
        memcpy(&bias, &rs[D3DRS_DEPTHBIAS], 4); memcpy(&slope, &rs[D3DRS_SLOPESCALEDEPTHBIAS], 4);
        int po = (bias != 0.f || slope != 0.f) ? 1 : 0;
        if (po != gl.polyOff) { if (po) glEnable(GL_POLYGON_OFFSET_FILL); else glDisable(GL_POLYGON_OFFSET_FILL); gl.polyOff = po; }
        if (po) {
            float units = bias * 16777216.0f;
            if (units != gl.polyU || slope != gl.polyS) { glPolygonOffset(slope, units); gl.polyU = units; gl.polyS = slope; }
        }
    }

    // ---------------------------------------------------------------- shaders / programs
    static uint64_t HashKeys(const std::vector<d3dxlate::Semantic> &v)
    {
        std::vector<uint16_t> k;
        for (auto &s : v) k.push_back(s.key());
        std::sort(k.begin(), k.end());
        uint64_t h = 1469598103934665603ull;
        for (uint16_t x : k) h = (h ^ x) * 1099511628211ull;
        return h;
    }

    GLuint compile(GLenum type, const std::string &src, const char *what)
    {
        GLuint s = glCreateShader(type);
        const char *p = src.c_str();
        glShaderSource(s, 1, &p, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048]; GLsizei n = 0;
            glGetShaderInfoLog(s, sizeof log, &n, log);
            Log("%s shader compile failed: %s", what, log);
            if (getenv("KISAK_DUMP_BADSHADER")) fprintf(stderr, "----\n%s\n----\n", src.c_str());
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    bool prepareVS(VertexShader *v)
    {
        if (v->glShader) return true;
        if (v->failed) return false;
        if (!v->translated) {
            d3dxlate::Options o;
            if (!d3dxlate::Translate(v->code.data(), v->code.size(), o, v->glsl, v->info, v->error)) {
                Log("vertex shader translation failed: %s", v->error.c_str()); v->failed = true; return false;
            }
            v->translated = true;
            v->outSig = HashKeys(v->info.outputs);
        }
        v->glShader = compile(GL_VERTEX_SHADER, v->glsl, "vertex");
        if (!v->glShader) { v->failed = true; return false; }
        return true;
    }

    GLuint preparePS(PixelShader *p, VertexShader *v, uint32_t shadowMask)
    {
        if (p->failed) return 0;
        auto key = std::make_pair(v->outSig, shadowMask);
        auto it = p->glShaders.find(key);
        if (it != p->glShaders.end()) return it->second;
        std::vector<uint16_t> keys;
        for (auto &s : v->info.outputs) keys.push_back(s.key());
        d3dxlate::Options o;
        o.vsOutputs = &keys;
        o.shadowMask = shadowMask;
        std::string glsl, err;
        d3dxlate::Info info;
        GLuint sh = 0;
        if (!d3dxlate::Translate(p->code.data(), p->code.size(), o, glsl, info, err)) {
            Log("pixel shader translation failed: %s", err.c_str()); p->failed = true;
        } else {
            p->info = info; p->translated = true;
            sh = compile(GL_FRAGMENT_SHADER, glsl, "fragment");
            if (!sh) p->glsl = glsl;
        }
        p->glShaders[key] = sh;
        return sh;
    }

    Program *getProgram(uint32_t shadowMask)
    {
        if (!vs || !ps) return nullptr;
        uint32_t sm = shadowMask;
        auto key = std::make_pair(vs->uid, std::make_pair(ps->uid, sm));
        auto it = programs.find(key);
        if (it != programs.end()) return it->second;
        Program *pr = new Program();
        programs[key] = pr;
        pr->vs = vs; pr->ps = ps;
        if (!prepareVS(vs)) { pr->failed = true; return pr; }
        GLuint fs = preparePS(ps, vs, sm);
        if (!fs) { pr->failed = true; return pr; }
        pr->samplerMask = ps->info.samplerMask;
        memcpy(pr->samplerKind, ps->info.samplerKind, 16);
        GLuint p = glCreateProgram();
        glAttachShader(p, vs->glShader);
        glAttachShader(p, fs);
        for (auto &s : vs->info.inputs) {
            char nm[32];
            snprintf(nm, sizeof nm, "a_%d_%d", s.usage, s.index);
            glBindAttribLocation(p, (GLuint)AttribLoc(s.key()), nm);
        }
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048]; GLsizei n = 0;
            glGetProgramInfoLog(p, sizeof log, &n, log);
            Log("program link failed: %s", log);
            glDeleteProgram(p);
            pr->failed = true;
            return pr;
        }
        pr->prog = p;
        glUseProgram(p); gl.program = p;
        for (int s = 0; s < 16; ++s) {
            if (!(pr->samplerMask & (1u << s))) continue;
            char nm[8]; snprintf(nm, sizeof nm, "s%d", s);
            GLint loc = glGetUniformLocation(p, nm);
            if (loc >= 0) glUniform1i(loc, s);
        }
        GLuint bi = glGetUniformBlockIndex(p, "VSConsts"); if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p, bi, 0);
        bi = glGetUniformBlockIndex(p, "PSConsts"); if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p, bi, 1);
        pr->uHalfPixel = glGetUniformLocation(p, "u_halfPixel");
        pr->uBgra = glGetUniformLocation(p, "u_bgra");
        pr->uAlpha = glGetUniformLocation(p, "u_alpha");
        pr->uSrgbWrite = glGetUniformLocation(p, "u_srgbWrite");
        pr->uSrgbRead = glGetUniformLocation(p, "u_srgbRead");
        return pr;
    }

    // ---------------------------------------------------------------- texture binding
    TexCore *coreOf(IDirect3DBaseTexture9 *t)
    {
        switch (t->GetType()) {
        case D3DRTYPE_TEXTURE: return &static_cast<Texture2D *>(t)->core;
        case D3DRTYPE_CUBETEXTURE: return &static_cast<TextureCube *>(t)->core;
        case D3DRTYPE_VOLUMETEXTURE: return &static_cast<TextureVolume *>(t)->core;
        default: return nullptr;
        }
    }

    GLuint samplerObject(const SamplerState &s, bool compare, bool noLinear, bool hasMips)
    {
        uint64_t k = 0;
        auto put = [&](uint64_t v, int bits) { k = (k << bits) | (v & ((1ull << bits) - 1)); };
        put(s.addrU, 3); put(s.addrV, 3); put(s.addrW, 3); put(s.mag, 2); put(s.min, 2); put(s.mip, 2); put(std::min<DWORD>(s.maxAniso, 31), 5);
        put(compare, 1); put(noLinear, 1); put(hasMips, 1);
        auto it = samplerObjs.find(k);
        if (it != samplerObjs.end()) return it->second;
        GLuint o = 0;
        glGenSamplers(1, &o);
        glSamplerParameteri(o, GL_TEXTURE_WRAP_S, WrapMode(s.addrU));
        glSamplerParameteri(o, GL_TEXTURE_WRAP_T, WrapMode(s.addrV));
        glSamplerParameteri(o, GL_TEXTURE_WRAP_R, WrapMode(s.addrW));
        bool minLin = s.min >= D3DTEXF_LINEAR, magLin = s.mag >= D3DTEXF_LINEAR;
        if (noLinear) { minLin = magLin = false; }
        GLenum minF;
        if (s.mip == D3DTEXF_NONE || !hasMips) minF = minLin ? GL_LINEAR : GL_NEAREST;
        else if (s.mip == D3DTEXF_POINT) minF = minLin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
        else minF = minLin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
        glSamplerParameteri(o, GL_TEXTURE_MIN_FILTER, (GLint)minF);
        glSamplerParameteri(o, GL_TEXTURE_MAG_FILTER, magLin ? GL_LINEAR : GL_NEAREST);
        if (g_aniso && (s.min == D3DTEXF_ANISOTROPIC || s.mag == D3DTEXF_ANISOTROPIC))
            glSamplerParameterf(o, GL_TEXTURE_MAX_ANISOTROPY_EXT_, std::min((float)s.maxAniso, g_maxAniso));
        if (compare) { glSamplerParameteri(o, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE); glSamplerParameteri(o, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL); }
        samplerObjs[k] = o;
        return o;
    }

    // returns the shadow-sampler mask for the PS's samplers and binds textures
    uint32_t shadowMaskForPS()
    {
        uint32_t m = 0;
        if (!ps || !ps->translated) return 0;
        for (int s = 0; s < 16; ++s) {
            if (!(ps->info.samplerMask & (1u << s)) || ps->info.samplerKind[s] != d3dxlate::SAMP_2D || !tex[s]) continue;
            TexCore *c = coreOf(tex[s]);
            if (c && c->isDepth() && GetFmt(c->fmt).depth) m |= 1u << s;
        }
        return m;
    }

    void bindTextures(Program *pr, uint32_t *srgbReadMask)
    {
        uint32_t srgb = 0;
        for (int s = 0; s < 16; ++s) {
            if (!(pr->samplerMask & (1u << s))) continue;
            uint8_t kind = pr->samplerKind[s] ? pr->samplerKind[s] : (uint8_t)d3dxlate::SAMP_2D;
            GLenum target = kind == d3dxlate::SAMP_CUBE ? GL_TEXTURE_CUBE_MAP : kind == d3dxlate::SAMP_VOLUME ? GL_TEXTURE_3D : GL_TEXTURE_2D;
            GLuint t = defTex[kind == d3dxlate::SAMP_CUBE ? 1 : kind == d3dxlate::SAMP_VOLUME ? 2 : 0];
            bool compare = false, noLinear = false, hasMips = false;
            TexCore *c = tex[s] ? coreOf(tex[s]) : nullptr;
            if (c && c->target == target) {
                c->ensureGL();
                if (c->tex) {
                    t = c->tex;
                    compare = c->isDepth() && GetFmt(c->fmt).depth;
                    D3DFORMAT f = c->fmt;
                    noLinear = !g_floatLinear && (f == D3DFMT_R32F || f == D3DFMT_G32R32F || f == D3DFMT_A32B32G32R32F);
                    hasMips = c->levels > 1;
                    if (ss[s].srgb) srgb |= 1u << s;
                }
            }
            if (g_activeUnit != s) { glActiveTexture(GL_TEXTURE0 + s); g_activeUnit = s; }
            glBindTexture(target, t);
            glBindSampler(s, samplerObject(ss[s], compare, noLinear, hasMips));
        }
        *srgbReadMask = srgb;
    }

    void uploadConstants()
    {
        if (vsHi > vsLo) {
            glBindBuffer(GL_UNIFORM_BUFFER, ubo[0]);
            glBufferSubData(GL_UNIFORM_BUFFER, vsLo * 16, (vsHi - vsLo) * 16, vsConst + vsLo * 4);
            vsLo = 256; vsHi = 0;
        }
        if (psHi > psLo) {
            glBindBuffer(GL_UNIFORM_BUFFER, ubo[1]);
            glBufferSubData(GL_UNIFORM_BUFFER, psLo * 16, (psHi - psLo) * 16, psConst + psLo * 4);
            psLo = 224; psHi = 0;
        }
    }

    // ---------------------------------------------------------------- vertex inputs
    struct ElemGL { GLint size; GLenum type; GLboolean norm; bool isColor; };
    static bool ElemFormat(BYTE t, ElemGL *e)
    {
        switch (t) {
        case D3DDECLTYPE_FLOAT1: *e = {1, GL_FLOAT, GL_FALSE, false}; return true;
        case D3DDECLTYPE_FLOAT2: *e = {2, GL_FLOAT, GL_FALSE, false}; return true;
        case D3DDECLTYPE_FLOAT3: *e = {3, GL_FLOAT, GL_FALSE, false}; return true;
        case D3DDECLTYPE_FLOAT4: *e = {4, GL_FLOAT, GL_FALSE, false}; return true;
        case D3DDECLTYPE_D3DCOLOR: *e = {4, GL_UNSIGNED_BYTE, GL_TRUE, true}; return true;
        case D3DDECLTYPE_UBYTE4: *e = {4, GL_UNSIGNED_BYTE, GL_FALSE, false}; return true;
        case D3DDECLTYPE_SHORT2: *e = {2, GL_SHORT, GL_FALSE, false}; return true;
        case D3DDECLTYPE_SHORT4: *e = {4, GL_SHORT, GL_FALSE, false}; return true;
        case D3DDECLTYPE_UBYTE4N: *e = {4, GL_UNSIGNED_BYTE, GL_TRUE, false}; return true;
        case D3DDECLTYPE_SHORT2N: *e = {2, GL_SHORT, GL_TRUE, false}; return true;
        case D3DDECLTYPE_SHORT4N: *e = {4, GL_SHORT, GL_TRUE, false}; return true;
        case D3DDECLTYPE_USHORT2N: *e = {2, GL_UNSIGNED_SHORT, GL_TRUE, false}; return true;
        case D3DDECLTYPE_USHORT4N: *e = {4, GL_UNSIGNED_SHORT, GL_TRUE, false}; return true;
        case D3DDECLTYPE_DEC3N: *e = {4, GL_INT_2_10_10_10_REV, GL_TRUE, false}; return true;
        case D3DDECLTYPE_UDEC3: *e = {4, GL_UNSIGNED_INT_2_10_10_10_REV, GL_FALSE, false}; return true;
        case D3DDECLTYPE_FLOAT16_2: *e = {2, GL_HALF_FLOAT, GL_FALSE, false}; return true;
        case D3DDECLTYPE_FLOAT16_4: *e = {4, GL_HALF_FLOAT, GL_FALSE, false}; return true;
        default: return false;
        }
    }

    // `baseVertex` is added to every stream's start; returns the u_bgra mask for the VS inputs
    int setupVertexInputs(Program *pr, INT baseVertex, bool *ok)
    {
        *ok = true;
        int bgra = 0;
        uint32_t enabled = 0;
        const auto &inputs = pr->vs->info.inputs;
        for (size_t k = 0; k < inputs.size(); ++k) {
            const d3dxlate::Semantic &sem = inputs[k];
            int loc = AttribLoc(sem.key());
            const D3DVERTEXELEMENT9 *el = nullptr;
            if (decl) for (auto &e : decl->elems) if (e.Usage == sem.usage && e.UsageIndex == sem.index) { el = &e; break; }
            ElemGL eg;
            if (!el || el->Stream >= 16 || !streams[el->Stream].vb || !ElemFormat(el->Type, &eg)) {
                glDisableVertexAttribArray(loc);
                glVertexAttrib4f(loc, 0.f, 0.f, 0.f, 1.f);
                continue;
            }
            Stream &st = streams[el->Stream];
            st.vb->core.sync();
            glBindBuffer(GL_ARRAY_BUFFER, st.vb->core.buf);
            size_t off = (size_t)st.offset + (size_t)(int64_t)baseVertex * st.stride + el->Offset;
            glVertexAttribPointer((GLuint)loc, eg.size, eg.type, eg.norm, (GLsizei)st.stride, (const void *)off);
            glEnableVertexAttribArray(loc);
            enabled |= 1u << loc;
            if (eg.isColor) bgra |= 1 << k;
        }
        // disable stale attributes
        uint32_t stale = gl.attribMask & ~enabled;
        for (int i = 0; stale; ++i, stale >>= 1) if (stale & 1) glDisableVertexAttribArray(i);
        gl.attribMask = enabled;
        return bgra;
    }

    bool prepareDraw(INT baseVertex, Program **outPr)
    {
        DrainDeferredDeletes();
        if (!vs || !ps) return false;
        applyRenderTargets();
        applyViewportScissor();
        applyState();
        // The shadow mask needs the PS translated once (sampler kinds); translate it against this VS first.
        if (!vs->translated) prepareVS(vs);
        if (!ps->translated && vs->translated) preparePS(ps, vs, 0);
        Program *pr = getProgram(shadowMaskForPS());
        if (!pr || pr->failed || !pr->prog) return false;
        if (gl.program != pr->prog) { glUseProgram(pr->prog); gl.program = pr->prog; }
        uint32_t srgbRead = 0;
        bindTextures(pr, &srgbRead);
        uploadConstants();
        bool ok;
        int bgra = setupVertexInputs(pr, baseVertex, &ok);
        if (pr->uBgra >= 0 && bgra != pr->lastBgra) { glUniform1i(pr->uBgra, bgra); pr->lastBgra = bgra; }
        UINT rw, rh; rtSize(&rw, &rh);
        float hp[2] = {1.0f / (float)std::max<UINT>(rw, 1), 1.0f / (float)std::max<UINT>(rh, 1)};
        if (pr->uHalfPixel >= 0 && (hp[0] != pr->lastHalf[0] || hp[1] != pr->lastHalf[1])) { glUniform2f(pr->uHalfPixel, hp[0], hp[1]); pr->lastHalf[0] = hp[0]; pr->lastHalf[1] = hp[1]; }
        if (pr->uAlpha >= 0) {
            float af = rs[D3DRS_ALPHATESTENABLE] ? (float)rs[D3DRS_ALPHAFUNC] : 0.f, ar = (float)(rs[D3DRS_ALPHAREF] & 0xFF) / 255.f;
            if (af != pr->lastAlpha[0] || ar != pr->lastAlpha[1]) { glUniform2f(pr->uAlpha, af, ar); pr->lastAlpha[0] = af; pr->lastAlpha[1] = ar; }
        }
        if (pr->uSrgbWrite >= 0) { int w = rs[D3DRS_SRGBWRITEENABLE] ? 1 : 0; if (w != pr->lastSrgbW) { glUniform1i(pr->uSrgbWrite, w); pr->lastSrgbW = w; } }
        if (pr->uSrgbRead >= 0 && (int)srgbRead != pr->lastSrgbR) { glUniform1i(pr->uSrgbRead, (GLint)srgbRead); pr->lastSrgbR = (int)srgbRead; }
        *outPr = pr;
        return true;
    }

    static GLenum PrimMode(D3DPRIMITIVETYPE t, UINT primCount, UINT *count)
    {
        switch (t) {
        case D3DPT_POINTLIST: *count = primCount; return GL_POINTS;
        case D3DPT_LINELIST: *count = primCount * 2; return GL_LINES;
        case D3DPT_LINESTRIP: *count = primCount + 1; return GL_LINE_STRIP;
        case D3DPT_TRIANGLESTRIP: *count = primCount + 2; return GL_TRIANGLE_STRIP;
        case D3DPT_TRIANGLEFAN: *count = primCount + 2; return GL_TRIANGLE_FAN;
        default: *count = primCount * 3; return GL_TRIANGLES;
        }
    }

    // ================================================================= IUnknown-ish / device interface
    HRESULT __stdcall TestCooperativeLevel() override { return D3D_OK; }
    UINT __stdcall GetAvailableTextureMem() override { return 512u * 1024u * 1024u; }

    HRESULT __stdcall CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **sc) override { *sc = new SwapChain(this); return D3D_OK; }
    HRESULT __stdcall GetSwapChain(UINT, IDirect3DSwapChain9 **sc) override { *sc = new SwapChain(this); return D3D_OK; }

    HRESULT __stdcall Reset(D3DPRESENT_PARAMETERS *p) override
    {
        pp = *p;
        if (pp.BackBufferWidth && pp.BackBufferHeight) emscripten_set_canvas_element_size("#canvas", (int)pp.BackBufferWidth, (int)pp.BackBufferHeight);
        resetStateDefaults();
        gl = GLCache();
        gl.program = ~0u;
        createBackbuffer();
        return D3D_OK;
    }

    HRESULT presentFrame()
    {
        DrainDeferredDeletes();
        // blit the backbuffer to the canvas (default framebuffer), flipping vertically back to GL orientation
        backbuffer->ensureGL();
        Surface *cols[4] = {backbuffer, nullptr, nullptr, nullptr};
        GLuint rf = getFBO(cols, nullptr);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, rf);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST); gl.scissor = 0;
        GLint w = (GLint)backbuffer->w, h = (GLint)backbuffer->h;
        glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        emscripten_webgl_commit_frame();
        curFbo = {{~0u, ~0u, ~0u, ~0u}, ~0u};
        ++frame;
        return D3D_OK;
    }
    HRESULT __stdcall Present(const RECT *, const RECT *, HWND, const void *) override { return presentFrame(); }

    HRESULT __stdcall GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **surf) override
    {
        *surf = backbuffer; backbuffer->AddRef(); return D3D_OK;
    }
    void __stdcall SetGammaRamp(UINT, DWORD, const D3DGAMMARAMP *) override {}

    HRESULT __stdcall CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *) override
    {
        if (!GetFmt(fmt).valid) { Log("CreateTexture: unsupported format %d", (int)fmt); return D3DERR_INVALIDCALL; }
        Texture2D *t = new Texture2D(this);
        t->core.init(w, h, 1, levels, fmt, usage, pool, 1);
        *out = t;
        return D3D_OK;
    }
    HRESULT __stdcall CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9 **out, HANDLE *) override
    {
        if (!GetFmt(fmt).valid) { Log("CreateVolumeTexture: unsupported format %d", (int)fmt); return D3DERR_INVALIDCALL; }
        TextureVolume *t = new TextureVolume(this);
        t->core.init(w, h, d, levels, fmt, usage, pool, 1);
        *out = t;
        return D3D_OK;
    }
    HRESULT __stdcall CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9 **out, HANDLE *) override
    {
        if (!GetFmt(fmt).valid) { Log("CreateCubeTexture: unsupported format %d", (int)fmt); return D3DERR_INVALIDCALL; }
        TextureCube *t = new TextureCube(this);
        t->core.init(edge, edge, 1, levels, fmt, usage, pool, 6);
        *out = t;
        return D3D_OK;
    }
    HRESULT __stdcall CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *) override
    {
        VertexBuffer *vb = new VertexBuffer(this);
        vb->core.init(len, GL_ARRAY_BUFFER, usage); vb->usage = usage; vb->pool = pool; vb->fvf = fvf;
        *out = vb;
        return D3D_OK;
    }
    HRESULT __stdcall CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *) override
    {
        IndexBuffer *ib = new IndexBuffer(this);
        ib->core.init(len, GL_ELEMENT_ARRAY_BUFFER, usage); ib->usage = usage; ib->pool = pool; ib->fmt = fmt;
        *out = ib;
        return D3D_OK;
    }
    HRESULT __stdcall CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **out, HANDLE *) override
    {
        if (!GetFmt(fmt).valid) return D3DERR_INVALIDCALL;
        Surface *s = new Surface(this);
        s->kind = Surface::K_RENDERBUFFER; s->w = w; s->h = h; s->fmt = fmt; s->usage = D3DUSAGE_RENDERTARGET;
        *out = s;
        return D3D_OK;
    }
    HRESULT __stdcall CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **out, HANDLE *) override
    {
        if (!GetFmt(fmt).valid) return D3DERR_INVALIDCALL;
        Surface *s = new Surface(this);
        s->kind = Surface::K_RENDERBUFFER; s->w = w; s->h = h; s->fmt = fmt; s->usage = D3DUSAGE_DEPTHSTENCIL;
        *out = s;
        return D3D_OK;
    }
    HRESULT __stdcall CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool, IDirect3DSurface9 **out, HANDLE *) override
    {
        if (!GetFmt(fmt).valid) return D3DERR_INVALIDCALL;
        Surface *s = new Surface(this);
        s->kind = Surface::K_PLAIN; s->w = w; s->h = h; s->fmt = fmt; s->pool = pool;
        *out = s;
        return D3D_OK;
    }

    HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst) override
    {
        TexCore *a = coreOf(src), *b = coreOf(dst);
        if (!a || !b || a->faces != b->faces) return D3DERR_INVALIDCALL;
        std::lock_guard<std::mutex> g1(a->lock);
        std::lock_guard<std::mutex> g2(b->lock);
        // match the smallest common mip chain
        UINT ls = a->levels, ld = b->levels;
        for (int f = 0; f < a->faces; ++f) {
            for (UINT ldst = 0; ldst < ld; ++ldst) {
                // find the src level with the same dimensions as dst level
                UINT dw, dh; b->levelDims(ldst, &dw, &dh, nullptr);
                for (UINT lsrc = 0; lsrc < ls; ++lsrc) {
                    UINT sw, sh; a->levelDims(lsrc, &sw, &sh, nullptr);
                    if (sw == dw && sh == dh) {
                        auto &S = a->lv[(size_t)f * ls + lsrc]; auto &D = b->lv[(size_t)f * ld + ldst];
                        if (!S.data.empty()) { D.data = S.data; D.dirty = true; }
                        break;
                    }
                }
            }
        }
        return D3D_OK;
    }

    HRESULT __stdcall GetRenderTargetData(IDirect3DSurface9 *rtS, IDirect3DSurface9 *dstS) override
    {
        Surface *src = static_cast<Surface *>(rtS), *dst = static_cast<Surface *>(dstS);
        if (dst->kind != Surface::K_PLAIN) return D3DERR_INVALIDCALL;
        return readSurface(src, dst);
    }
    HRESULT readSurface(Surface *src, Surface *dst)
    {
        src->ensureGL();
        Surface *cols[4] = {src, nullptr, nullptr, nullptr};
        GLuint f = getFBO(cols, nullptr);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, f);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        UINT w = std::min(src->w, dst->w), h = std::min(src->h, dst->h);
        std::vector<uint8_t> tmp((size_t)w * h * 4);
        glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
        UINT pitch; LevelBytes(GetFmt(dst->fmt), dst->w, dst->h, 1, &pitch, nullptr);
        dst->cpu.assign((size_t)pitch * dst->h, 0);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                const uint8_t *s = &tmp[((size_t)y * w + x) * 4]; uint8_t *d = &dst->cpu[(size_t)y * pitch + (size_t)x * 4];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
            }
        curFbo = {{~0u, ~0u, ~0u, ~0u}, ~0u};
        return D3D_OK;
    }
    HRESULT frontBufferData(Surface *dst) { return readSurface(backbuffer, dst); }

    HRESULT __stdcall StretchRect(IDirect3DSurface9 *srcS, const RECT *sr, IDirect3DSurface9 *dstS, const RECT *dr, D3DTEXTUREFILTERTYPE filter) override
    {
        Surface *src = static_cast<Surface *>(srcS), *dst = static_cast<Surface *>(dstS);
        if (dst->kind == Surface::K_PLAIN) return D3DERR_INVALIDCALL;
        RECT s = sr ? *sr : RECT{0, 0, (LONG)src->w, (LONG)src->h};
        RECT d = dr ? *dr : RECT{0, 0, (LONG)dst->w, (LONG)dst->h};
        GLuint readFbo;
        if (src->kind == Surface::K_PLAIN) {
            // upload CPU pixels to a staging texture
            const FmtInfo &fi = GetFmt(src->fmt);
            std::vector<uint8_t> conv;
            UINT pitch; LevelBytes(fi, src->w, src->h, 1, &pitch, nullptr);
            if (src->cpu.empty()) return D3D_OK;
            if (fi.conv != CONV_NONE) { ConvertForUpload(fi, src->cpu.data(), src->w, src->h, pitch, conv); }
            glActiveTexture(GL_TEXTURE0 + kScratchUnit); g_activeUnit = kScratchUnit;
            if (!stagingTex) glGenTextures(1, &stagingTex);
            glBindTexture(GL_TEXTURE_2D, stagingTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)src->w, (GLsizei)src->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, conv.empty() ? src->cpu.data() : conv.data());
            glBindFramebuffer(GL_READ_FRAMEBUFFER, blitRead);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stagingTex, 0);
            readFbo = blitRead;
        } else {
            src->ensureGL();
            Surface *cols[4] = {src->isDepth() ? nullptr : src, nullptr, nullptr, nullptr};
            readFbo = getFBO(cols, src->isDepth() ? src : nullptr);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
        }
        dst->ensureGL();
        Surface *dcols[4] = {dst->isDepth() ? nullptr : dst, nullptr, nullptr, nullptr};
        GLuint drawFbo = getFBO(dcols, dst->isDepth() ? dst : nullptr);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
        glDisable(GL_SCISSOR_TEST); gl.scissor = 0;
        glColorMask(1, 1, 1, 1); gl.colorMask = 0xF;
        GLbitfield mask = dst->isDepth() ? (GL_DEPTH_BUFFER_BIT) : GL_COLOR_BUFFER_BIT;
        if (dst->isDepth()) glDepthMask(GL_TRUE), gl.depthMask = 1;
        glBlitFramebuffer(s.left, s.top, s.right, s.bottom, d.left, d.top, d.right, d.bottom, mask,
                          (mask == GL_COLOR_BUFFER_BIT && filter >= D3DTEXF_LINEAR && !GetFmt(dst->fmt).floatFmt) ? GL_LINEAR : GL_NEAREST);
        curFbo = {{~0u, ~0u, ~0u, ~0u}, ~0u};
        return D3D_OK;
    }
    static void ConvertForUpload(const FmtInfo &fi, const uint8_t *src, UINT w, UINT h, UINT pitch, std::vector<uint8_t> &out)
    {
        out.resize((size_t)w * h * 4);
        for (UINT y = 0; y < h; ++y)
            for (UINT x = 0; x < w; ++x) {
                const uint8_t *s = src + (size_t)y * pitch + (size_t)x * 4; uint8_t *o = &out[((size_t)y * w + x) * 4];
                o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = fi.conv == CONV_BGRA8 ? s[3] : 255;
            }
    }

    HRESULT __stdcall SetRenderTarget(DWORD idx, IDirect3DSurface9 *s) override
    {
        if (idx >= 4) return D3DERR_INVALIDCALL;
        Surface *n = static_cast<Surface *>(s);
        if (n) n->AddRef();
        if (rt[idx]) rt[idx]->Release();
        rt[idx] = n;
        if (idx == 0 && n) {   // D3D resets the viewport and scissor to the new target
            vp.X = 0; vp.Y = 0; vp.Width = n->w; vp.Height = n->h; vp.MinZ = 0.f; vp.MaxZ = 1.f;
            scissor = {0, 0, (LONG)n->w, (LONG)n->h};
        }
        return D3D_OK;
    }
    HRESULT __stdcall GetRenderTarget(DWORD idx, IDirect3DSurface9 **s) override
    {
        if (idx >= 4 || !rt[idx]) { *s = nullptr; return D3DERR_NOTFOUND_(); }
        *s = rt[idx]; rt[idx]->AddRef(); return D3D_OK;
    }
    static HRESULT D3DERR_NOTFOUND_() { return MAKE_D3DHRESULT(2150); }
    HRESULT __stdcall SetDepthStencilSurface(IDirect3DSurface9 *s) override
    {
        Surface *n = static_cast<Surface *>(s);
        if (n) n->AddRef();
        if (ds) ds->Release();
        ds = n;
        return D3D_OK;
    }
    HRESULT __stdcall GetDepthStencilSurface(IDirect3DSurface9 **s) override
    {
        if (!ds) { *s = nullptr; return D3DERR_NOTFOUND_(); }
        *s = ds; ds->AddRef(); return D3D_OK;
    }

    HRESULT __stdcall BeginScene() override { inScene = true; return D3D_OK; }
    HRESULT __stdcall EndScene() override { inScene = false; return D3D_OK; }

    HRESULT __stdcall Clear(DWORD count, const void *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override
    {
        applyRenderTargets();
        GLbitfield mask = 0;
        if ((flags & D3DCLEAR_TARGET) && rt[0]) {
            glColorMask(1, 1, 1, 1); gl.colorMask = 0xF;
            glClearColor(((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, ((color >> 24) & 255) / 255.f);
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if ((flags & D3DCLEAR_ZBUFFER) && ds) { glDepthMask(GL_TRUE); gl.depthMask = 1; glClearDepthf(z); mask |= GL_DEPTH_BUFFER_BIT; }
        if ((flags & D3DCLEAR_STENCIL) && ds && ds->hasStencil()) {
            glStencilMask(0xFF); gl.stencilHash = ~0ull; glClearStencil((GLint)stencil); mask |= GL_STENCIL_BUFFER_BIT; }
        if (!mask) return D3D_OK;
        // D3D honours the scissor rectangle when enabled; with explicit rects clear each one
        if (count && rects) {
            const RECT *r = (const RECT *)rects;
            glEnable(GL_SCISSOR_TEST);
            for (DWORD i = 0; i < count; ++i) { glScissor(r[i].left, r[i].top, r[i].right - r[i].left, r[i].bottom - r[i].top); glClear(mask); }
            gl.scissor = 1; gl.sw = -1;
        } else {
            if (rs[D3DRS_SCISSORTESTENABLE]) {
                glEnable(GL_SCISSOR_TEST); gl.scissor = 1;
                glScissor(scissor.left, scissor.top, scissor.right - scissor.left, scissor.bottom - scissor.top);
                gl.sx = scissor.left; gl.sy = scissor.top; gl.sw = scissor.right - scissor.left; gl.sh = scissor.bottom - scissor.top;
            } else { glDisable(GL_SCISSOR_TEST); gl.scissor = 0; }
            glClear(mask);
        }
        checkGL("Clear");
        return D3D_OK;
    }

    HRESULT __stdcall SetViewport(const D3DVIEWPORT9 *v) override { vp = *v; return D3D_OK; }
    HRESULT __stdcall GetViewport(D3DVIEWPORT9 *v) override { *v = vp; return D3D_OK; }
    HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE s, DWORD v) override { if ((unsigned)s < 256) rs[s] = v; return D3D_OK; }
    HRESULT __stdcall GetRenderState(D3DRENDERSTATETYPE s, DWORD *v) override { *v = (unsigned)s < 256 ? rs[s] : 0; return D3D_OK; }
    HRESULT __stdcall SetTexture(DWORD stage, IDirect3DBaseTexture9 *t) override
    {
        if (stage >= 16) return D3D_OK;
        if (t) t->AddRef();
        if (tex[stage]) tex[stage]->Release();
        tex[stage] = t;
        return D3D_OK;
    }
    HRESULT __stdcall SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) override { return D3D_OK; }
    HRESULT __stdcall SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) override
    {
        if (sampler >= 16) return D3D_OK;
        SamplerState &s = ss[sampler];
        switch (type) {
        case D3DSAMP_ADDRESSU: s.addrU = value; break; case D3DSAMP_ADDRESSV: s.addrV = value; break; case D3DSAMP_ADDRESSW: s.addrW = value; break;
        case D3DSAMP_MAGFILTER: s.mag = value; break; case D3DSAMP_MINFILTER: s.min = value; break; case D3DSAMP_MIPFILTER: s.mip = value; break;
        case D3DSAMP_MAXANISOTROPY: s.maxAniso = value; break; case D3DSAMP_SRGBTEXTURE: s.srgb = value; break;
        default: break;
        }
        return D3D_OK;
    }
    HRESULT __stdcall SetScissorRect(const RECT *r) override { scissor = *r; return D3D_OK; }

    HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primCount) override
    {
        Program *pr;
        if (!prepareDraw((INT)startVertex, &pr)) return D3D_OK;
        UINT count;
        GLenum mode = PrimMode(type, primCount, &count);
        glDrawArrays(mode, 0, (GLsizei)count);
        ++drawCount;
        checkGL("DrawPrimitive");
        return D3D_OK;
    }
    HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT, UINT, UINT startIndex, UINT primCount) override
    {
        if (!ib) return D3DERR_INVALIDCALL;
        Program *pr;
        if (!prepareDraw(baseVertex, &pr)) return D3D_OK;
        ib->core.sync();
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->core.buf);
        UINT count;
        GLenum mode = PrimMode(type, primCount, &count);
        bool i32 = ib->fmt == D3DFMT_INDEX32;
        glDrawElements(mode, (GLsizei)count, i32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, (const void *)((size_t)startIndex * (i32 ? 4 : 2)));
        ++drawCount;
        checkGL("DrawIndexedPrimitive");
        return D3D_OK;
    }
    HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primCount, const void *data, UINT stride) override
    {
        UINT count;
        GLenum mode = PrimMode(type, primCount, &count);
        // stream the vertices through a scratch buffer bound as stream 0
        VertexBuffer tmp(this);
        tmp.core.init(count * stride, GL_ARRAY_BUFFER, D3DUSAGE_DYNAMIC);
        memcpy(tmp.core.cpu.data(), data, (size_t)count * stride);
        Stream saved = streams[0];
        streams[0].vb = &tmp; streams[0].offset = 0; streams[0].stride = stride;
        Program *pr;
        bool ok = prepareDraw(0, &pr);
        if (ok) { glDrawArrays(mode, 0, (GLsizei)count); ++drawCount; checkGL("DrawPrimitiveUP"); }
        streams[0] = saved;
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        tmp.core.destroyGL();
        tmp.refs = 1000;   // stack object: never delete
        return D3D_OK;
    }

    HRESULT __stdcall CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elems, IDirect3DVertexDeclaration9 **out) override
    {
        VertexDecl *d = new VertexDecl();
        for (const D3DVERTEXELEMENT9 *e = elems; e->Stream != 0xFF; ++e) d->elems.push_back(*e);
        *out = d;
        return D3D_OK;
    }
    HRESULT __stdcall SetVertexDeclaration(IDirect3DVertexDeclaration9 *d) override
    {
        VertexDecl *n = static_cast<VertexDecl *>(d);
        if (n) n->AddRef();
        if (decl) decl->Release();
        decl = n;
        return D3D_OK;
    }
    HRESULT __stdcall SetFVF(DWORD) override { return D3D_OK; }
    HRESULT __stdcall CreateVertexShader(const DWORD *fn, IDirect3DVertexShader9 **out) override
    {
        VertexShader *s = new VertexShader();
        s->uid = g_shaderUids++;
        size_t n = d3dxlate::TokenCount((const uint32_t *)fn, 1 << 20);
        if (!n) { delete s; return D3DERR_INVALIDCALL; }
        s->code.assign((const uint32_t *)fn, (const uint32_t *)fn + n);
        *out = s;
        return D3D_OK;
    }
    HRESULT __stdcall SetVertexShader(IDirect3DVertexShader9 *v) override
    {
        VertexShader *n = static_cast<VertexShader *>(v);
        if (n) n->AddRef();
        if (vs) vs->Release();
        vs = n;
        return D3D_OK;
    }
    HRESULT __stdcall SetVertexShaderConstantF(UINT reg, const float *data, UINT count) override
    {
        if (reg >= 256) return D3DERR_INVALIDCALL;
        if (reg + count > 256) count = 256 - reg;
        memcpy(vsConst + reg * 4, data, (size_t)count * 16);
        vsLo = std::min<int>(vsLo, (int)reg); vsHi = std::max<int>(vsHi, (int)(reg + count));
        return D3D_OK;
    }
    HRESULT __stdcall SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *vb, UINT offset, UINT stride) override
    {
        if (stream >= 16) return D3DERR_INVALIDCALL;
        VertexBuffer *n = static_cast<VertexBuffer *>(vb);
        if (n) n->AddRef();
        if (streams[stream].vb) streams[stream].vb->Release();
        streams[stream].vb = n; streams[stream].offset = offset; streams[stream].stride = stride;
        return D3D_OK;
    }
    HRESULT __stdcall SetIndices(IDirect3DIndexBuffer9 *i) override
    {
        IndexBuffer *n = static_cast<IndexBuffer *>(i);
        if (n) n->AddRef();
        if (ib) ib->Release();
        ib = n;
        return D3D_OK;
    }
    HRESULT __stdcall CreatePixelShader(const DWORD *fn, IDirect3DPixelShader9 **out) override
    {
        PixelShader *s = new PixelShader();
        s->uid = g_shaderUids++;
        size_t n = d3dxlate::TokenCount((const uint32_t *)fn, 1 << 20);
        if (!n) { delete s; return D3DERR_INVALIDCALL; }
        s->code.assign((const uint32_t *)fn, (const uint32_t *)fn + n);
        *out = s;
        return D3D_OK;
    }
    HRESULT __stdcall SetPixelShader(IDirect3DPixelShader9 *p) override
    {
        PixelShader *n = static_cast<PixelShader *>(p);
        if (n) n->AddRef();
        if (ps) ps->Release();
        ps = n;
        return D3D_OK;
    }
    HRESULT __stdcall SetPixelShaderConstantF(UINT reg, const float *data, UINT count) override
    {
        if (reg >= 224) return D3DERR_INVALIDCALL;
        if (reg + count > 224) count = 224 - reg;
        memcpy(psConst + reg * 4, data, (size_t)count * 16);
        psLo = std::min<int>(psLo, (int)reg); psHi = std::max<int>(psHi, (int)(reg + count));
        return D3D_OK;
    }
    HRESULT __stdcall CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **q) override
    {
        if (type != D3DQUERYTYPE_EVENT && type != D3DQUERYTYPE_OCCLUSION && type != D3DQUERYTYPE_TIMESTAMP) return D3DERR_NOTAVAILABLE;
        *q = new Query(this, type);
        return D3D_OK;
    }
    HRESULT __stdcall GetDevice_(IDirect3DDevice9 **) { return D3D_OK; }
};

IDirect3DDevice9 *DeviceInterface(Device *d) { return d; }

// ---- swap chain forwards to the device
HRESULT SwapChain::Present(const RECT *, const RECT *, HWND, const void *, DWORD) { return dev->presentFrame(); }
HRESULT SwapChain::GetBackBuffer(UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **surf) { *surf = dev->backbuffer; dev->backbuffer->AddRef(); return D3D_OK; }
HRESULT SwapChain::GetFrontBufferData(IDirect3DSurface9 *dst) { return dev->frontBufferData(static_cast<Surface *>(dst)); }

void SetScratchUnit() { if (g_activeUnit != kScratchUnit) { glActiveTexture(GL_TEXTURE0 + kScratchUnit); g_activeUnit = kScratchUnit; } }
void RestoreActiveUnit() {}

// ====================================================================== IDirect3D9
struct D3D9Impl : RefImpl<IDirect3D9> {
    UINT __stdcall GetAdapterCount() override { return 1; }
    HRESULT __stdcall GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER9 *id) override
    {
        memset(id, 0, sizeof *id);
        snprintf(id->Driver, sizeof id->Driver, "webgl2");
        snprintf(id->Description, sizeof id->Description, "WebGL2 (Direct3D 9 emulation)");
        snprintf(id->DeviceName, sizeof id->DeviceName, "\\\\.\\DISPLAY1");
        id->VendorId = 0; id->DeviceId = 0;
        return D3D_OK;
    }
    static const int kModes = 6;
    static void Mode(UINT i, D3DDISPLAYMODE *dm)
    {
        static const UINT m[kModes][3] = {{640, 480, 60}, {800, 600, 60}, {1024, 768, 60}, {1280, 720, 60}, {1600, 900, 60}, {1920, 1080, 60}};
        dm->Width = m[i][0]; dm->Height = m[i][1]; dm->RefreshRate = m[i][2]; dm->Format = D3DFMT_X8R8G8B8;
    }
    UINT __stdcall GetAdapterModeCount(UINT, D3DFORMAT fmt) override { return fmt == D3DFMT_X8R8G8B8 ? kModes : 0; }
    HRESULT __stdcall EnumAdapterModes(UINT, D3DFORMAT fmt, UINT mode, D3DDISPLAYMODE *dm) override
    {
        if (fmt != D3DFMT_X8R8G8B8 || mode >= kModes) return D3DERR_INVALIDCALL;
        Mode(mode, dm); return D3D_OK;
    }
    HRESULT __stdcall GetAdapterDisplayMode(UINT, D3DDISPLAYMODE *dm) override { Mode(kModes - 1, dm); return D3D_OK; }
    HRESULT __stdcall CheckDeviceType(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL) override { return D3D_OK; }
    HRESULT __stdcall CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD usage, D3DRESOURCETYPE rtype, D3DFORMAT f) override
    {
        const FmtInfo &fi = GetFmt(f);
        if (!fi.valid) return D3DERR_NOTAVAILABLE;
        if (usage & D3DUSAGE_DEPTHSTENCIL) {
            if (!fi.depth) return D3DERR_NOTAVAILABLE;
            if (rtype == D3DRTYPE_TEXTURE) return D3DERR_NOTAVAILABLE;   // depth textures: engine falls back to colour shadow maps (M2)
            return D3D_OK;
        }
        if (usage & D3DUSAGE_RENDERTARGET) {
            if (fi.depth || !fi.renderable) return D3DERR_NOTAVAILABLE;
            if (fi.floatFmt && !g_colorBufferFloat && !g_device) {}  // context not created yet: assume the extension is available
            return D3D_OK;
        }
        return D3D_OK;
    }
    HRESULT __stdcall CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE ms, DWORD *q) override
    {
        if (q) *q = 0;
        return ms == D3DMULTISAMPLE_NONE ? D3D_OK : D3DERR_NOTAVAILABLE;
    }
    HRESULT __stdcall CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT ds) override { return GetFmt(ds).depth ? D3D_OK : D3DERR_NOTAVAILABLE; }
    HRESULT __stdcall GetDeviceCaps(UINT, D3DDEVTYPE type, D3DCAPS9 *c) override
    {
        memset(c, 0, sizeof *c);
        c->DeviceType = type; c->AdapterOrdinal = 0;
        c->Caps = 0x00020000; c->Caps2 = 0x20000000 | 0x40000000 | 0x00020000; c->Caps3 = 0x320;
        c->PresentationIntervals = 0x80000001; c->DevCaps = 0xFFFFFFFFu; c->PrimitiveMiscCaps = 0xFFFFFFFFu; c->RasterCaps = 0xFFFFFFFFu;
        c->ZCmpCaps = c->SrcBlendCaps = c->DestBlendCaps = c->AlphaCmpCaps = 0xFFFFFFFFu; c->ShadeCaps = 0xFFFFFFFFu;
        c->TextureCaps = 0xFFFFFFFFu & ~(0x2u | 0x20u);   // not power-of-two-only, not square-only
        c->TextureFilterCaps = c->CubeTextureFilterCaps = c->VolumeTextureFilterCaps = 0xFFFFFFFFu;
        c->TextureAddressCaps = c->VolumeTextureAddressCaps = 0xFFFFFFFFu; c->LineCaps = 0x1F;
        c->MaxTextureWidth = c->MaxTextureHeight = 8192; c->MaxVolumeExtent = 2048; c->MaxTextureRepeat = 8192; c->MaxTextureAspectRatio = 8192;
        c->MaxAnisotropy = 16; c->MaxVertexW = 1e10f; c->StencilCaps = 0x1FF; c->FVFCaps = 0x00080008; c->TextureOpCaps = 0xFFFFFFFFu;
        c->MaxTextureBlendStages = 8; c->MaxSimultaneousTextures = 16; c->VertexProcessingCaps = 0x3DB;
        c->MaxActiveLights = 8; c->MaxUserClipPlanes = 6; c->MaxVertexBlendMatrices = 4; c->MaxPointSize = 64.f;
        c->MaxPrimitiveCount = 0xFFFFFF; c->MaxVertexIndex = 0xFFFFFF; c->MaxStreams = 16; c->MaxStreamStride = 508;
        c->VertexShaderVersion = 0xFFFE0300; c->MaxVertexShaderConst = 256; c->PixelShaderVersion = 0xFFFF0300; c->PixelShader1xMaxValue = 65504.f;
        c->DevCaps2 = 0xFF; c->DeclTypes = 0x3FF; c->NumSimultaneousRTs = 4; c->StretchRectFilterCaps = 0x03000300;
        c->MaxVShaderInstructionsExecuted = 65535; c->MaxPShaderInstructionsExecuted = 65535;
        c->MaxVertexShader30InstructionSlots = 32768; c->MaxPixelShader30InstructionSlots = 32768;
        return D3D_OK;
    }
    HMONITOR __stdcall GetAdapterMonitor(UINT) override { return (HMONITOR)(uintptr_t)1; }
    HRESULT __stdcall CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out) override
    {
        Device *d = new Device();
        if (!d->init(pp)) { d->Release(); return D3DERR_NOTAVAILABLE; }
        *out = d;
        return D3D_OK;
    }
};

} // namespace d3dgl

extern "C++" IDirect3D9 *Direct3DCreate9(UINT)
{
    return new d3dgl::D3D9Impl();
}
