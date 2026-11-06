// d3d9.h -- Direct3D 9 interface shim for the web build.
//
// This is NOT Microsoft's header. It declares the subset of the D3D9 API that the KisakCOD
// renderer (src/gfx_d3d) uses, with the real D3D9 numeric values (the decompiled code passes many
// raw numeric constants to SetRenderState/SetSamplerState etc, so the numbering must match).
//
// Milestone M1 provides a concrete `IDirect3DDevice9` implementation on top of WebGL2; for M0
// (fastfile loading) the interfaces only need to exist so the engine compiles.
#pragma once

#include <Windows.h>

#ifdef __cplusplus

// ---------------------------------------------------------------- COM-ish base
struct IUnknown {
    virtual ULONG __stdcall AddRef() = 0;
    virtual ULONG __stdcall Release() = 0;
protected:
    virtual ~IUnknown() {}
};

#define MAKEFOURCC(ch0, ch1, ch2, ch3) \
    ((DWORD)(BYTE)(ch0) | ((DWORD)(BYTE)(ch1) << 8) | ((DWORD)(BYTE)(ch2) << 16) | ((DWORD)(BYTE)(ch3) << 24))

// ---------------------------------------------------------------- constants
#define D3D_SDK_VERSION 32
#define D3D_OK S_OK
#define D3DADAPTER_DEFAULT 0
#define MAKE_D3DHRESULT(code) ((HRESULT)(0x88760000 | (code)))
#define D3DERR_DEVICELOST MAKE_D3DHRESULT(2152)
#define D3DERR_DEVICENOTRESET MAKE_D3DHRESULT(2153)
#define D3DERR_INVALIDCALL MAKE_D3DHRESULT(2156)
#define D3DERR_NOTAVAILABLE MAKE_D3DHRESULT(2154)
#define D3DERR_OUTOFVIDEOMEMORY MAKE_D3DHRESULT(380)
#define D3DERR_WRONGTEXTUREFORMAT MAKE_D3DHRESULT(2072)
#define D3DERR_DRIVERINTERNALERROR MAKE_D3DHRESULT(2087)
#define D3DERR_DEVICEHUNG MAKE_D3DHRESULT(2164)

typedef enum _D3DFORMAT {
    D3DFMT_UNKNOWN = 0, D3DFMT_R8G8B8 = 20, D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23,
    D3DFMT_X1R5G5B5 = 24, D3DFMT_A1R5G5B5 = 25, D3DFMT_A4R4G4B4 = 26, D3DFMT_R3G3B2 = 27, D3DFMT_A8 = 28,
    D3DFMT_A8R3G3B2 = 29, D3DFMT_X4R4G4B4 = 30, D3DFMT_A2B10G10R10 = 31, D3DFMT_A8B8G8R8 = 32,
    D3DFMT_X8B8G8R8 = 33, D3DFMT_G16R16 = 34, D3DFMT_A2R10G10B10 = 35, D3DFMT_A16B16G16R16 = 36,
    D3DFMT_A8P8 = 40, D3DFMT_P8 = 41, D3DFMT_L8 = 50, D3DFMT_A8L8 = 51, D3DFMT_A4L4 = 52,
    D3DFMT_V8U8 = 60, D3DFMT_L6V5U5 = 61, D3DFMT_X8L8V8U8 = 62, D3DFMT_Q8W8V8U8 = 63, D3DFMT_V16U16 = 64,
    D3DFMT_D16_LOCKABLE = 70, D3DFMT_D32 = 71, D3DFMT_D15S1 = 73, D3DFMT_D24S8 = 75, D3DFMT_D24X8 = 77,
    D3DFMT_D24X4S4 = 79, D3DFMT_D16 = 80, D3DFMT_L16 = 81, D3DFMT_D32F_LOCKABLE = 82, D3DFMT_D24FS8 = 83,
    D3DFMT_VERTEXDATA = 100, D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102,
    D3DFMT_R16F = 111, D3DFMT_G16R16F = 112, D3DFMT_A16B16G16R16F = 113, D3DFMT_R32F = 114,
    D3DFMT_G32R32F = 115, D3DFMT_A32B32G32R32F = 116,
    D3DFMT_UYVY = MAKEFOURCC('U', 'Y', 'V', 'Y'), D3DFMT_YUY2 = MAKEFOURCC('Y', 'U', 'Y', '2'),
    D3DFMT_DXT1 = MAKEFOURCC('D', 'X', 'T', '1'), D3DFMT_DXT2 = MAKEFOURCC('D', 'X', 'T', '2'),
    D3DFMT_DXT3 = MAKEFOURCC('D', 'X', 'T', '3'), D3DFMT_DXT4 = MAKEFOURCC('D', 'X', 'T', '4'),
    D3DFMT_DXT5 = MAKEFOURCC('D', 'X', 'T', '5'),
    D3DFMT_FORCE_DWORD = 0x7fffffff
} D3DFORMAT;

typedef enum _D3DDEVTYPE { D3DDEVTYPE_HAL = 1, D3DDEVTYPE_REF = 2, D3DDEVTYPE_SW = 3, D3DDEVTYPE_NULLREF = 4 } D3DDEVTYPE;
typedef enum _D3DPOOL { D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2, D3DPOOL_SCRATCH = 3 } D3DPOOL;
typedef enum _D3DRESOURCETYPE {
    D3DRTYPE_SURFACE = 1, D3DRTYPE_VOLUME = 2, D3DRTYPE_TEXTURE = 3, D3DRTYPE_VOLUMETEXTURE = 4,
    D3DRTYPE_CUBETEXTURE = 5, D3DRTYPE_VERTEXBUFFER = 6, D3DRTYPE_INDEXBUFFER = 7
} D3DRESOURCETYPE;
typedef enum _D3DSWAPEFFECT { D3DSWAPEFFECT_DISCARD = 1, D3DSWAPEFFECT_FLIP = 2, D3DSWAPEFFECT_COPY = 3 } D3DSWAPEFFECT;
typedef enum _D3DBACKBUFFER_TYPE { D3DBACKBUFFER_TYPE_MONO = 0, D3DBACKBUFFER_TYPE_LEFT = 1, D3DBACKBUFFER_TYPE_RIGHT = 2 } D3DBACKBUFFER_TYPE;
typedef enum _D3DMULTISAMPLE_TYPE {
    D3DMULTISAMPLE_NONE = 0, D3DMULTISAMPLE_NONMASKABLE = 1, D3DMULTISAMPLE_2_SAMPLES = 2, D3DMULTISAMPLE_3_SAMPLES = 3,
    D3DMULTISAMPLE_4_SAMPLES = 4, D3DMULTISAMPLE_5_SAMPLES = 5, D3DMULTISAMPLE_6_SAMPLES = 6, D3DMULTISAMPLE_8_SAMPLES = 8
} D3DMULTISAMPLE_TYPE;
typedef enum _D3DPRIMITIVETYPE {
    D3DPT_POINTLIST = 1, D3DPT_LINELIST = 2, D3DPT_LINESTRIP = 3, D3DPT_TRIANGLELIST = 4, D3DPT_TRIANGLESTRIP = 5, D3DPT_TRIANGLEFAN = 6
} D3DPRIMITIVETYPE;
typedef enum _D3DCUBEMAP_FACES {
    D3DCUBEMAP_FACE_POSITIVE_X = 0, D3DCUBEMAP_FACE_NEGATIVE_X = 1, D3DCUBEMAP_FACE_POSITIVE_Y = 2,
    D3DCUBEMAP_FACE_NEGATIVE_Y = 3, D3DCUBEMAP_FACE_POSITIVE_Z = 4, D3DCUBEMAP_FACE_NEGATIVE_Z = 5
} D3DCUBEMAP_FACES;
typedef enum _D3DQUERYTYPE { D3DQUERYTYPE_EVENT = 8, D3DQUERYTYPE_OCCLUSION = 9, D3DQUERYTYPE_TIMESTAMP = 10 } D3DQUERYTYPE;
#define D3DISSUE_END (1 << 0)
#define D3DISSUE_BEGIN (1 << 1)
#define D3DGETDATA_FLUSH (1 << 0)

typedef enum _D3DRENDERSTATETYPE {
    D3DRS_ZENABLE = 7, D3DRS_FILLMODE = 8, D3DRS_SHADEMODE = 9, D3DRS_ZWRITEENABLE = 14, D3DRS_ALPHATESTENABLE = 15,
    D3DRS_LASTPIXEL = 16, D3DRS_SRCBLEND = 19, D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23,
    D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25, D3DRS_DITHERENABLE = 26, D3DRS_ALPHABLENDENABLE = 27,
    D3DRS_FOGENABLE = 28, D3DRS_SPECULARENABLE = 29, D3DRS_FOGCOLOR = 34, D3DRS_FOGTABLEMODE = 35,
    D3DRS_FOGSTART = 36, D3DRS_FOGEND = 37, D3DRS_FOGDENSITY = 38, D3DRS_RANGEFOGENABLE = 48,
    D3DRS_STENCILENABLE = 52, D3DRS_STENCILFAIL = 53, D3DRS_STENCILZFAIL = 54, D3DRS_STENCILPASS = 55,
    D3DRS_STENCILFUNC = 56, D3DRS_STENCILREF = 57, D3DRS_STENCILMASK = 58, D3DRS_STENCILWRITEMASK = 59,
    D3DRS_TEXTUREFACTOR = 60, D3DRS_WRAP0 = 128, D3DRS_CLIPPING = 136, D3DRS_LIGHTING = 137, D3DRS_AMBIENT = 139,
    D3DRS_FOGVERTEXMODE = 140, D3DRS_COLORVERTEX = 141, D3DRS_LOCALVIEWER = 142, D3DRS_NORMALIZENORMALS = 143,
    D3DRS_VERTEXBLEND = 151, D3DRS_CLIPPLANEENABLE = 152, D3DRS_POINTSIZE = 154, D3DRS_POINTSIZE_MIN = 155,
    D3DRS_POINTSPRITEENABLE = 156, D3DRS_POINTSCALEENABLE = 157, D3DRS_MULTISAMPLEANTIALIAS = 161,
    D3DRS_MULTISAMPLEMASK = 162, D3DRS_POINTSIZE_MAX = 166, D3DRS_COLORWRITEENABLE = 168, D3DRS_BLENDOP = 171,
    D3DRS_SCISSORTESTENABLE = 174, D3DRS_SLOPESCALEDEPTHBIAS = 175, D3DRS_ANTIALIASEDLINEENABLE = 176,
    D3DRS_MINTESSELLATIONLEVEL = 178, D3DRS_MAXTESSELLATIONLEVEL = 179, D3DRS_ADAPTIVETESS_X = 180,
    D3DRS_ADAPTIVETESS_Y = 181, D3DRS_ADAPTIVETESS_Z = 182, D3DRS_ADAPTIVETESS_W = 183,
    D3DRS_ENABLEADAPTIVETESSELLATION = 184, D3DRS_TWOSIDEDSTENCILMODE = 185, D3DRS_CCW_STENCILFAIL = 186,
    D3DRS_CCW_STENCILZFAIL = 187, D3DRS_CCW_STENCILPASS = 188, D3DRS_CCW_STENCILFUNC = 189,
    D3DRS_COLORWRITEENABLE1 = 190, D3DRS_COLORWRITEENABLE2 = 191, D3DRS_COLORWRITEENABLE3 = 192,
    D3DRS_BLENDFACTOR = 193, D3DRS_SRGBWRITEENABLE = 194, D3DRS_DEPTHBIAS = 195,
    D3DRS_SEPARATEALPHABLENDENABLE = 206, D3DRS_SRCBLENDALPHA = 207, D3DRS_DESTBLENDALPHA = 208, D3DRS_BLENDOPALPHA = 209,
    D3DRS_FORCE_DWORD = 0x7fffffff
} D3DRENDERSTATETYPE;

typedef enum _D3DSAMPLERSTATETYPE {
    D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV = 2, D3DSAMP_ADDRESSW = 3, D3DSAMP_BORDERCOLOR = 4, D3DSAMP_MAGFILTER = 5,
    D3DSAMP_MINFILTER = 6, D3DSAMP_MIPFILTER = 7, D3DSAMP_MIPMAPLODBIAS = 8, D3DSAMP_MAXMIPLEVEL = 9,
    D3DSAMP_MAXANISOTROPY = 10, D3DSAMP_SRGBTEXTURE = 11, D3DSAMP_ELEMENTINDEX = 12, D3DSAMP_DMAPOFFSET = 13,
    D3DSAMP_FORCE_DWORD = 0x7fffffff
} D3DSAMPLERSTATETYPE;

typedef enum _D3DTEXTURESTAGESTATETYPE {
    D3DTSS_COLOROP = 1, D3DTSS_COLORARG1 = 2, D3DTSS_COLORARG2 = 3, D3DTSS_ALPHAOP = 4, D3DTSS_ALPHAARG1 = 5,
    D3DTSS_ALPHAARG2 = 6, D3DTSS_FORCE_DWORD = 0x7fffffff
} D3DTEXTURESTAGESTATETYPE;
typedef enum _D3DTEXTUREOP { D3DTOP_DISABLE = 1, D3DTOP_SELECTARG1 = 2, D3DTOP_SELECTARG2 = 3, D3DTOP_MODULATE = 4 } D3DTEXTUREOP;
#define D3DTA_DIFFUSE 0
#define D3DTA_CURRENT 1
#define D3DTA_TEXTURE 2
#define D3DTA_TFACTOR 3
#define D3DTA_SPECULAR 4

typedef enum _D3DTEXTUREFILTERTYPE {
    D3DTEXF_NONE = 0, D3DTEXF_POINT = 1, D3DTEXF_LINEAR = 2, D3DTEXF_ANISOTROPIC = 3
} D3DTEXTUREFILTERTYPE;
typedef enum _D3DTEXTUREADDRESS {
    D3DTADDRESS_WRAP = 1, D3DTADDRESS_MIRROR = 2, D3DTADDRESS_CLAMP = 3, D3DTADDRESS_BORDER = 4, D3DTADDRESS_MIRRORONCE = 5
} D3DTEXTUREADDRESS;
typedef enum _D3DBLEND {
    D3DBLEND_ZERO = 1, D3DBLEND_ONE = 2, D3DBLEND_SRCCOLOR = 3, D3DBLEND_INVSRCCOLOR = 4, D3DBLEND_SRCALPHA = 5,
    D3DBLEND_INVSRCALPHA = 6, D3DBLEND_DESTALPHA = 7, D3DBLEND_INVDESTALPHA = 8, D3DBLEND_DESTCOLOR = 9,
    D3DBLEND_INVDESTCOLOR = 10, D3DBLEND_SRCALPHASAT = 11
} D3DBLEND;
typedef enum _D3DBLENDOP { D3DBLENDOP_ADD = 1, D3DBLENDOP_SUBTRACT = 2, D3DBLENDOP_REVSUBTRACT = 3, D3DBLENDOP_MIN = 4, D3DBLENDOP_MAX = 5 } D3DBLENDOP;
typedef enum _D3DCMPFUNC {
    D3DCMP_NEVER = 1, D3DCMP_LESS = 2, D3DCMP_EQUAL = 3, D3DCMP_LESSEQUAL = 4, D3DCMP_GREATER = 5,
    D3DCMP_NOTEQUAL = 6, D3DCMP_GREATEREQUAL = 7, D3DCMP_ALWAYS = 8
} D3DCMPFUNC;
typedef enum _D3DCULL { D3DCULL_NONE = 1, D3DCULL_CW = 2, D3DCULL_CCW = 3 } D3DCULL;
typedef enum _D3DFILLMODE { D3DFILL_POINT = 1, D3DFILL_WIREFRAME = 2, D3DFILL_SOLID = 3 } D3DFILLMODE;
typedef enum _D3DSTENCILOP {
    D3DSTENCILOP_KEEP = 1, D3DSTENCILOP_ZERO = 2, D3DSTENCILOP_REPLACE = 3, D3DSTENCILOP_INCRSAT = 4,
    D3DSTENCILOP_DECRSAT = 5, D3DSTENCILOP_INVERT = 6, D3DSTENCILOP_INCR = 7, D3DSTENCILOP_DECR = 8
} D3DSTENCILOP;
typedef enum _D3DZBUFFERTYPE { D3DZB_FALSE = 0, D3DZB_TRUE = 1, D3DZB_USEW = 2 } D3DZBUFFERTYPE;

#define D3DUSAGE_RENDERTARGET 0x00000001L
#define D3DUSAGE_DEPTHSTENCIL 0x00000002L
#define D3DUSAGE_WRITEONLY 0x00000008L
#define D3DUSAGE_SOFTWAREPROCESSING 0x00000010L
#define D3DUSAGE_DYNAMIC 0x00000200L
#define D3DUSAGE_AUTOGENMIPMAP 0x00000400L
#define D3DLOCK_READONLY 0x00000010L
#define D3DLOCK_NOSYSLOCK 0x00000800L
#define D3DLOCK_NOOVERWRITE 0x00001000L
#define D3DLOCK_DISCARD 0x00002000L
#define D3DLOCK_DONOTWAIT 0x00004000L
#define D3DCLEAR_TARGET 0x00000001L
#define D3DCLEAR_ZBUFFER 0x00000002L
#define D3DCLEAR_STENCIL 0x00000004L
#define D3DCREATE_FPU_PRESERVE 0x00000002L
#define D3DCREATE_MULTITHREADED 0x00000004L
#define D3DCREATE_PUREDEVICE 0x00000010L
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x00000020L
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x00000040L
#define D3DCREATE_MIXED_VERTEXPROCESSING 0x00000080L
#define D3DPTEXTURECAPS_NONPOW2CONDITIONAL 0x00000100L
#define D3DPRESENT_INTERVAL_DEFAULT 0
#define D3DPRESENT_INTERVAL_ONE 1
#define D3DFVF_XYZ 0x002
#define D3DFVF_XYZRHW 0x004
#define D3DFVF_NORMAL 0x010
#define D3DFVF_DIFFUSE 0x040
#define D3DFVF_SPECULAR 0x080
#define D3DFVF_TEX1 0x100
#define D3DFVF_TLVERTEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1)

typedef DWORD D3DCOLOR;
#define D3DCOLOR_ARGB(a, r, g, b) ((D3DCOLOR)((((a) & 0xff) << 24) | (((r) & 0xff) << 16) | (((g) & 0xff) << 8) | ((b) & 0xff)))
#define D3DCOLOR_XRGB(r, g, b) D3DCOLOR_ARGB(0xff, r, g, b)
#define D3DCOLOR_RGBA(r, g, b, a) D3DCOLOR_ARGB(a, r, g, b)

// D3D shader-bytecode opcodes referenced by the shader loader
#define D3DSIO_DEFB 47
#define D3DSIO_DEFI 48
#define D3DSIO_DEF 81

typedef enum _D3DDECLTYPE {
    D3DDECLTYPE_FLOAT1 = 0, D3DDECLTYPE_FLOAT2 = 1, D3DDECLTYPE_FLOAT3 = 2, D3DDECLTYPE_FLOAT4 = 3,
    D3DDECLTYPE_D3DCOLOR = 4, D3DDECLTYPE_UBYTE4 = 5, D3DDECLTYPE_SHORT2 = 6, D3DDECLTYPE_SHORT4 = 7,
    D3DDECLTYPE_UBYTE4N = 8, D3DDECLTYPE_SHORT2N = 9, D3DDECLTYPE_SHORT4N = 10, D3DDECLTYPE_USHORT2N = 11,
    D3DDECLTYPE_USHORT4N = 12, D3DDECLTYPE_UDEC3 = 13, D3DDECLTYPE_DEC3N = 14, D3DDECLTYPE_FLOAT16_2 = 15,
    D3DDECLTYPE_FLOAT16_4 = 16, D3DDECLTYPE_UNUSED = 17
} D3DDECLTYPE;
typedef enum _D3DDECLUSAGE {
    D3DDECLUSAGE_POSITION = 0, D3DDECLUSAGE_BLENDWEIGHT = 1, D3DDECLUSAGE_BLENDINDICES = 2, D3DDECLUSAGE_NORMAL = 3,
    D3DDECLUSAGE_PSIZE = 4, D3DDECLUSAGE_TEXCOORD = 5, D3DDECLUSAGE_TANGENT = 6, D3DDECLUSAGE_BINORMAL = 7,
    D3DDECLUSAGE_TESSFACTOR = 8, D3DDECLUSAGE_POSITIONT = 9, D3DDECLUSAGE_COLOR = 10, D3DDECLUSAGE_FOG = 11,
    D3DDECLUSAGE_DEPTH = 12, D3DDECLUSAGE_SAMPLE = 13
} D3DDECLUSAGE;
typedef enum _D3DDECLMETHOD { D3DDECLMETHOD_DEFAULT = 0 } D3DDECLMETHOD;

typedef struct _D3DVERTEXELEMENT9 {
    WORD Stream; WORD Offset; BYTE Type; BYTE Method; BYTE Usage; BYTE UsageIndex;
} D3DVERTEXELEMENT9;
#define D3DDECL_END() { 0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0 }

// ---------------------------------------------------------------- structs
typedef struct _D3DVIEWPORT9 { DWORD X, Y, Width, Height; float MinZ, MaxZ; } D3DVIEWPORT9;
typedef struct _D3DLOCKED_RECT { INT Pitch; void *pBits; } D3DLOCKED_RECT;
typedef struct _D3DBOX { UINT Left, Top, Right, Bottom, Front, Back; } D3DBOX;
typedef struct _D3DLOCKED_BOX { INT RowPitch; INT SlicePitch; void *pBits; } D3DLOCKED_BOX;
typedef struct _D3DSURFACE_DESC {
    D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool;
    D3DMULTISAMPLE_TYPE MultiSampleType; DWORD MultiSampleQuality; UINT Width; UINT Height;
} D3DSURFACE_DESC;
typedef struct _D3DVOLUME_DESC {
    D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Width, Height, Depth;
} D3DVOLUME_DESC;
typedef struct _D3DVERTEXBUFFER_DESC { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; DWORD FVF; } D3DVERTEXBUFFER_DESC;
typedef struct _D3DINDEXBUFFER_DESC { D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool; UINT Size; } D3DINDEXBUFFER_DESC;
typedef struct _D3DDISPLAYMODE { UINT Width, Height, RefreshRate; D3DFORMAT Format; } D3DDISPLAYMODE;
typedef struct _D3DPRESENT_PARAMETERS_ {
    UINT BackBufferWidth, BackBufferHeight; D3DFORMAT BackBufferFormat; UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType; DWORD MultiSampleQuality; D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow; BOOL Windowed; BOOL EnableAutoDepthStencil; D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags; UINT FullScreen_RefreshRateInHz; UINT PresentationInterval;
} D3DPRESENT_PARAMETERS;
typedef struct _D3DVSHADERCAPS2_0 { DWORD Caps; INT DynamicFlowControlDepth, NumTemps, StaticFlowControlDepth; } D3DVSHADERCAPS2_0;
typedef struct _D3DPSHADERCAPS2_0 { DWORD Caps; INT DynamicFlowControlDepth, NumTemps, StaticFlowControlDepth, NumInstructionSlots; } D3DPSHADERCAPS2_0;
typedef struct _D3DCAPS9 {
    D3DDEVTYPE DeviceType; UINT AdapterOrdinal; DWORD Caps, Caps2, Caps3, PresentationIntervals, CursorCaps, DevCaps,
        PrimitiveMiscCaps, RasterCaps, ZCmpCaps, SrcBlendCaps, DestBlendCaps, AlphaCmpCaps, ShadeCaps, TextureCaps,
        TextureFilterCaps, CubeTextureFilterCaps, VolumeTextureFilterCaps, TextureAddressCaps, VolumeTextureAddressCaps,
        LineCaps, MaxTextureWidth, MaxTextureHeight, MaxVolumeExtent, MaxTextureRepeat, MaxTextureAspectRatio,
        MaxAnisotropy;
    float MaxVertexW, GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom, ExtentsAdjust;
    DWORD StencilCaps, FVFCaps, TextureOpCaps, MaxTextureBlendStages, MaxSimultaneousTextures, VertexProcessingCaps,
        MaxActiveLights, MaxUserClipPlanes, MaxVertexBlendMatrices, MaxVertexBlendMatrixIndex;
    float MaxPointSize;
    DWORD MaxPrimitiveCount, MaxVertexIndex, MaxStreams, MaxStreamStride, VertexShaderVersion, MaxVertexShaderConst,
        PixelShaderVersion;
    float PixelShader1xMaxValue;
    DWORD DevCaps2;
    float MaxNpatchTessellationLevel;
    DWORD Reserved5;
    UINT MasterAdapterOrdinal, AdapterOrdinalInGroup, NumberOfAdaptersInGroup;
    DWORD DeclTypes, NumSimultaneousRTs, StretchRectFilterCaps;
    D3DVSHADERCAPS2_0 VS20Caps; D3DPSHADERCAPS2_0 PS20Caps;
    DWORD VertexTextureFilterCaps, MaxVShaderInstructionsExecuted, MaxPShaderInstructionsExecuted,
        MaxVertexShader30InstructionSlots, MaxPixelShader30InstructionSlots;
} D3DCAPS9;
typedef struct _D3DADAPTER_IDENTIFIER9 {
    char Driver[512]; char Description[512]; char DeviceName[32];
    LARGE_INTEGER DriverVersion; DWORD VendorId, DeviceId, SubSysId, Revision; DWORD WHQLLevel;
} D3DADAPTER_IDENTIFIER9;
typedef struct _D3DGAMMARAMP { WORD red[256], green[256], blue[256]; } D3DGAMMARAMP;
typedef struct _D3DRASTER_STATUS { BOOL InVBlank; UINT ScanLine; } D3DRASTER_STATUS;

// ---------------------------------------------------------------- interfaces
struct IDirect3DDevice9;
struct IDirect3DSurface9;

struct IDirect3DResource9 : IUnknown {
    virtual HRESULT __stdcall GetDevice(IDirect3DDevice9 **dev) = 0;
    virtual DWORD __stdcall SetPriority(DWORD p) = 0;
    virtual void __stdcall PreLoad() = 0;
    virtual D3DRESOURCETYPE __stdcall GetType() = 0;
};
struct IDirect3DSurface9 : IDirect3DResource9 {
    virtual HRESULT __stdcall GetDesc(D3DSURFACE_DESC *desc) = 0;
    virtual HRESULT __stdcall LockRect(D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) = 0;
    virtual HRESULT __stdcall UnlockRect() = 0;
};
struct IDirect3DBaseTexture9 : IDirect3DResource9 {
    virtual DWORD __stdcall GetLevelCount() = 0;
    virtual DWORD __stdcall SetLOD(DWORD lod) = 0;
    virtual DWORD __stdcall GetLOD() = 0;
    virtual void __stdcall GenerateMipSubLevels() = 0;
};
struct IDirect3DTexture9 : IDirect3DBaseTexture9 {
    virtual HRESULT __stdcall GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) = 0;
    virtual HRESULT __stdcall GetSurfaceLevel(UINT level, IDirect3DSurface9 **surf) = 0;
    virtual HRESULT __stdcall LockRect(UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) = 0;
    virtual HRESULT __stdcall UnlockRect(UINT level) = 0;
    virtual HRESULT __stdcall AddDirtyRect(const RECT *rect) = 0;
};
struct IDirect3DVolumeTexture9 : IDirect3DBaseTexture9 {
    virtual HRESULT __stdcall GetLevelDesc(UINT level, D3DVOLUME_DESC *desc) = 0;
    virtual HRESULT __stdcall LockBox(UINT level, D3DLOCKED_BOX *lb, const D3DBOX *box, DWORD flags) = 0;
    virtual HRESULT __stdcall UnlockBox(UINT level) = 0;
    virtual HRESULT __stdcall AddDirtyBox(const D3DBOX *box) = 0;
};
struct IDirect3DCubeTexture9 : IDirect3DBaseTexture9 {
    virtual HRESULT __stdcall GetLevelDesc(UINT level, D3DSURFACE_DESC *desc) = 0;
    virtual HRESULT __stdcall GetCubeMapSurface(D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **surf) = 0;
    virtual HRESULT __stdcall LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) = 0;
    virtual HRESULT __stdcall UnlockRect(D3DCUBEMAP_FACES face, UINT level) = 0;
};
struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;
// Web port extension: the engine locks a whole dynamic buffer (D3DLOCK_DISCARD, offset 0, size 0/total) but only fills a prefix of it. Telling the
// D3D -> WebGL layer how many bytes were actually written before Unlock() lets it upload just that prefix instead of the whole buffer (the skinned
// cache alone is 4.5 MB, the pre-tess index buffer 2 MB, the code mesh 0.5 MB: ~7 MB per frame through the browser's GPU process).
void kisak_buffer_written(IDirect3DVertexBuffer9 *vb, unsigned bytes);
void kisak_buffer_written(IDirect3DIndexBuffer9 *ib, unsigned bytes);

struct IDirect3DVertexBuffer9 : IDirect3DResource9 {
    virtual HRESULT __stdcall Lock(UINT offset, UINT size, void **data, DWORD flags) = 0;
    virtual HRESULT __stdcall Unlock() = 0;
    virtual HRESULT __stdcall GetDesc(D3DVERTEXBUFFER_DESC *desc) = 0;
};
struct IDirect3DIndexBuffer9 : IDirect3DResource9 {
    virtual HRESULT __stdcall Lock(UINT offset, UINT size, void **data, DWORD flags) = 0;
    virtual HRESULT __stdcall Unlock() = 0;
    virtual HRESULT __stdcall GetDesc(D3DINDEXBUFFER_DESC *desc) = 0;
};
struct IDirect3DVertexDeclaration9 : IUnknown {
    virtual HRESULT __stdcall GetDeclaration(D3DVERTEXELEMENT9 *elems, UINT *count) = 0;
};
struct IDirect3DVertexShader9 : IUnknown {
    virtual HRESULT __stdcall GetFunction(void *data, UINT *size) = 0;
};
struct IDirect3DPixelShader9 : IUnknown {
    virtual HRESULT __stdcall GetFunction(void *data, UINT *size) = 0;
};
struct IDirect3DQuery9 : IUnknown {
    virtual D3DQUERYTYPE __stdcall GetType() = 0;
    virtual DWORD __stdcall GetDataSize() = 0;
    virtual HRESULT __stdcall Issue(DWORD flags) = 0;
    virtual HRESULT __stdcall GetData(void *data, DWORD size, DWORD flags) = 0;
};
struct IDirect3DSwapChain9 : IUnknown {
    virtual HRESULT __stdcall Present(const RECT *src, const RECT *dst, HWND wnd, const void *dirty, DWORD flags) = 0;
    virtual HRESULT __stdcall GetBackBuffer(UINT idx, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surf) = 0;
    virtual HRESULT __stdcall GetFrontBufferData(IDirect3DSurface9 *dst) = 0;
};

struct IDirect3DDevice9 : IUnknown {
    virtual HRESULT __stdcall TestCooperativeLevel() = 0;
    virtual UINT __stdcall GetAvailableTextureMem() = 0;
    virtual HRESULT __stdcall CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pp, IDirect3DSwapChain9 **sc) = 0;
    virtual HRESULT __stdcall GetSwapChain(UINT idx, IDirect3DSwapChain9 **sc) = 0;
    virtual HRESULT __stdcall Reset(D3DPRESENT_PARAMETERS *pp) = 0;
    virtual HRESULT __stdcall Present(const RECT *src, const RECT *dst, HWND wnd, const void *dirty) = 0;
    virtual HRESULT __stdcall GetBackBuffer(UINT sc, UINT idx, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **surf) = 0;
    virtual void __stdcall SetGammaRamp(UINT sc, DWORD flags, const D3DGAMMARAMP *ramp) = 0;
    virtual HRESULT __stdcall CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **tex, HANDLE *shared) = 0;
    virtual HRESULT __stdcall CreateVolumeTexture(UINT w, UINT h, UINT d, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9 **tex, HANDLE *shared) = 0;
    virtual HRESULT __stdcall CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9 **tex, HANDLE *shared) = 0;
    virtual HRESULT __stdcall CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **vb, HANDLE *shared) = 0;
    virtual HRESULT __stdcall CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **ib, HANDLE *shared) = 0;
    virtual HRESULT __stdcall CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL lockable, IDirect3DSurface9 **surf, HANDLE *shared) = 0;
    virtual HRESULT __stdcall CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL discard, IDirect3DSurface9 **surf, HANDLE *shared) = 0;
    virtual HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst) = 0;
    virtual HRESULT __stdcall GetRenderTargetData(IDirect3DSurface9 *rt, IDirect3DSurface9 *dst) = 0;
    virtual HRESULT __stdcall StretchRect(IDirect3DSurface9 *src, const RECT *srcRect, IDirect3DSurface9 *dst, const RECT *dstRect, D3DTEXTUREFILTERTYPE filter) = 0;
    virtual HRESULT __stdcall CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool, IDirect3DSurface9 **surf, HANDLE *shared) = 0;
    virtual HRESULT __stdcall SetRenderTarget(DWORD idx, IDirect3DSurface9 *rt) = 0;
    virtual HRESULT __stdcall GetRenderTarget(DWORD idx, IDirect3DSurface9 **rt) = 0;
    virtual HRESULT __stdcall SetDepthStencilSurface(IDirect3DSurface9 *ds) = 0;
    virtual HRESULT __stdcall GetDepthStencilSurface(IDirect3DSurface9 **ds) = 0;
    virtual HRESULT __stdcall BeginScene() = 0;
    virtual HRESULT __stdcall EndScene() = 0;
    virtual HRESULT __stdcall Clear(DWORD count, const void *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) = 0;
    virtual HRESULT __stdcall SetViewport(const D3DVIEWPORT9 *vp) = 0;
    virtual HRESULT __stdcall GetViewport(D3DVIEWPORT9 *vp) = 0;
    virtual HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE state, DWORD value) = 0;
    virtual HRESULT __stdcall GetRenderState(D3DRENDERSTATETYPE state, DWORD *value) = 0;
    virtual HRESULT __stdcall SetTexture(DWORD stage, IDirect3DBaseTexture9 *tex) = 0;
    virtual HRESULT __stdcall SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) = 0;
    virtual HRESULT __stdcall SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) = 0;
    virtual HRESULT __stdcall SetScissorRect(const RECT *rect) = 0;
    virtual HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE type, UINT startVertex, UINT primCount) = 0;
    virtual HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex, UINT numVertices, UINT startIndex, UINT primCount) = 0;
    virtual HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primCount, const void *data, UINT stride) = 0;
    virtual HRESULT __stdcall CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elems, IDirect3DVertexDeclaration9 **decl) = 0;
    virtual HRESULT __stdcall SetVertexDeclaration(IDirect3DVertexDeclaration9 *decl) = 0;
    virtual HRESULT __stdcall SetFVF(DWORD fvf) = 0;
    virtual HRESULT __stdcall CreateVertexShader(const DWORD *fn, IDirect3DVertexShader9 **vs) = 0;
    virtual HRESULT __stdcall SetVertexShader(IDirect3DVertexShader9 *vs) = 0;
    virtual HRESULT __stdcall SetVertexShaderConstantF(UINT reg, const float *data, UINT vec4count) = 0;
    virtual HRESULT __stdcall SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *vb, UINT offset, UINT stride) = 0;
    virtual HRESULT __stdcall SetIndices(IDirect3DIndexBuffer9 *ib) = 0;
    virtual HRESULT __stdcall CreatePixelShader(const DWORD *fn, IDirect3DPixelShader9 **ps) = 0;
    virtual HRESULT __stdcall SetPixelShader(IDirect3DPixelShader9 *ps) = 0;
    virtual HRESULT __stdcall SetPixelShaderConstantF(UINT reg, const float *data, UINT vec4count) = 0;
    virtual HRESULT __stdcall CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **q) = 0;
};

struct IDirect3D9 : IUnknown {
    virtual UINT __stdcall GetAdapterCount() = 0;
    virtual HRESULT __stdcall GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *id) = 0;
    virtual UINT __stdcall GetAdapterModeCount(UINT adapter, D3DFORMAT fmt) = 0;
    virtual HRESULT __stdcall EnumAdapterModes(UINT adapter, D3DFORMAT fmt, UINT mode, D3DDISPLAYMODE *dm) = 0;
    virtual HRESULT __stdcall GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE *dm) = 0;
    virtual HRESULT __stdcall CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT backbuf, BOOL windowed) = 0;
    virtual HRESULT __stdcall CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFmt, DWORD usage, D3DRESOURCETYPE rtype, D3DFORMAT checkFmt) = 0;
    virtual HRESULT __stdcall CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT fmt, BOOL windowed, D3DMULTISAMPLE_TYPE ms, DWORD *quality) = 0;
    virtual HRESULT __stdcall CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT adapterFmt, D3DFORMAT rtFmt, D3DFORMAT dsFmt) = 0;
    virtual HRESULT __stdcall GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps) = 0;
    virtual HMONITOR __stdcall GetAdapterMonitor(UINT adapter) = 0;
    virtual HRESULT __stdcall CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **dev) = 0;
};

typedef IDirect3D9 *LPDIRECT3D9;
typedef IDirect3DDevice9 *LPDIRECT3DDEVICE9;
typedef IDirect3DTexture9 *LPDIRECT3DTEXTURE9;
typedef IDirect3DVertexDeclaration9 *LPDIRECT3DVERTEXDECLARATION9;

extern "C++" IDirect3D9 *Direct3DCreate9(UINT sdkVersion);

#endif // __cplusplus
