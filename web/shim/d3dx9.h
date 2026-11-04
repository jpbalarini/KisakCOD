// d3dx9.h -- tiny D3DX shim (shader-compiler entry points only; the web build never compiles HLSL at runtime).
#pragma once
#include <d3d9.h>

#ifdef __cplusplus
struct ID3DXBuffer : IUnknown {
    virtual void *__stdcall GetBufferPointer() = 0;
    virtual DWORD __stdcall GetBufferSize() = 0;
};
typedef ID3DXBuffer *LPD3DXBUFFER;

struct ID3DXConstantTable : IUnknown {
    virtual void *__stdcall GetBufferPointer() = 0;
    virtual DWORD __stdcall GetBufferSize() = 0;
};
typedef ID3DXConstantTable *LPD3DXCONSTANTTABLE;

// CTAB (shader constant table) layout embedded in compiled D3D9 shader bytecode
typedef struct _D3DXSHADER_CONSTANTTABLE { DWORD Size; DWORD Creator; DWORD Version; DWORD Constants; DWORD ConstantInfo; DWORD Flags; DWORD Target; } D3DXSHADER_CONSTANTTABLE, *LPD3DXSHADER_CONSTANTTABLE;
typedef struct _D3DXSHADER_CONSTANTINFO { DWORD Name; WORD RegisterSet; WORD RegisterIndex; WORD RegisterCount; WORD Reserved; DWORD TypeInfo; DWORD DefaultValue; } D3DXSHADER_CONSTANTINFO, *LPD3DXSHADER_CONSTANTINFO;
typedef struct _D3DXMACRO { const char *Name; const char *Definition; } D3DXMACRO, *LPD3DXMACRO;
typedef struct _D3DXSEMANTIC { UINT Usage; UINT UsageIndex; } D3DXSEMANTIC, *LPD3DXSEMANTIC;
struct ID3DXInclude;
typedef ID3DXInclude *LPD3DXINCLUDE;

extern "C++" {
HRESULT D3DXCreateBuffer(DWORD size, LPD3DXBUFFER *buf);
HRESULT D3DXCompileShader(const char *src, UINT srcLen, const D3DXMACRO *defines, LPD3DXINCLUDE include,
                          const char *entry, const char *profile, DWORD flags, LPD3DXBUFFER *code,
                          LPD3DXBUFFER *errors, LPD3DXCONSTANTTABLE *constTable);
HRESULT D3DXGetShaderConstantTable(const DWORD *fn, LPD3DXCONSTANTTABLE *table);
HRESULT D3DXGetShaderInputSemantics(const DWORD *fn, D3DXSEMANTIC *sem, UINT *count);
HRESULT D3DXGetShaderOutputSemantics(const DWORD *fn, D3DXSEMANTIC *sem, UINT *count);
BOOL D3DXInitialize(void);
}
#endif
