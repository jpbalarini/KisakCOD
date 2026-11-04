// shader_xlate.cpp -- D3D9 shader bytecode (SM2.0 / SM3.0 token streams) -> GLSL ES 3.00. See shader_xlate.h.
#include "shader_xlate.h"

#include <algorithm>
#include <array>
#include <map>
#include <math.h>
#include <set>
#include <stdio.h>
#include <string.h>

namespace d3dxlate {

namespace {

enum RegType {
    R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_ADDR = 3, R_RASTOUT = 4, R_ATTROUT = 5, R_OUTPUT = 6, R_CONSTINT = 7,
    R_COLOROUT = 8, R_DEPTHOUT = 9, R_SAMPLER = 10, R_CONST2 = 11, R_CONST3 = 12, R_CONST4 = 13, R_CONSTBOOL = 14,
    R_LOOP = 15, R_TEMPFLOAT16 = 16, R_MISC = 17, R_LABEL = 18, R_PREDICATE = 19
};

enum Op {
    OP_NOP = 0, OP_MOV = 1, OP_ADD = 2, OP_SUB = 3, OP_MAD = 4, OP_MUL = 5, OP_RCP = 6, OP_RSQ = 7, OP_DP3 = 8, OP_DP4 = 9,
    OP_MIN = 10, OP_MAX = 11, OP_SLT = 12, OP_SGE = 13, OP_EXP = 14, OP_LOG = 15, OP_LIT = 16, OP_DST = 17, OP_LRP = 18,
    OP_FRC = 19, OP_M4X4 = 20, OP_M4X3 = 21, OP_M3X4 = 22, OP_M3X3 = 23, OP_M3X2 = 24, OP_CALL = 25, OP_CALLNZ = 26,
    OP_LOOP = 27, OP_RET = 28, OP_ENDLOOP = 29, OP_LABEL = 30, OP_DCL = 31, OP_POW = 32, OP_CRS = 33, OP_SGN = 34,
    OP_ABS = 35, OP_NRM = 36, OP_SINCOS = 37, OP_REP = 38, OP_ENDREP = 39, OP_IF = 40, OP_IFC = 41, OP_ELSE = 42,
    OP_ENDIF = 43, OP_BREAK = 44, OP_BREAKC = 45, OP_MOVA = 46, OP_DEFB = 47, OP_DEFI = 48, OP_TEXKILL = 65, OP_TEXLD = 66,
    OP_EXPP = 78, OP_LOGP = 79, OP_CND = 80, OP_DEF = 81, OP_CMP = 88, OP_DP2ADD = 90, OP_DSX = 91, OP_DSY = 92,
    OP_TEXLDD = 93, OP_SETP = 94, OP_TEXLDL = 95, OP_BREAKP = 96, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF
};

struct Instr {
    uint32_t op, ctl, len;
    const uint32_t *t; // operand tokens
};

static std::string fnum(float f)
{
    char b[64];
    if (!std::isfinite(f)) {
        uint32_t u;
        memcpy(&u, &f, 4);
        snprintf(b, sizeof b, "uintBitsToFloat(%uu)", u);
        return b;
    }
    snprintf(b, sizeof b, "%.9g", f);
    if (!strpbrk(b, ".eE")) strcat(b, ".0");
    return b;
}

static std::string maskStr(int mask)
{
    std::string s;
    for (int i = 0; i < 4; ++i) if (mask & (1 << i)) s += "xyzw"[i];
    return s;
}

struct Xlate {
    const uint32_t *w;
    size_t n;
    Options opt;
    Info &info;
    std::string &err;
    bool ps = false;
    std::vector<Instr> ins;
    std::map<int, Semantic> inDecl;    // register number -> semantic (v# for VS inputs, v#/t# for PS)
    std::map<int, Semantic> outDecl;   // VS output register -> semantic
    std::map<int, std::array<float, 4>> defs;
    std::map<int, std::array<int, 4>> defis;
    std::set<int> defbs;
    std::map<int, int> psTexRegs;      // PS ps_2_0 texture regs t# -> unused marker
    int maxTemp = -1;
    std::set<int> colorOuts;
    bool depthOut = false, vposUsed = false, vfaceUsed = false;
    std::map<int, int> inputSlot;      // input reg -> slot (declaration order) for the BGRA fix-up
    std::string body;
    int depth = 0;

    Xlate(const uint32_t *w_, size_t n_, const Options &o, Info &i, std::string &e) : w(w_), n(n_), opt(o), info(i), err(e) {}

    bool fail(const std::string &m) { if (err.empty()) err = m; return false; }

    static int typeOf(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }
    static int numOf(uint32_t t) { return (int)(t & 0x7FF); }

    // ------------------------------------------------------------ pass 1: decode, collect decls/defs
    bool scan()
    {
        if (n < 2) return fail("empty shader");
        uint32_t ver = w[0];
        uint32_t kind = ver >> 16;
        if (kind == 0xFFFE) ps = false; else if (kind == 0xFFFF) ps = true; else return fail("bad version token");
        info.isPixel = ps;
        info.major = (ver >> 8) & 0xFF;
        info.minor = ver & 0xFF;
        if (info.major < 2) return fail("shader model 1.x not supported");
        size_t i = 1;
        while (i < n) {
            uint32_t t = w[i];
            uint32_t op = t & 0xFFFF;
            if (op == OP_COMMENT) { i += 1 + ((t >> 16) & 0x7FFF); continue; }
            if (op == OP_END) break;
            uint32_t len = (t >> 24) & 0xF;
            if (i + 1 + len > n) return fail("truncated instruction");
            Instr in{op, (t >> 16) & 0xFF, len, &w[i + 1]};
            if (t & 0x40000000) return fail("predicated instructions not supported");
            if (op == OP_DCL) {
                if (len != 2) return fail("bad dcl");
                uint32_t u = in.t[0], d = in.t[1];
                int type = typeOf(d), num = numOf(d);
                if (type == R_SAMPLER) {
                    int tt = (u >> 27) & 0xF;
                    if (num > 15) return fail("sampler index out of range");
                    info.samplerKind[num] = tt == 2 ? SAMP_2D : tt == 3 ? SAMP_CUBE : tt == 4 ? SAMP_VOLUME : SAMP_2D;
                    info.samplerMask |= 1u << num;
                } else if (type == R_MISC) {
                    if (num == 0) vposUsed = true; else if (num == 1) vfaceUsed = true;
                } else {
                    Semantic s{(uint8_t)(u & 0x1F), (uint8_t)((u >> 16) & 0xF)};
                    if (type == R_INPUT || (ps && type == R_ADDR)) {
                        int key = type == R_ADDR ? 1000 + num : num;
                        inDecl[key] = s;
                        inputSlot[key] = (int)inputSlot.size();
                        info.inputs.push_back(s);
                    } else if (type == R_OUTPUT && !ps) {
                        outDecl[num] = s;
                    } else if (!ps && (type == R_RASTOUT || type == R_ATTROUT)) {
                        return fail("vs_2_0 style output registers not supported");
                    }
                }
            } else if (op == OP_DEF) {
                if (len != 5) return fail("bad def");
                std::array<float, 4> v;
                memcpy(v.data(), &in.t[1], 16);
                defs[numOf(in.t[0])] = v;
            } else if (op == OP_DEFI) {
                if (len != 5) return fail("bad defi");
                std::array<int, 4> v;
                memcpy(v.data(), &in.t[1], 16);
                defis[numOf(in.t[0])] = v;
            } else if (op == OP_DEFB) {
                if (len != 2) return fail("bad defb");
                if (in.t[1] & 1) defbs.insert(numOf(in.t[0]));
            } else {
                for (uint32_t k = 0; k < len; ++k) {
                    uint32_t p = in.t[k];
                    if ((p >> 31) == 0) continue; // not a parameter token
                    if (p & 0x2000) return fail("relative addressing not supported");
                    if (typeOf(p) == R_TEMP) maxTemp = std::max(maxTemp, numOf(p));
                    if (typeOf(p) == R_COLOROUT && op != OP_TEXLD) {
                        // dest only
                    }
                }
                if (len >= 1 && (in.t[0] >> 31)) {
                    int type = typeOf(in.t[0]);
                    if (type == R_COLOROUT && op != OP_TEXKILL) colorOuts.insert(numOf(in.t[0]));
                    if (type == R_DEPTHOUT) depthOut = true;
                    if (type == R_CONSTINT || type == R_CONSTBOOL || type == R_LOOP || type == R_PREDICATE) { /* checked at emit */ }
                }
                ins.push_back(in);
            }
            i += 1 + len;
        }
        info.writesDepth = depthOut;
        info.colorOutputs = colorOuts.empty() ? 0 : (*colorOuts.rbegin() + 1);
        if (!ps) for (auto &kv : outDecl) if (kv.second.usage != USAGE_POSITION && kv.second.usage != USAGE_PSIZE) info.outputs.push_back(kv.second);
        return true;
    }

    // ------------------------------------------------------------ register naming
    std::string constName(int type, int num)
    {
        int idx = num;
        if (type == R_CONST2) idx += 2048; else if (type == R_CONST3) idx += 4096; else if (type == R_CONST4) idx += 6144;
        auto it = defs.find(idx);
        if (it != defs.end()) return "d" + std::to_string(idx);
        int limit = ps ? 224 : 256;
        if (idx >= limit) { fail("constant register out of range"); return "vec4(0.0)"; }
        return std::string(ps ? "pcb[" : "vcb[") + std::to_string(idx) + "]";
    }

    std::string regRead(uint32_t tok)
    {
        int type = typeOf(tok), num = numOf(tok);
        switch (type) {
        case R_TEMP: return "r" + std::to_string(num);
        case R_INPUT: return "v" + std::to_string(num);
        case R_CONST: case R_CONST2: case R_CONST3: case R_CONST4: return constName(type, num);
        case R_ADDR: return ps ? "t" + std::to_string(num) : "a0";
        case R_OUTPUT: return "o" + std::to_string(num);
        case R_COLOROUT: return "oC" + std::to_string(num);
        case R_DEPTHOUT: return "oDepth";
        case R_MISC: return num == 0 ? "vPos" : "vFace";
        case R_CONSTBOOL: return "b" + std::to_string(num);
        case R_CONSTINT: return "i" + std::to_string(num);
        default: fail("unsupported source register type " + std::to_string(type)); return "vec4(0.0)";
        }
    }

    std::string src(uint32_t tok)
    {
        std::string b = regRead(tok);
        int sw = (tok >> 16) & 0xFF;
        std::string s = b;
        if (sw != 0xE4) {
            char c[5];
            for (int i = 0; i < 4; ++i) c[i] = "xyzw"[(sw >> (2 * i)) & 3];
            c[4] = 0;
            s = b + "." + c;
        }
        switch ((tok >> 24) & 0xF) {
        case 0: return s;
        case 1: return "(-" + s + ")";
        case 2: return "(" + s + " - vec4(0.5))";
        case 3: return "(-(" + s + " - vec4(0.5)))";
        case 4: return "(" + s + " * 2.0 - vec4(1.0))";
        case 5: return "(-(" + s + " * 2.0 - vec4(1.0)))";
        case 6: return "(vec4(1.0) - " + s + ")";
        case 7: return "(" + s + " * 2.0)";
        case 8: return "(-(" + s + " * 2.0))";
        case 9: return "(" + s + " / vec4((" + s + ").z))";
        case 10: return "(" + s + " / vec4((" + s + ").w))";
        case 11: return "abs(" + s + ")";
        case 12: return "(-abs(" + s + "))";
        default: fail("unsupported source modifier"); return s;
        }
    }

    // row k of a matrix whose first row is the constant register in `tok` (matrix ops ignore its swizzle)
    std::string constRow(uint32_t tok, int k)
    {
        int type = typeOf(tok);
        if (type != R_CONST && type != R_CONST2 && type != R_CONST3 && type != R_CONST4) { fail("matrix op on non-constant"); return "vec4(0.0)"; }
        std::string s = constName(type, numOf(tok) + k);
        return s;
    }

    std::string dstName(uint32_t tok)
    {
        int type = typeOf(tok), num = numOf(tok);
        switch (type) {
        case R_TEMP: return "r" + std::to_string(num);
        case R_OUTPUT: return "o" + std::to_string(num);
        case R_COLOROUT: return "oC" + std::to_string(num);
        case R_DEPTHOUT: return "oDepth";
        case R_ADDR: return ps ? "t" + std::to_string(num) : "a0";
        default: fail("unsupported destination register type " + std::to_string(type)); return "r0";
        }
    }

    void line(const std::string &s)
    {
        body += std::string((size_t)std::max(depth, 0) * 2 + 2, ' ');
        body += s;
        body += '\n';
    }

    void assign(uint32_t dtok, const std::string &expr)
    {
        std::string name = dstName(dtok);
        int mask = (dtok >> 16) & 0xF;
        int rm = (dtok >> 20) & 0xF;
        int shift = (dtok >> 24) & 0xF;
        std::string e = expr;
        if (shift) {
            int s = shift >= 8 ? shift - 16 : shift;
            e = "(" + e + " * " + fnum(s >= 0 ? (float)(1 << s) : 1.0f / (float)(1 << -s)) + ")";
        }
        if (rm & 1) e = "clamp(" + e + ", 0.0, 1.0)";
        if (mask == 0xF) line(name + " = " + e + ";");
        else if (mask) { std::string m = maskStr(mask); line(name + "." + m + " = (" + e + ")." + m + ";"); }
    }

    static std::string sc(const std::string &s) { return "(" + s + ").x"; }

    std::string sampleExpr(const Instr &in)
    {
        // texld dst, coord, sampler  (also texldl / texldd handled by caller through ctl)
        uint32_t st = in.t[2];
        if (typeOf(st) != R_SAMPLER) { fail("texld sampler operand is not a sampler"); return "vec4(0.0)"; }
        int sn = numOf(st);
        std::string c = src(in.t[1]);
        std::string s = "s" + std::to_string(sn);
        uint8_t kind = info.samplerKind[sn] ? info.samplerKind[sn] : (uint8_t)SAMP_2D;
        bool shadow = ps && ((opt.shadowMask >> sn) & 1) && kind == SAMP_2D;
        std::string r;
        uint32_t ctl = in.ctl;
        bool lod = in.op == OP_TEXLDL;
        bool proj = !lod && (ctl & 1);
        bool bias = !lod && (ctl & 2);
        if (shadow) {
            if (proj) r = "vec4(textureProj(" + s + ", vec4((" + c + ").xyz, (" + c + ").w)))";
            else r = "vec4(texture(" + s + ", vec3((" + c + ").xyz)))";
        } else if (kind == SAMP_2D) {
            if (lod) r = "textureLod(" + s + ", (" + c + ").xy, (" + c + ").w)";
            else if (proj) r = "textureProj(" + s + ", vec3((" + c + ").xy, (" + c + ").w))";
            else if (bias) r = "texture(" + s + ", (" + c + ").xy, (" + c + ").w)";
            else r = "texture(" + s + ", (" + c + ").xy)";
        } else if (kind == SAMP_CUBE) {
            if (lod) r = "textureLod(" + s + ", (" + c + ").xyz, (" + c + ").w)";
            else if (proj) r = "texture(" + s + ", (" + c + ").xyz / (" + c + ").w)";
            else if (bias) r = "texture(" + s + ", (" + c + ").xyz, (" + c + ").w)";
            else r = "texture(" + s + ", (" + c + ").xyz)";
        } else {
            if (lod) r = "textureLod(" + s + ", (" + c + ").xyz, (" + c + ").w)";
            else if (proj) r = "textureProj(" + s + ", (" + c + "))";
            else if (bias) r = "texture(" + s + ", (" + c + ").xyz, (" + c + ").w)";
            else r = "texture(" + s + ", (" + c + ").xyz)";
        }
        if (shadow) return r;
        return "d3d_sr(" + r + ", " + std::to_string(sn) + ")";
    }

    // ------------------------------------------------------------ pass 2: emit instruction bodies
    bool emit()
    {
        for (const Instr &in : ins) {
            const uint32_t *t = in.t;
            switch (in.op) {
            case OP_NOP: break;
            case OP_MOV: assign(t[0], src(t[1])); break;
            case OP_ADD: assign(t[0], src(t[1]) + " + " + src(t[2])); break;
            case OP_SUB: assign(t[0], src(t[1]) + " - " + src(t[2])); break;
            case OP_MUL: assign(t[0], src(t[1]) + " * " + src(t[2])); break;
            case OP_MAD: assign(t[0], src(t[1]) + " * " + src(t[2]) + " + " + src(t[3])); break;
            case OP_RCP: assign(t[0], "vec4(1.0 / " + sc(src(t[1])) + ")"); break;
            case OP_RSQ: assign(t[0], "vec4(inversesqrt(abs(" + sc(src(t[1])) + ")))"); break;
            case OP_DP3: assign(t[0], "vec4(dot((" + src(t[1]) + ").xyz, (" + src(t[2]) + ").xyz))"); break;
            case OP_DP4: assign(t[0], "vec4(dot(" + src(t[1]) + ", " + src(t[2]) + "))"); break;
            case OP_DP2ADD: assign(t[0], "vec4(dot((" + src(t[1]) + ").xy, (" + src(t[2]) + ").xy) + " + sc(src(t[3])) + ")"); break;
            case OP_MIN: assign(t[0], "min(" + src(t[1]) + ", " + src(t[2]) + ")"); break;
            case OP_MAX: assign(t[0], "max(" + src(t[1]) + ", " + src(t[2]) + ")"); break;
            case OP_SLT: assign(t[0], "vec4(lessThan(" + src(t[1]) + ", " + src(t[2]) + "))"); break;
            case OP_SGE: assign(t[0], "vec4(greaterThanEqual(" + src(t[1]) + ", " + src(t[2]) + "))"); break;
            case OP_EXP: case OP_EXPP: assign(t[0], "vec4(exp2(" + sc(src(t[1])) + "))"); break;
            case OP_LOG: case OP_LOGP: assign(t[0], "vec4(log2(abs(" + sc(src(t[1])) + ")))"); break;
            case OP_LIT: assign(t[0], "d3d_lit(" + src(t[1]) + ")"); break;
            case OP_DST: { std::string a = src(t[1]), b = src(t[2]);
                assign(t[0], "vec4(1.0, (" + a + ").y * (" + b + ").y, (" + a + ").z, (" + b + ").w)"); break; }
            case OP_LRP: assign(t[0], "mix(" + src(t[3]) + ", " + src(t[2]) + ", " + src(t[1]) + ")"); break;
            case OP_FRC: assign(t[0], "fract(" + src(t[1]) + ")"); break;
            case OP_POW: assign(t[0], "vec4(pow(abs(" + sc(src(t[1])) + "), " + sc(src(t[2])) + "))"); break;
            case OP_CRS: assign(t[0], "vec4(cross((" + src(t[1]) + ").xyz, (" + src(t[2]) + ").xyz), 0.0)"); break;
            case OP_SGN: assign(t[0], "sign(" + src(t[1]) + ")"); break;
            case OP_ABS: assign(t[0], "abs(" + src(t[1]) + ")"); break;
            case OP_NRM: assign(t[0], "d3d_nrm(" + src(t[1]) + ")"); break;
            case OP_SINCOS: assign(t[0], "vec4(cos(" + sc(src(t[1])) + "), sin(" + sc(src(t[1])) + "), 0.0, 0.0)"); break;
            case OP_CMP: { std::string a = src(t[1]);
                assign(t[0], "mix(" + src(t[3]) + ", " + src(t[2]) + ", vec4(greaterThanEqual(" + a + ", vec4(0.0))))"); break; }
            case OP_M4X4: case OP_M4X3: case OP_M3X4: case OP_M3X3: case OP_M3X2: {
                bool v3 = in.op == OP_M3X4 || in.op == OP_M3X3 || in.op == OP_M3X2;
                int rows = in.op == OP_M4X4 || in.op == OP_M3X4 ? 4 : in.op == OP_M4X3 || in.op == OP_M3X3 ? 3 : 2;
                std::string a = src(t[1]);
                std::string r = "vec4(";
                for (int k = 0; k < 4; ++k) {
                    if (k) r += ", ";
                    if (k < rows) r += v3 ? "dot((" + a + ").xyz, (" + constRow(t[2], k) + ").xyz)" : "dot(" + a + ", " + constRow(t[2], k) + ")";
                    else r += "0.0";
                }
                assign(t[0], r + ")");
                break; }
            case OP_TEXLD: case OP_TEXLDL: assign(t[0], sampleExpr(in)); break;
            case OP_TEXKILL: info.usesTexkill = true; line("if (any(lessThan((" + regRead(t[0]) + ").xyz, vec3(0.0)))) discard;"); break;
            case OP_DSX: assign(t[0], "dFdx(" + src(t[1]) + ")"); break;
            case OP_DSY: assign(t[0], "dFdy(" + src(t[1]) + ")"); break;
            case OP_IFC: {
                static const char *cmp[8] = {"?", ">", "==", ">=", "<", "!=", "<=", "?"};
                uint32_t c = in.ctl & 7;
                if (c == 0 || c == 7) return fail("bad ifc comparison");
                line("if (" + sc(src(t[0])) + " " + cmp[c] + " " + sc(src(t[1])) + ") {");
                ++depth;
                break; }
            case OP_IF: {
                uint32_t p = t[0];
                if (typeOf(p) != R_CONSTBOOL) return fail("if on non-bool register");
                int num = numOf(p);
                bool v = defbs.count(num) != 0;
                line(std::string("if (") + (v ? "true" : "false") + ") {");
                ++depth;
                break; }
            case OP_ELSE: --depth; line("} else {"); ++depth; break;
            case OP_ENDIF: --depth; line("}"); break;
            case OP_RET: if (depth == 0) {} else line("return;"); break;
            default: return fail("unsupported opcode " + std::to_string(in.op));
            }
            if (!err.empty()) return false;
        }
        return err.empty();
    }

    // ------------------------------------------------------------ assemble the full GLSL source
    std::string assemble()
    {
        std::string o;
        o += "#version 300 es\n";
        o += "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp samplerCube;\n"
             "precision highp sampler3D;\nprecision highp sampler2DShadow;\n";
        if (!ps) {
            o += "layout(std140) uniform VSConsts { vec4 vcb[256]; };\n";
            o += "uniform vec2 u_halfPixel;\nuniform int u_bgra;\n";
            for (auto &kv : inDecl)
                o += "in vec4 a_" + std::to_string(kv.second.usage) + "_" + std::to_string(kv.second.index) + ";\n";
            for (auto &kv : outDecl)
                if (kv.second.usage != USAGE_POSITION && kv.second.usage != USAGE_PSIZE)
                    o += "out vec4 v_" + std::to_string(kv.second.usage) + "_" + std::to_string(kv.second.index) + ";\n";
        } else {
            o += "layout(std140) uniform PSConsts { vec4 pcb[224]; };\n";
            o += "uniform vec2 u_alpha;\nuniform int u_srgbWrite;\nuniform int u_srgbRead;\n";
            for (int s = 0; s < 16; ++s) {
                if (!(info.samplerMask & (1u << s))) continue;
                bool shadow = ((opt.shadowMask >> s) & 1) && info.samplerKind[s] == SAMP_2D;
                const char *ty = shadow ? "sampler2DShadow" : info.samplerKind[s] == SAMP_CUBE ? "samplerCube" : info.samplerKind[s] == SAMP_VOLUME ? "sampler3D" : "sampler2D";
                o += std::string("uniform ") + ty + " s" + std::to_string(s) + ";\n";
            }
            std::set<uint16_t> avail;
            if (opt.vsOutputs) avail.insert(opt.vsOutputs->begin(), opt.vsOutputs->end());
            for (auto &kv : inDecl) {
                if (opt.vsOutputs && !avail.count(kv.second.key())) continue;
                o += "in vec4 v_" + std::to_string(kv.second.usage) + "_" + std::to_string(kv.second.index) + ";\n";
            }
            int nOut = std::max(info.colorOutputs, 1);
            for (int i = 0; i < nOut; ++i) o += "layout(location = " + std::to_string(i) + ") out vec4 fc" + std::to_string(i) + ";\n";
        }
        for (auto &kv : defs) {
            o += "const vec4 d" + std::to_string(kv.first) + " = vec4(" + fnum(kv.second[0]) + ", " + fnum(kv.second[1]) + ", " +
                 fnum(kv.second[2]) + ", " + fnum(kv.second[3]) + ");\n";
        }
        o += "vec4 d3d_lit(vec4 s) { float w = clamp(s.w, -127.9961, 127.9961); return vec4(1.0, max(s.x, 0.0), s.x > 0.0 ? pow(max(s.y, 0.0), w) : 0.0, 1.0); }\n";
        o += "vec4 d3d_nrm(vec4 s) { float l = dot(s.xyz, s.xyz); return l > 0.0 ? s * inversesqrt(l) : vec4(0.0); }\n";
        if (ps) {
            o += "vec3 d3d_s2l(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), vec3(greaterThan(c, vec3(0.04045)))); }\n";
            o += "vec3 d3d_l2s(vec3 c) { c = max(c, vec3(0.0)); return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, vec3(greaterThan(c, vec3(0.0031308)))); }\n";
            o += "vec4 d3d_sr(vec4 c, int i) { if ((u_srgbRead & (1 << i)) != 0) c.rgb = d3d_s2l(c.rgb); return c; }\n";
        }
        o += "void main() {\n";
        if (!ps) {
            for (auto &kv : inDecl) {
                int reg = kv.first;
                o += "  vec4 v" + std::to_string(reg) + " = a_" + std::to_string(kv.second.usage) + "_" + std::to_string(kv.second.index) + ";\n";
                o += "  if ((u_bgra & (1 << " + std::to_string(inputSlot[reg]) + ")) != 0) v" + std::to_string(reg) + " = v" + std::to_string(reg) + ".zyxw;\n";
            }
            for (auto &kv : outDecl) o += "  vec4 o" + std::to_string(kv.first) + " = vec4(0.0);\n";
        } else {
            std::set<uint16_t> avail;
            if (opt.vsOutputs) avail.insert(opt.vsOutputs->begin(), opt.vsOutputs->end());
            for (auto &kv : inDecl) {
                int reg = kv.first;
                std::string nm = reg >= 1000 ? "t" + std::to_string(reg - 1000) : "v" + std::to_string(reg);
                bool present = !opt.vsOutputs || avail.count(kv.second.key());
                o += "  vec4 " + nm + " = " + (present ? "v_" + std::to_string(kv.second.usage) + "_" + std::to_string(kv.second.index) : "vec4(0.0)") + ";\n";
            }
            if (vposUsed) o += "  vec4 vPos = vec4(gl_FragCoord.xy, 0.0, 0.0);\n";
            if (vfaceUsed) o += "  vec4 vFace = vec4(gl_FrontFacing ? 1.0 : -1.0);\n";
            for (int c : colorOuts) o += "  vec4 oC" + std::to_string(c) + " = vec4(0.0);\n";
            if (colorOuts.empty()) o += "  vec4 oC0 = vec4(0.0);\n";
            if (depthOut) o += "  vec4 oDepth = vec4(0.0);\n";
        }
        for (int r = 0; r <= maxTemp; ++r) o += "  vec4 r" + std::to_string(r) + " = vec4(0.0);\n";
        o += body;
        if (!ps) {
            int posReg = -1, psizeReg = -1;
            for (auto &kv : outDecl) {
                if (kv.second.usage == USAGE_POSITION && kv.second.index == 0) posReg = kv.first;
                else if (kv.second.usage == USAGE_PSIZE) psizeReg = kv.first;
                else o += "  v_" + std::to_string(kv.second.usage) + "_" + std::to_string(kv.second.index) + " = o" + std::to_string(kv.first) + ";\n";
            }
            if (posReg < 0) { fail("vertex shader has no position output"); return {}; }
            o += "  vec4 pos = o" + std::to_string(posReg) + ";\n";
            o += "  pos.xy += vec2(1.0, -1.0) * u_halfPixel * pos.w;\n";
            o += "  pos.z = 2.0 * pos.z - pos.w;\n";
            o += "  pos.y = -pos.y;\n";
            o += "  gl_Position = pos;\n";
            if (psizeReg >= 0) o += "  gl_PointSize = o" + std::to_string(psizeReg) + ".x;\n";
        } else {
            if (!colorOuts.empty() || true) {
                o += "  if (u_alpha.x > 0.5) {\n"
                     "    int f = int(u_alpha.x + 0.5); float a = oC0.a; float r = u_alpha.y; bool pass = true;\n"
                     "    if (f == 1) pass = false; else if (f == 2) pass = a < r; else if (f == 3) pass = a == r; else if (f == 4) pass = a <= r;\n"
                     "    else if (f == 5) pass = a > r; else if (f == 6) pass = a != r; else if (f == 7) pass = a >= r;\n"
                     "    if (!pass) discard;\n  }\n";
                o += "  if (u_srgbWrite != 0) oC0.rgb = d3d_l2s(oC0.rgb);\n";
            }
            if (colorOuts.empty()) o += "  fc0 = oC0;\n";
            for (int c : colorOuts) o += "  fc" + std::to_string(c) + " = oC" + std::to_string(c) + ";\n";
            if (depthOut) o += "  gl_FragDepth = oDepth.x;\n";
        }
        o += "}\n";
        return o;
    }
};

} // namespace

size_t TokenCount(const uint32_t *w, size_t maxTokens)
{
    if (maxTokens < 2) return 0;
    size_t i = 1;
    while (i < maxTokens) {
        uint32_t t = w[i];
        uint32_t op = t & 0xFFFF;
        if (op == 0xFFFF) return i + 1;
        if (op == 0xFFFE) i += 1 + ((t >> 16) & 0x7FFF);
        else i += 1 + ((t >> 24) & 0xF);
    }
    return 0;
}

bool Translate(const uint32_t *tokens, size_t maxTokens, const Options &opt, std::string &glsl, Info &info, std::string &err)
{
    info = Info();
    err.clear();
    size_t n = TokenCount(tokens, maxTokens);
    if (!n) { err = "no end token"; return false; }
    Xlate x(tokens, n, opt, info, err);
    if (!x.scan()) return false;
    if (!x.emit()) return false;
    glsl = x.assemble();
    return err.empty();
}

} // namespace d3dxlate
