// d3d9_gl_resources.cpp -- textures, surfaces, buffers, declarations, shaders and queries of the D3D9 -> WebGL2 device.
#include "d3d9_gl_internal.h"

#include <emscripten/html5.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace d3dgl {

// implemented in d3d9_gl_device.cpp
uint32_t NextSurfaceId();
void PurgeFBOs(uint32_t surfaceId);
void SetScratchUnit();       // glActiveTexture(scratch unit) keeping the device's active-unit cache consistent
void RestoreActiveUnit();
IDirect3DDevice9 *DeviceInterface(Device *d);
bool S3tcAvailable();

// ====================================================================== deferred GL deletes
static std::mutex g_delMutex;
static std::vector<GLuint> g_delIds[4];
static std::vector<uint32_t> g_delSurfaces;
static std::atomic<bool> g_delPending{false};
void DeferDelete(int kind, GLuint id) { if (!id) return; std::lock_guard<std::mutex> g(g_delMutex); g_delIds[kind].push_back(id); g_delPending = true; }
void DeferPurgeSurface(uint32_t id) { std::lock_guard<std::mutex> g(g_delMutex); g_delSurfaces.push_back(id); g_delPending = true; }
void DrainDeferredDeletes()
{
    if (!g_delPending.exchange(false)) return;
    std::vector<GLuint> ids[4]; std::vector<uint32_t> surfs;
    { std::lock_guard<std::mutex> g(g_delMutex); for (int i = 0; i < 4; ++i) ids[i].swap(g_delIds[i]); surfs.swap(g_delSurfaces); }
    for (uint32_t s : surfs) PurgeFBOs(s);
    if (!ids[0].empty()) glDeleteTextures((GLsizei)ids[0].size(), ids[0].data());
    if (!ids[1].empty()) glDeleteBuffers((GLsizei)ids[1].size(), ids[1].data());
    if (!ids[2].empty()) glDeleteRenderbuffers((GLsizei)ids[2].size(), ids[2].data());
    if (!ids[3].empty()) glDeleteQueries((GLsizei)ids[3].size(), ids[3].data());
}

// ====================================================================== formats
static FmtInfo MakeFmt(D3DFORMAT d3d, bool valid, int bpu, int conv, GLenum internalFmt, GLenum format, GLenum type)
{
    FmtInfo f;
    f.d3d = d3d; f.valid = valid; f.bytesPerUnit = bpu; f.conv = conv; f.internalFmt = internalFmt; f.format = format; f.type = type;
    return f;
}

const FmtInfo &GetFmt(D3DFORMAT d)
{
    static FmtInfo table[256];
    static bool inited = false;
    static FmtInfo invalid;
    static FmtInfo dxt1, dxt3, dxt5;
    if (!inited) {
        inited = true;
        auto set = [&](D3DFORMAT f, FmtInfo i) { table[(int)f] = i; };
        FmtInfo a;
        a = MakeFmt(D3DFMT_A8R8G8B8, true, 4, CONV_BGRA8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); a.renderable = true; set(D3DFMT_A8R8G8B8, a);
        a = MakeFmt(D3DFMT_X8R8G8B8, true, 4, CONV_BGRX8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); a.renderable = true; set(D3DFMT_X8R8G8B8, a);
        a = MakeFmt(D3DFMT_A8B8G8R8, true, 4, CONV_NONE, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); a.renderable = true; set(D3DFMT_A8B8G8R8, a);
        a = MakeFmt(D3DFMT_X8B8G8R8, true, 4, CONV_NONE, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); a.renderable = true; set(D3DFMT_X8B8G8R8, a);
        a = MakeFmt(D3DFMT_R5G6B5, true, 2, CONV_NONE, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5); a.renderable = true; set(D3DFMT_R5G6B5, a);
        a = MakeFmt(D3DFMT_X1R5G5B5, true, 2, CONV_X1R5G5B5, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); set(D3DFMT_X1R5G5B5, a);
        a = MakeFmt(D3DFMT_A1R5G5B5, true, 2, CONV_A1R5G5B5, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); set(D3DFMT_A1R5G5B5, a);
        a = MakeFmt(D3DFMT_A4R4G4B4, true, 2, CONV_A4R4G4B4, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); set(D3DFMT_A4R4G4B4, a);
        a = MakeFmt(D3DFMT_A8, true, 1, CONV_A8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); a.renderable = true; set(D3DFMT_A8, a);
        a = MakeFmt(D3DFMT_L8, true, 1, CONV_L8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); set(D3DFMT_L8, a);
        a = MakeFmt(D3DFMT_A8L8, true, 2, CONV_A8L8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); set(D3DFMT_A8L8, a);
        a = MakeFmt(D3DFMT_V8U8, true, 2, CONV_NONE, GL_RG8_SNORM, GL_RG, GL_BYTE); set(D3DFMT_V8U8, a);
        a = MakeFmt(D3DFMT_Q8W8V8U8, true, 4, CONV_NONE, GL_RGBA8_SNORM, GL_RGBA, GL_BYTE); set(D3DFMT_Q8W8V8U8, a);
        a = MakeFmt(D3DFMT_R16F, true, 2, CONV_NONE, GL_R16F, GL_RED, GL_HALF_FLOAT); a.floatFmt = a.renderable = true; set(D3DFMT_R16F, a);
        a = MakeFmt(D3DFMT_G16R16F, true, 4, CONV_NONE, GL_RG16F, GL_RG, GL_HALF_FLOAT); a.floatFmt = a.renderable = true; set(D3DFMT_G16R16F, a);
        a = MakeFmt(D3DFMT_A16B16G16R16F, true, 8, CONV_NONE, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT); a.floatFmt = a.renderable = true; set(D3DFMT_A16B16G16R16F, a);
        a = MakeFmt(D3DFMT_R32F, true, 4, CONV_NONE, GL_R32F, GL_RED, GL_FLOAT); a.floatFmt = a.renderable = true; set(D3DFMT_R32F, a);
        a = MakeFmt(D3DFMT_G32R32F, true, 8, CONV_NONE, GL_RG32F, GL_RG, GL_FLOAT); a.floatFmt = a.renderable = true; set(D3DFMT_G32R32F, a);
        a = MakeFmt(D3DFMT_A32B32G32R32F, true, 16, CONV_NONE, GL_RGBA32F, GL_RGBA, GL_FLOAT); a.floatFmt = a.renderable = true; set(D3DFMT_A32B32G32R32F, a);
        // depth / stencil
        auto dep = [&](D3DFORMAT f, int bpu, GLenum internal, GLenum format, GLenum type, bool st) {
            FmtInfo x = MakeFmt(f, true, bpu, CONV_NONE, internal, format, type); x.depth = true; x.stencil = st; x.renderable = true; set(f, x); };
        dep(D3DFMT_D16, 2, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, false);
        dep(D3DFMT_D16_LOCKABLE, 2, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, false);
        dep(D3DFMT_D24X8, 4, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, false);
        dep(D3DFMT_D24S8, 4, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, true);
        dep(D3DFMT_D24X4S4, 4, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, true);
        dep(D3DFMT_D15S1, 2, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, true);
        dep(D3DFMT_D32, 4, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, false);
        dep(D3DFMT_D32F_LOCKABLE, 4, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, false);
        dep(D3DFMT_D24FS8, 4, GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, true);
        for (FmtInfo *p : {&dxt1, &dxt3, &dxt5}) { p->valid = true; p->compressed = true; p->blockDim = 4; }
    }
    int idx = (int)d;
    if (idx >= 0 && idx < 256 && table[idx].valid) return table[idx];
    bool s3 = S3tcAvailable();
    auto mk = [&](FmtInfo &x, D3DFORMAT f, int bpu, int convIfSoft, GLenum internal) {
        x.d3d = f; x.bytesPerUnit = bpu;
        if (s3) { x.conv = CONV_NONE; x.internalFmt = internal; x.format = internal; x.type = 0; }
        else { x.conv = convIfSoft; x.internalFmt = GL_RGBA8; x.format = GL_RGBA; x.type = GL_UNSIGNED_BYTE; }
        return &x;
    };
    switch ((unsigned)d) {
    case D3DFMT_DXT1: return *mk(dxt1, D3DFMT_DXT1, 8, CONV_DXT1, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT_);
    case D3DFMT_DXT2: case D3DFMT_DXT3: return *mk(dxt3, D3DFMT_DXT3, 16, CONV_DXT3, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT_);
    case D3DFMT_DXT4: case D3DFMT_DXT5: return *mk(dxt5, D3DFMT_DXT5, 16, CONV_DXT5, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT_);
    }
    return invalid;
}

size_t LevelBytes(const FmtInfo &fi, UINT w, UINT h, UINT d, UINT *pitchOut, UINT *slicePitchOut)
{
    UINT pitch, slice;
    if (fi.compressed) {
        UINT bw = (w + 3) / 4, bh = (h + 3) / 4;
        if (!bw) bw = 1;
        if (!bh) bh = 1;
        pitch = bw * fi.bytesPerUnit;
        slice = pitch * bh;
    } else {
        pitch = w * fi.bytesPerUnit;
        slice = pitch * h;
    }
    if (pitchOut) *pitchOut = pitch;
    if (slicePitchOut) *slicePitchOut = slice;
    return (size_t)slice * (d ? d : 1);
}

// ---- software conversions (D3D memory layout -> GL upload layout)
static void DecodeDXTBlock(const uint8_t *b, int kind, uint8_t out[16][4])
{
    const uint8_t *cb = b + (kind == CONV_DXT1 ? 0 : 8);
    uint16_t c0 = cb[0] | (cb[1] << 8), c1 = cb[2] | (cb[3] << 8);
    auto expand = [](uint16_t c, int *r, int *g, int *bl) {
        *r = ((c >> 11) & 31) * 255 / 31; *g = ((c >> 5) & 63) * 255 / 63; *bl = (c & 31) * 255 / 31; };
    int col[4][4];
    expand(c0, &col[0][0], &col[0][1], &col[0][2]); col[0][3] = 255;
    expand(c1, &col[1][0], &col[1][1], &col[1][2]); col[1][3] = 255;
    if (c0 > c1 || kind != CONV_DXT1) {
        for (int k = 0; k < 3; ++k) { col[2][k] = (2 * col[0][k] + col[1][k]) / 3; col[3][k] = (col[0][k] + 2 * col[1][k]) / 3; }
        col[2][3] = col[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) { col[2][k] = (col[0][k] + col[1][k]) / 2; col[3][k] = 0; }
        col[2][3] = 255; col[3][3] = 0;
    }
    uint32_t bits = cb[4] | (cb[5] << 8) | (cb[6] << 16) | ((uint32_t)cb[7] << 24);
    for (int i = 0; i < 16; ++i) { int s = (bits >> (2 * i)) & 3; for (int k = 0; k < 4; ++k) out[i][k] = (uint8_t)col[s][k]; }
    if (kind == CONV_DXT3) {
        for (int i = 0; i < 16; ++i) { int nib = (b[i / 2] >> ((i & 1) * 4)) & 15; out[i][3] = (uint8_t)(nib * 17); }
    } else if (kind == CONV_DXT5) {
        int a0 = b[0], a1 = b[1], al[8];
        al[0] = a0; al[1] = a1;
        if (a0 > a1) for (int i = 1; i < 7; ++i) al[1 + i] = ((7 - i) * a0 + i * a1) / 7;
        else { for (int i = 1; i < 5; ++i) al[1 + i] = ((5 - i) * a0 + i * a1) / 5; al[6] = 0; al[7] = 255; }
        uint64_t ab = 0;
        for (int i = 0; i < 6; ++i) ab |= (uint64_t)b[2 + i] << (8 * i);
        for (int i = 0; i < 16; ++i) out[i][3] = (uint8_t)al[(ab >> (3 * i)) & 7];
    }
}

static void ConvertLevel(const FmtInfo &fi, const uint8_t *src, UINT w, UINT h, UINT srcPitch, std::vector<uint8_t> &out)
{
    out.resize((size_t)w * h * 4);
    uint8_t *o = out.data();
    switch (fi.conv) {
    case CONV_BGRA8: case CONV_BGRX8:
        for (UINT y = 0; y < h; ++y) { const uint8_t *s = src + (size_t)y * srcPitch; for (UINT x = 0; x < w; ++x, s += 4, o += 4) {
            o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = fi.conv == CONV_BGRA8 ? s[3] : 255; } }
        break;
    case CONV_A8:
        for (UINT y = 0; y < h; ++y) { const uint8_t *s = src + (size_t)y * srcPitch; for (UINT x = 0; x < w; ++x, ++s, o += 4) { o[0] = o[1] = o[2] = 0; o[3] = *s; } }
        break;
    case CONV_L8:
        for (UINT y = 0; y < h; ++y) { const uint8_t *s = src + (size_t)y * srcPitch; for (UINT x = 0; x < w; ++x, ++s, o += 4) { o[0] = o[1] = o[2] = *s; o[3] = 255; } }
        break;
    case CONV_A8L8:
        for (UINT y = 0; y < h; ++y) { const uint8_t *s = src + (size_t)y * srcPitch; for (UINT x = 0; x < w; ++x, s += 2, o += 4) { o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; } }
        break;
    case CONV_A4R4G4B4:
        for (UINT y = 0; y < h; ++y) { const uint16_t *s = (const uint16_t *)(src + (size_t)y * srcPitch); for (UINT x = 0; x < w; ++x, ++s, o += 4) {
            uint16_t v = *s; o[0] = ((v >> 8) & 15) * 17; o[1] = ((v >> 4) & 15) * 17; o[2] = (v & 15) * 17; o[3] = ((v >> 12) & 15) * 17; } }
        break;
    case CONV_A1R5G5B5: case CONV_X1R5G5B5:
        for (UINT y = 0; y < h; ++y) { const uint16_t *s = (const uint16_t *)(src + (size_t)y * srcPitch); for (UINT x = 0; x < w; ++x, ++s, o += 4) {
            uint16_t v = *s; o[0] = ((v >> 10) & 31) * 255 / 31; o[1] = ((v >> 5) & 31) * 255 / 31; o[2] = (v & 31) * 255 / 31;
            o[3] = fi.conv == CONV_A1R5G5B5 ? ((v >> 15) ? 255 : 0) : 255; } }
        break;
    case CONV_DXT1: case CONV_DXT3: case CONV_DXT5: {
        UINT bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (UINT by = 0; by < bh; ++by) for (UINT bx = 0; bx < bw; ++bx) {
            uint8_t px[16][4];
            DecodeDXTBlock(src + (size_t)by * srcPitch + (size_t)bx * fi.bytesPerUnit, fi.conv, px);
            for (UINT py = 0; py < 4; ++py) for (UINT pxl = 0; pxl < 4; ++pxl) {
                UINT x = bx * 4 + pxl, y = by * 4 + py;
                if (x < w && y < h) memcpy(&out[((size_t)y * w + x) * 4], px[py * 4 + pxl], 4);
            }
        }
        break; }
    default: break;
    }
}

// ====================================================================== TexCore
void TexCore::init(UINT w_, UINT h_, UINT d_, UINT levels_, D3DFORMAT f, DWORD u, D3DPOOL p, int faces_)
{
    w = w_; h = h_; d = d_ ? d_ : 1; fmt = f; usage = u; pool = p; faces = faces_;
    UINT maxDim = w > h ? w : h;
    if (d > maxDim) maxDim = d;
    UINT full = 1;
    while ((maxDim >> (full - 1)) > 1) ++full;
    levels = levels_ == 0 ? full : levels_;
    if (levels > full) levels = full;
    target = faces == 6 ? GL_TEXTURE_CUBE_MAP : d > 1 ? GL_TEXTURE_3D : GL_TEXTURE_2D;
    lv.resize((size_t)faces * levels);
}

void TexCore::levelDims(UINT level, UINT *lw, UINT *lh, UINT *ld) const
{
    UINT a = w >> level, b = h >> level, c = d >> level;
    if (lw) *lw = a ? a : 1;
    if (lh) *lh = b ? b : 1;
    if (ld) *ld = c ? c : 1;
}

HRESULT TexCore::lockLevel(int face, UINT level, void **bits, INT *pitch, INT *slicePitch, bool readOnly)
{
    if (level >= levels || face < 0 || face >= faces) return D3DERR_INVALIDCALL;
    std::lock_guard<std::mutex> g(lock);
    const FmtInfo &fi = GetFmt(fmt);
    if (!fi.valid) return D3DERR_INVALIDCALL;
    UINT lw, lh, ld, p, sp;
    levelDims(level, &lw, &lh, &ld);
    size_t bytes = LevelBytes(fi, lw, lh, d > 1 ? ld : 1, &p, &sp);
    Level &L = lv[(size_t)face * levels + level];
    if (L.data.size() != bytes) L.data.assign(bytes, 0);
    if (!readOnly) L.dirty = true;
    *bits = L.data.data();
    if (pitch) *pitch = (INT)p;
    if (slicePitch) *slicePitch = (INT)sp;
    return D3D_OK;
}

void TexCore::unlockLevel(int, UINT) {}

void TexCore::destroyGL()
{
    if (tex) { DeferDelete(0, tex); tex = 0; }
}

void TexCore::ensureGL()
{
    // The texture's dirty levels are uploaded on the GL thread; the CPU copy is dropped afterwards unless the texture lives in system memory.
    std::lock_guard<std::mutex> g(lock);
    if (glFailed) return;
    const FmtInfo &fi = GetFmt(fmt);
    if (!fi.valid) { glFailed = true; Log("texture format %d unsupported", (int)fmt); return; }
    SetScratchUnit();
    if (!tex) {
        glGenTextures(1, &tex);
        glBindTexture(target, tex);
        GLenum internal = fi.internalFmt;
        if (target == GL_TEXTURE_3D) glTexStorage3D(target, (GLsizei)levels, internal, (GLsizei)w, (GLsizei)h, (GLsizei)d);
        else glTexStorage2D(target, (GLsizei)levels, internal, (GLsizei)w, (GLsizei)h);
        if (!fi.depth) glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    } else {
        glBindTexture(target, tex);
    }
    bool freeAfter = pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_SCRATCH;
    std::vector<uint8_t> tmp;
    for (int face = 0; face < faces; ++face) {
        for (UINT level = 0; level < levels; ++level) {
            Level &L = lv[(size_t)face * levels + level];
            if (!L.dirty || L.data.empty()) continue;
            UINT lw, lh, ld, pitch, slice;
            levelDims(level, &lw, &lh, &ld);
            LevelBytes(fi, lw, lh, d > 1 ? ld : 1, &pitch, &slice);
            GLenum tgt = faces == 6 ? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face) : target;
            const void *data = L.data.data();
            if (fi.conv != CONV_NONE) {
                if (d > 1) { tmp.clear(); std::vector<uint8_t> one; for (UINT z = 0; z < ld; ++z) { ConvertLevel(fi, L.data.data() + (size_t)z * slice, lw, lh, pitch, one); tmp.insert(tmp.end(), one.begin(), one.end()); } }
                else ConvertLevel(fi, L.data.data(), lw, lh, pitch, tmp);
                data = tmp.data();
            }
            if (d > 1) {
                glTexSubImage3D(tgt, (GLint)level, 0, 0, 0, (GLsizei)lw, (GLsizei)lh, (GLsizei)ld, fi.format, fi.type, data);
            } else if (fi.compressed && fi.conv == CONV_NONE) {
                glCompressedTexSubImage2D(tgt, (GLint)level, 0, 0, (GLsizei)lw, (GLsizei)lh, fi.internalFmt, (GLsizei)(size_t)slice, data);
            } else {
                glTexSubImage2D(tgt, (GLint)level, 0, 0, (GLsizei)lw, (GLsizei)lh, fi.format, fi.type, data);
            }
            L.dirty = false;
            if (freeAfter) { std::vector<uint8_t>().swap(L.data); }
        }
    }
    RestoreActiveUnit();
}

// ====================================================================== Surface
Surface::Surface(Device *d) : dev(d), id(NextSurfaceId()) {}

Surface::~Surface()
{
    DeferPurgeSurface(id);
    if (rbo) DeferDelete(2, rbo);
    if (owner) owner->Release();
}

bool Surface::isDepth() const { return GetFmt(fmt).depth; }
bool Surface::hasStencil() const { return GetFmt(fmt).stencil; }

void Surface::ensureGL()
{
    if (kind == K_TEXLEVEL) { if (tex) tex->ensureGL(); return; }
    if (kind == K_PLAIN || rbo) return;
    const FmtInfo &fi = GetFmt(fmt);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, fi.valid ? fi.internalFmt : GL_RGBA8, (GLsizei)w, (GLsizei)h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

HRESULT Surface::GetDevice(IDirect3DDevice9 **out) { *out = DeviceInterface(dev); (*out)->AddRef(); return D3D_OK; }

HRESULT Surface::GetDesc(D3DSURFACE_DESC *desc)
{
    desc->Format = fmt; desc->Type = D3DRTYPE_SURFACE; desc->Usage = usage; desc->Pool = pool;
    desc->MultiSampleType = D3DMULTISAMPLE_NONE; desc->MultiSampleQuality = 0; desc->Width = w; desc->Height = h;
    return D3D_OK;
}

HRESULT Surface::LockRect(D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    if (kind == K_TEXLEVEL && tex) {
        void *bits; INT pitch;
        HRESULT hr = tex->lockLevel(face, level, &bits, &pitch, nullptr, (flags & D3DLOCK_READONLY) != 0);
        if (hr < 0) return hr;
        lr->pBits = bits; lr->Pitch = pitch;
        if (rect) { const FmtInfo &fi = GetFmt(fmt); lr->pBits = (uint8_t *)bits + (fi.compressed ? (rect->top / 4) * pitch + (rect->left / 4) * fi.bytesPerUnit : rect->top * pitch + rect->left * fi.bytesPerUnit); }
        return D3D_OK;
    }
    if (kind == K_PLAIN) {
        const FmtInfo &fi = GetFmt(fmt);
        UINT pitch;
        size_t bytes = LevelBytes(fi, w, h, 1, &pitch, nullptr);
        if (cpu.size() != bytes) cpu.assign(bytes, 0);
        lr->Pitch = (INT)pitch;
        lr->pBits = cpu.data() + (rect ? (size_t)rect->top * pitch + (size_t)rect->left * fi.bytesPerUnit : 0);
        return D3D_OK;
    }
    return D3DERR_INVALIDCALL;   // render targets / depth surfaces are not lockable
}

HRESULT Surface::UnlockRect() { return D3D_OK; }

// ====================================================================== Texture2D
HRESULT Texture2D::GetDevice(IDirect3DDevice9 **out) { *out = DeviceInterface(dev); (*out)->AddRef(); return D3D_OK; }
Texture2D::~Texture2D() { core.destroyGL(); }
void Texture2D::GenerateMipSubLevels() {}
HRESULT Texture2D::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc)
{
    if (level >= core.levels) return D3DERR_INVALIDCALL;
    UINT lw, lh; core.levelDims(level, &lw, &lh, nullptr);
    desc->Format = core.fmt; desc->Type = D3DRTYPE_SURFACE; desc->Usage = core.usage; desc->Pool = core.pool;
    desc->MultiSampleType = D3DMULTISAMPLE_NONE; desc->MultiSampleQuality = 0; desc->Width = lw; desc->Height = lh;
    return D3D_OK;
}
HRESULT Texture2D::GetSurfaceLevel(UINT level, IDirect3DSurface9 **out)
{
    if (level >= core.levels) return D3DERR_INVALIDCALL;
    Surface *s = new Surface(dev);
    s->kind = Surface::K_TEXLEVEL;
    s->fmt = core.fmt; s->usage = core.usage; s->pool = core.pool;
    core.levelDims(level, &s->w, &s->h, nullptr);
    s->owner = this; AddRef();
    s->tex = &core; s->face = 0; s->level = level;
    *out = s;
    return D3D_OK;
}
HRESULT Texture2D::LockRect(UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    void *bits; INT pitch;
    HRESULT hr = core.lockLevel(0, level, &bits, &pitch, nullptr, (flags & D3DLOCK_READONLY) != 0);
    if (hr < 0) return hr;
    const FmtInfo &fi = GetFmt(core.fmt);
    lr->Pitch = pitch;
    lr->pBits = bits;
    if (rect) lr->pBits = (uint8_t *)bits + (fi.compressed ? (rect->top / 4) * pitch + (rect->left / 4) * fi.bytesPerUnit : rect->top * pitch + rect->left * fi.bytesPerUnit);
    return D3D_OK;
}
HRESULT Texture2D::UnlockRect(UINT level) { core.unlockLevel(0, level); return D3D_OK; }

// ====================================================================== TextureCube
HRESULT TextureCube::GetDevice(IDirect3DDevice9 **out) { *out = DeviceInterface(dev); (*out)->AddRef(); return D3D_OK; }
TextureCube::~TextureCube() { core.destroyGL(); }
void TextureCube::GenerateMipSubLevels() {}
HRESULT TextureCube::GetLevelDesc(UINT level, D3DSURFACE_DESC *desc)
{
    if (level >= core.levels) return D3DERR_INVALIDCALL;
    UINT lw, lh; core.levelDims(level, &lw, &lh, nullptr);
    desc->Format = core.fmt; desc->Type = D3DRTYPE_SURFACE; desc->Usage = core.usage; desc->Pool = core.pool;
    desc->MultiSampleType = D3DMULTISAMPLE_NONE; desc->MultiSampleQuality = 0; desc->Width = lw; desc->Height = lh;
    return D3D_OK;
}
HRESULT TextureCube::GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **out)
{
    if (level >= core.levels || (int)face > 5) return D3DERR_INVALIDCALL;
    Surface *s = new Surface(dev);
    s->kind = Surface::K_TEXLEVEL;
    s->fmt = core.fmt; s->usage = core.usage; s->pool = core.pool;
    core.levelDims(level, &s->w, &s->h, nullptr);
    s->owner = this; AddRef();
    s->tex = &core; s->face = (int)face; s->level = level;
    *out = s;
    return D3D_OK;
}
HRESULT TextureCube::LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    void *bits; INT pitch;
    HRESULT hr = core.lockLevel((int)face, level, &bits, &pitch, nullptr, (flags & D3DLOCK_READONLY) != 0);
    if (hr < 0) return hr;
    const FmtInfo &fi = GetFmt(core.fmt);
    lr->Pitch = pitch;
    lr->pBits = bits;
    if (rect) lr->pBits = (uint8_t *)bits + (fi.compressed ? (rect->top / 4) * pitch + (rect->left / 4) * fi.bytesPerUnit : rect->top * pitch + rect->left * fi.bytesPerUnit);
    return D3D_OK;
}
HRESULT TextureCube::UnlockRect(D3DCUBEMAP_FACES face, UINT level) { core.unlockLevel((int)face, level); return D3D_OK; }

// ====================================================================== TextureVolume
HRESULT TextureVolume::GetDevice(IDirect3DDevice9 **out) { *out = DeviceInterface(dev); (*out)->AddRef(); return D3D_OK; }
TextureVolume::~TextureVolume() { core.destroyGL(); }
HRESULT TextureVolume::GetLevelDesc(UINT level, D3DVOLUME_DESC *desc)
{
    if (level >= core.levels) return D3DERR_INVALIDCALL;
    UINT lw, lh, ld; core.levelDims(level, &lw, &lh, &ld);
    desc->Format = core.fmt; desc->Type = D3DRTYPE_VOLUME; desc->Usage = core.usage; desc->Pool = core.pool;
    desc->Width = lw; desc->Height = lh; desc->Depth = ld;
    return D3D_OK;
}
HRESULT TextureVolume::LockBox(UINT level, D3DLOCKED_BOX *lb, const D3DBOX *, DWORD flags)
{
    void *bits; INT pitch, slice;
    HRESULT hr = core.lockLevel(0, level, &bits, &pitch, &slice, (flags & D3DLOCK_READONLY) != 0);
    if (hr < 0) return hr;
    lb->pBits = bits; lb->RowPitch = pitch; lb->SlicePitch = slice;
    return D3D_OK;
}
HRESULT TextureVolume::UnlockBox(UINT level) { core.unlockLevel(0, level); return D3D_OK; }

// ====================================================================== buffers
void BufferCore::sync()
{
    std::lock_guard<std::mutex> g(lock);
    if (!created) {
        glGenBuffers(1, &buf);
        glBindBuffer(target, buf);
        glBufferData(target, (GLsizeiptr)cpu.size(), nullptr, (usage & D3DUSAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        created = true;
        dirtyLo = 0; dirtyHi = cpu.size();
    } else {
        glBindBuffer(target, buf);
    }
    if (dirtyHi > dirtyLo && dirtyLo != (size_t)-1) {
        size_t hi = dirtyHi > cpu.size() ? cpu.size() : dirtyHi;
        if (hi > dirtyLo) glBufferSubData(target, (GLintptr)dirtyLo, (GLsizeiptr)(hi - dirtyLo), cpu.data() + dirtyLo);
    }
    dirtyLo = (size_t)-1; dirtyHi = 0;
}

void BufferCore::destroyGL()
{
    if (buf) { DeferDelete(1, buf); buf = 0; }
    created = false;
}

HRESULT VertexBuffer::GetDevice(IDirect3DDevice9 **out) { *out = DeviceInterface(dev); (*out)->AddRef(); return D3D_OK; }
VertexBuffer::~VertexBuffer() { core.destroyGL(); }
HRESULT VertexBuffer::Lock(UINT offset, UINT size, void **data, DWORD flags)
{
    if (offset > core.cpu.size()) return D3DERR_INVALIDCALL;
    if (!size || offset + size > core.cpu.size()) size = (UINT)core.cpu.size() - offset;
    lockOffset = offset; lockSize = (flags & D3DLOCK_READONLY) ? 0 : size;
    *data = core.cpu.data() + offset;
    return D3D_OK;
}
HRESULT VertexBuffer::Unlock() { if (lockSize) core.touch(lockOffset, (size_t)lockOffset + lockSize); lockSize = 0; return D3D_OK; }
HRESULT VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *d)
{ d->Format = D3DFMT_VERTEXDATA; d->Type = D3DRTYPE_VERTEXBUFFER; d->Usage = usage; d->Pool = pool; d->Size = (UINT)core.cpu.size(); d->FVF = fvf; return D3D_OK; }

HRESULT IndexBuffer::GetDevice(IDirect3DDevice9 **out) { *out = DeviceInterface(dev); (*out)->AddRef(); return D3D_OK; }
IndexBuffer::~IndexBuffer() { core.destroyGL(); }
HRESULT IndexBuffer::Lock(UINT offset, UINT size, void **data, DWORD flags)
{
    if (offset > core.cpu.size()) return D3DERR_INVALIDCALL;
    if (!size || offset + size > core.cpu.size()) size = (UINT)core.cpu.size() - offset;
    lockOffset = offset; lockSize = (flags & D3DLOCK_READONLY) ? 0 : size;
    *data = core.cpu.data() + offset;
    return D3D_OK;
}
HRESULT IndexBuffer::Unlock() { if (lockSize) core.touch(lockOffset, (size_t)lockOffset + lockSize); lockSize = 0; return D3D_OK; }
HRESULT IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *d)
{ d->Format = fmt; d->Type = D3DRTYPE_INDEXBUFFER; d->Usage = usage; d->Pool = pool; d->Size = (UINT)core.cpu.size(); return D3D_OK; }

// ====================================================================== declarations / shaders
HRESULT VertexDecl::GetDeclaration(D3DVERTEXELEMENT9 *e, UINT *count)
{
    if (count) *count = (UINT)elems.size() + 1;
    if (e) { for (size_t i = 0; i < elems.size(); ++i) e[i] = elems[i]; D3DVERTEXELEMENT9 end = D3DDECL_END(); e[elems.size()] = end; }
    return D3D_OK;
}
HRESULT VertexShader::GetFunction(void *data, UINT *size)
{
    UINT bytes = (UINT)code.size() * 4;
    if (!data) { *size = bytes; return D3D_OK; }
    memcpy(data, code.data(), bytes < *size ? bytes : *size);
    return D3D_OK;
}
HRESULT PixelShader::GetFunction(void *data, UINT *size)
{
    UINT bytes = (UINT)code.size() * 4;
    if (!data) { *size = bytes; return D3D_OK; }
    memcpy(data, code.data(), bytes < *size ? bytes : *size);
    return D3D_OK;
}

// ====================================================================== queries
Query::~Query()
{
    // GLsync objects are owned by the GL thread; a leaked fence is harmless, queries are queued
    if (q) DeferDelete(3, q);
}
HRESULT Query::Issue(DWORD flags)
{
    if (type == D3DQUERYTYPE_EVENT) {
        if (flags & D3DISSUE_END) {
            if (sync) glDeleteSync(sync);
            sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
            issued = true;
        }
    } else if (type == D3DQUERYTYPE_OCCLUSION) {
        if (!q) glGenQueries(1, &q);
        if (flags & D3DISSUE_BEGIN) glBeginQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE, q);
        if (flags & D3DISSUE_END) { glEndQuery(GL_ANY_SAMPLES_PASSED_CONSERVATIVE); issued = true; }
    } else {
        issued = true;
    }
    return D3D_OK;
}
HRESULT Query::GetData(void *data, DWORD size, DWORD)
{
    // WebGL never makes query/fence results visible within the task that issued them, and the engine polls these in
    // spin loops (GPU sync, sun-sprite calibration). So: report completion immediately; occlusion counts use the GL
    // result when it happens to be available and 0 ("not visible") otherwise.
    if (type == D3DQUERYTYPE_EVENT) {
        if (data && size >= 4) *(DWORD *)data = TRUE;
        return D3D_OK;
    }
    if (type == D3DQUERYTYPE_OCCLUSION) {
        DWORD res = 0;
        if (q && issued) {
            GLuint avail = 0;
            glGetQueryObjectuiv(q, GL_QUERY_RESULT_AVAILABLE, &avail);
            if (avail) { GLuint r = 0; glGetQueryObjectuiv(q, GL_QUERY_RESULT, &r); res = r ? 4096 : 0; }
        }
        if (data && size >= 4) *(DWORD *)data = res;
        return D3D_OK;
    }
    if (data && size >= 4) *(DWORD *)data = 0;
    return D3D_OK;
}

} // namespace d3dgl
