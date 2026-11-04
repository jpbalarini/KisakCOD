// d3d9_gl_internal.h -- internal declarations for the Direct3D 9 -> WebGL2 device (see d3d9_gl.cpp).
//
// Threading model: the engine creates D3D resources on its fastfile (DB) thread, but a WebGL2 context belongs to exactly one
// thread. So resource creation/Lock/Unlock only touch CPU memory; GL objects are created and filled lazily by the thread that
// owns the context (the engine's main/render thread) the first time a resource is *used* (SetTexture/SetStreamSource/...).
#pragma once
#include <Windows.h>
#include <d3d9.h>

#include <GLES3/gl3.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "shader_xlate.h"

namespace d3dgl {

void Log(const char *fmt, ...);

// ---- GL extension tokens that gl3.h does not carry
enum : GLenum {
    GL_COMPRESSED_RGB_S3TC_DXT1_EXT_ = 0x83F0, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT_ = 0x83F1,
    GL_COMPRESSED_RGBA_S3TC_DXT3_EXT_ = 0x83F2, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT_ = 0x83F3,
    GL_TEXTURE_MAX_ANISOTROPY_EXT_ = 0x84FE, GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT_ = 0x84FF,
};

struct FmtInfo {
    D3DFORMAT d3d = D3DFMT_UNKNOWN;
    bool valid = false;
    bool compressed = false;   // block compressed in D3D memory
    bool depth = false, stencil = false;
    bool floatFmt = false;
    int bytesPerUnit = 0;      // bytes per pixel (or per 4x4 block when compressed)
    int blockDim = 1;          // 4 for DXT
    int conv = 0;              // conversion from D3D memory layout to GL upload layout (see ConvKind)
    GLenum internalFmt = 0, format = 0, type = 0;   // GL layout *after* conversion
    bool renderable = false;
};
enum ConvKind {
    CONV_NONE = 0, CONV_BGRA8, CONV_BGRX8, CONV_A8, CONV_L8, CONV_A8L8, CONV_A4R4G4B4, CONV_A1R5G5B5, CONV_X1R5G5B5,
    CONV_DXT1, CONV_DXT3, CONV_DXT5  // software decode, only when S3TC is unavailable
};
const FmtInfo &GetFmt(D3DFORMAT f);
bool S3tcSupported();

// size in bytes of one mip level as it is laid out in D3D memory (tight pitch); also returns the row pitch
size_t LevelBytes(const FmtInfo &fi, UINT w, UINT h, UINT d, UINT *pitchOut, UINT *slicePitchOut);

struct Device;

// GL objects are only ever deleted by the GL thread: destructors (which may run on the fastfile thread) queue the ids here and
// the device drains the queue (DrainDeferredDeletes) at the start of every frame / draw batch.
void DeferDelete(int kind, GLuint id);      // kind: 0 texture, 1 buffer, 2 renderbuffer, 3 query
void DeferPurgeSurface(uint32_t surfaceId); // drop cached FBOs referring to the surface
void DrainDeferredDeletes();                // GL thread

// ---------------------------------------------------------------- base refcount
template <class I>
struct RefImpl : I {
    std::atomic<int> refs{1};
    ULONG __stdcall AddRef() override { return (ULONG)++refs; }
    ULONG __stdcall Release() override {
        int r = --refs;
        if (r == 0) { delete this; return 0; }
        return (ULONG)r;
    }
    virtual ~RefImpl() {}
};

// ---------------------------------------------------------------- textures
struct TexCore {
    UINT w = 0, h = 0, d = 1, levels = 1;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    DWORD usage = 0;
    D3DPOOL pool = D3DPOOL_MANAGED;
    int faces = 1;          // 6 for cube maps
    GLenum target = GL_TEXTURE_2D;
    GLuint tex = 0;         // created lazily on the GL thread
    bool glFailed = false;
    DWORD lod = 0;
    struct Level { std::vector<uint8_t> data; bool dirty = false; };
    std::vector<Level> lv;  // faces * levels, index = face * levels + level
    std::mutex lock;

    void init(UINT w_, UINT h_, UINT d_, UINT levels_, D3DFORMAT f, DWORD u, D3DPOOL p, int faces_);
    void levelDims(UINT level, UINT *lw, UINT *lh, UINT *ld) const;
    UINT levelCount() const { return levels; }
    // returns pointer + pitch to the CPU copy of a level (allocating it if needed); marks it dirty unless readOnly
    HRESULT lockLevel(int face, UINT level, void **bits, INT *pitch, INT *slicePitch, bool readOnly);
    void unlockLevel(int face, UINT level);
    void ensureGL();        // GL thread only: create GL texture and upload dirty levels
    bool isRenderTarget() const { return (usage & D3DUSAGE_RENDERTARGET) != 0; }
    bool isDepth() const { return (usage & D3DUSAGE_DEPTHSTENCIL) != 0; }
    void destroyGL();
};

struct Surface;

struct Texture2D : RefImpl<IDirect3DTexture9> {
    TexCore core;
    Device *dev;
    Texture2D(Device *d) : dev(d) {}
    ~Texture2D() override;
    HRESULT __stdcall GetDevice(IDirect3DDevice9 **dev) override;
    DWORD __stdcall SetPriority(DWORD) override { return 0; }
    void __stdcall PreLoad() override {}
    D3DRESOURCETYPE __stdcall GetType() override { return D3DRTYPE_TEXTURE; }
    DWORD __stdcall GetLevelCount() override { return core.levels; }
    DWORD __stdcall SetLOD(DWORD l) override { DWORD o = core.lod; core.lod = l; return o; }
    DWORD __stdcall GetLOD() override { return core.lod; }
    void __stdcall GenerateMipSubLevels() override;
    HRESULT __stdcall GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) override;
    HRESULT __stdcall GetSurfaceLevel(UINT level, IDirect3DSurface9 **surf) override;
    HRESULT __stdcall LockRect(UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) override;
    HRESULT __stdcall UnlockRect(UINT level) override;
    HRESULT __stdcall AddDirtyRect(const RECT *) override { return D3D_OK; }
};

struct TextureCube : RefImpl<IDirect3DCubeTexture9> {
    TexCore core;
    Device *dev;
    TextureCube(Device *d) : dev(d) {}
    ~TextureCube() override;
    HRESULT __stdcall GetDevice(IDirect3DDevice9 **dev) override;
    DWORD __stdcall SetPriority(DWORD) override { return 0; }
    void __stdcall PreLoad() override {}
    D3DRESOURCETYPE __stdcall GetType() override { return D3DRTYPE_CUBETEXTURE; }
    DWORD __stdcall GetLevelCount() override { return core.levels; }
    DWORD __stdcall SetLOD(DWORD l) override { DWORD o = core.lod; core.lod = l; return o; }
    DWORD __stdcall GetLOD() override { return core.lod; }
    void __stdcall GenerateMipSubLevels() override;
    HRESULT __stdcall GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) override;
    HRESULT __stdcall GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **surf) override;
    HRESULT __stdcall LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) override;
    HRESULT __stdcall UnlockRect(D3DCUBEMAP_FACES face, UINT level) override;
};

struct TextureVolume : RefImpl<IDirect3DVolumeTexture9> {
    TexCore core;
    Device *dev;
    TextureVolume(Device *d) : dev(d) {}
    ~TextureVolume() override;
    HRESULT __stdcall GetDevice(IDirect3DDevice9 **dev) override;
    DWORD __stdcall SetPriority(DWORD) override { return 0; }
    void __stdcall PreLoad() override {}
    D3DRESOURCETYPE __stdcall GetType() override { return D3DRTYPE_VOLUMETEXTURE; }
    DWORD __stdcall GetLevelCount() override { return core.levels; }
    DWORD __stdcall SetLOD(DWORD l) override { DWORD o = core.lod; core.lod = l; return o; }
    DWORD __stdcall GetLOD() override { return core.lod; }
    void __stdcall GenerateMipSubLevels() override {}
    HRESULT __stdcall GetLevelDesc(UINT level, D3DVOLUME_DESC *desc) override;
    HRESULT __stdcall LockBox(UINT level, D3DLOCKED_BOX *lb, const D3DBOX *box, DWORD flags) override;
    HRESULT __stdcall UnlockBox(UINT level) override;
    HRESULT __stdcall AddDirtyBox(const D3DBOX *) override { return D3D_OK; }
};

// A render-target / depth-stencil / texture-level / offscreen-plain surface.
struct Surface : RefImpl<IDirect3DSurface9> {
    enum Kind { K_TEXLEVEL, K_RENDERBUFFER, K_PLAIN } kind = K_RENDERBUFFER;
    Device *dev;
    uint32_t id;
    UINT w = 0, h = 0;
    D3DFORMAT fmt = D3DFMT_UNKNOWN;
    DWORD usage = 0;
    D3DPOOL pool = D3DPOOL_DEFAULT;
    IUnknown *owner = nullptr;   // texture holding the level (ref held)
    TexCore *tex = nullptr;
    int face = 0;
    UINT level = 0;
    GLuint rbo = 0;              // K_RENDERBUFFER
    bool isBackbuffer = false;
    std::vector<uint8_t> cpu;    // K_PLAIN
    Surface(Device *d);
    ~Surface() override;
    bool isDepth() const;
    bool hasStencil() const;
    void ensureGL();             // GL thread
    HRESULT __stdcall GetDevice(IDirect3DDevice9 **dev) override;
    DWORD __stdcall SetPriority(DWORD) override { return 0; }
    void __stdcall PreLoad() override {}
    D3DRESOURCETYPE __stdcall GetType() override { return D3DRTYPE_SURFACE; }
    HRESULT __stdcall GetDesc(D3DSURFACE_DESC *desc) override;
    HRESULT __stdcall LockRect(D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) override;
    HRESULT __stdcall UnlockRect() override;
};

// ---------------------------------------------------------------- buffers
struct BufferCore {
    std::vector<uint8_t> cpu;
    GLuint buf = 0;
    GLenum target = GL_ARRAY_BUFFER;
    size_t dirtyLo = (size_t)-1, dirtyHi = 0;
    bool created = false;
    std::mutex lock;
    DWORD usage = 0;
    void init(UINT size, GLenum tgt, DWORD u) { cpu.assign(size, 0); target = tgt; usage = u; dirtyLo = 0; dirtyHi = size; }
    void touch(size_t lo, size_t hi) { std::lock_guard<std::mutex> g(lock); if (lo < dirtyLo) dirtyLo = lo; if (hi > dirtyHi) dirtyHi = hi; }
    void sync();     // GL thread: create + upload dirty range
    void destroyGL();
};

struct VertexBuffer : RefImpl<IDirect3DVertexBuffer9> {
    BufferCore core; Device *dev; DWORD fvf = 0; DWORD usage = 0; D3DPOOL pool = D3DPOOL_DEFAULT;
    UINT lockOffset = 0, lockSize = 0;
    VertexBuffer(Device *d) : dev(d) {}
    ~VertexBuffer() override;
    HRESULT __stdcall GetDevice(IDirect3DDevice9 **dev) override;
    DWORD __stdcall SetPriority(DWORD) override { return 0; }
    void __stdcall PreLoad() override {}
    D3DRESOURCETYPE __stdcall GetType() override { return D3DRTYPE_VERTEXBUFFER; }
    HRESULT __stdcall Lock(UINT offset, UINT size, void **data, DWORD flags) override;
    HRESULT __stdcall Unlock() override;
    HRESULT __stdcall GetDesc(D3DVERTEXBUFFER_DESC *desc) override;
};
struct IndexBuffer : RefImpl<IDirect3DIndexBuffer9> {
    BufferCore core; Device *dev; D3DFORMAT fmt = D3DFMT_INDEX16; DWORD usage = 0; D3DPOOL pool = D3DPOOL_DEFAULT;
    UINT lockOffset = 0, lockSize = 0;
    IndexBuffer(Device *d) : dev(d) {}
    ~IndexBuffer() override;
    HRESULT __stdcall GetDevice(IDirect3DDevice9 **dev) override;
    DWORD __stdcall SetPriority(DWORD) override { return 0; }
    void __stdcall PreLoad() override {}
    D3DRESOURCETYPE __stdcall GetType() override { return D3DRTYPE_INDEXBUFFER; }
    HRESULT __stdcall Lock(UINT offset, UINT size, void **data, DWORD flags) override;
    HRESULT __stdcall Unlock() override;
    HRESULT __stdcall GetDesc(D3DINDEXBUFFER_DESC *desc) override;
};

// ---------------------------------------------------------------- vertex declaration, shaders, query
struct VertexDecl : RefImpl<IDirect3DVertexDeclaration9> {
    std::vector<D3DVERTEXELEMENT9> elems;   // without the end marker
    HRESULT __stdcall GetDeclaration(D3DVERTEXELEMENT9 *e, UINT *count) override;
};

struct ShaderBase {
    uint64_t uid = 0;        // unique id (program-cache key); pointers may be reused
    uint64_t outSig = 0;     // VS: hash of the varyings it writes
    std::vector<uint32_t> code;
    bool translated = false, failed = false;
    std::string glsl, error;
    d3dxlate::Info info;
};
struct VertexShader : RefImpl<IDirect3DVertexShader9>, ShaderBase {
    GLuint glShader = 0;
    HRESULT __stdcall GetFunction(void *data, UINT *size) override;
};
struct PixelShader : RefImpl<IDirect3DPixelShader9>, ShaderBase {
    // GL fragment shader objects keyed by (vertex-output signature, shadow-sampler mask)
    std::map<std::pair<uint64_t, uint32_t>, GLuint> glShaders;
    HRESULT __stdcall GetFunction(void *data, UINT *size) override;
};

struct Query : RefImpl<IDirect3DQuery9> {
    D3DQUERYTYPE type; Device *dev;
    GLsync sync = nullptr; GLuint q = 0; bool issued = false; DWORD result = 0;
    Query(Device *d, D3DQUERYTYPE t) : type(t), dev(d) {}
    ~Query() override;
    D3DQUERYTYPE __stdcall GetType() override { return type; }
    DWORD __stdcall GetDataSize() override { return type == D3DQUERYTYPE_OCCLUSION ? 4 : 1; }
    HRESULT __stdcall Issue(DWORD flags) override;
    HRESULT __stdcall GetData(void *data, DWORD size, DWORD flags) override;
};

struct SwapChain : RefImpl<IDirect3DSwapChain9> {
    Device *dev;
    SwapChain(Device *d) : dev(d) {}
    HRESULT __stdcall Present(const RECT *src, const RECT *dst, HWND wnd, const void *dirty, DWORD flags) override;
    HRESULT __stdcall GetBackBuffer(UINT idx, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surf) override;
    HRESULT __stdcall GetFrontBufferData(IDirect3DSurface9 *dst) override;
};

} // namespace d3dgl
