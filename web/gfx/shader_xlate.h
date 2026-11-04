// shader_xlate.h -- Direct3D 9 shader bytecode (vs/ps 1.x-3.0 subset) -> GLSL ES 3.00 translator.
//
// Engine-independent (pure C++17, no engine or GL headers) so it can be unit-tested natively against shaders
// dumped from the fastfiles (web/tools/xlate_test.cpp).
//
// Register-file model (matches D3D9 exactly so the engine's SetXShaderConstantF calls map 1:1):
//   * float constants live in a std140 uniform block:  vcb[256] (vertex)  /  pcb[224] (pixel)
//   * `def` constants are baked into the shader as GLSL consts
//   * vertex inputs are attributes  a_<usage>_<index>,  vertex->pixel varyings are  v_<usage>_<index>
//     (usage = D3DDECLUSAGE value), so a program links by semantic exactly like D3D9 does
//   * the D3D9 -> GL convention differences are fixed up in the vertex-shader epilogue:
//       - clip-space z [0,w] -> [-w,w]
//       - y flip (render targets are stored with D3D row order)
//       - half-pixel offset (D3D9 pixel centres are at integers)
//   * D3D9 fixed-function alpha test and sRGB write (render states that also apply to shaders) are emulated in
//     the pixel-shader epilogue from uniforms (u_alpha, u_srgbWrite)
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

namespace d3dxlate {

enum Usage { // D3DDECLUSAGE values, as they appear in dcl tokens
    USAGE_POSITION = 0, USAGE_BLENDWEIGHT = 1, USAGE_BLENDINDICES = 2, USAGE_NORMAL = 3, USAGE_PSIZE = 4,
    USAGE_TEXCOORD = 5, USAGE_TANGENT = 6, USAGE_BINORMAL = 7, USAGE_TESSFACTOR = 8, USAGE_POSITIONT = 9,
    USAGE_COLOR = 10, USAGE_FOG = 11, USAGE_DEPTH = 12, USAGE_SAMPLE = 13
};

struct Semantic {
    uint8_t usage, index;
    uint16_t key() const { return (uint16_t)((usage << 4) | (index & 15)); }
};

enum SamplerKind : uint8_t { SAMP_NONE = 0, SAMP_2D = 2, SAMP_CUBE = 3, SAMP_VOLUME = 4 };

struct Info {
    bool isPixel = false;
    int major = 0, minor = 0;
    std::vector<Semantic> inputs;     // VS: attributes read;  PS: varyings read
    std::vector<Semantic> outputs;    // VS: varyings written (POSITION excluded)
    uint8_t samplerKind[16] = {};     // SamplerKind per sampler register (PS samplers; vs samplers unsupported)
    uint32_t samplerMask = 0;
    bool writesDepth = false;
    int colorOutputs = 0;             // PS: number of oC registers written (bitmask-free count of highest + 1)
    bool usesTexkill = false;
};

struct Options {
    // PS only: semantics (Semantic::key()) the paired vertex shader writes. Inputs the VS does not write read as 0.
    // nullptr = assume every declared input is present.
    const std::vector<uint16_t> *vsOutputs = nullptr;
    // PS only: bit n set => sampler n is bound to a depth texture and is sampled with hardware compare
    uint32_t shadowMask = 0;
};

// Translate one shader. Returns false (and sets err) on malformed / unsupported bytecode.
bool Translate(const uint32_t *tokens, size_t maxTokens, const Options &opt, std::string &glsl, Info &info, std::string &err);

// Number of DWORDs in a shader token stream including the end token.
size_t TokenCount(const uint32_t *tokens, size_t maxTokens);

} // namespace d3dxlate
