// ssao_web.cpp -- dvars for the web build's screen-space ambient occlusion (web/gfx/d3d9_gl_device.cpp, Device::applySSAO).
//
// How it fits: the engine already has a "float-z" pass (an R32F target holding linear view depth, built for soft particles and depth of
// field). While r_webSSAO is on, r_scene.cpp (KISAK_WEB block in R_SetDepthOfField) forces that pass on for the view, and
// rb_draw3d.cpp calls kisak_ssao_apply() right after the lit scene / decals / lights are drawn and before emissive effects, glow and the
// HUD. The GL device then multiplies an occlusion term into the scene colour target. Costs one extra geometry pass (the float-z one).
//
//   r_webSSAO          0/1   master switch
//   r_webSSAORadius    world units the occlusion looks around (CoD units are roughly inches)
//   r_webSSAOStrength  0..3  how dark the occlusion gets
//   r_webSSAOBias      0..1  ignores nearly-flat neighbours (raise it if flat walls look dirty)
//   r_webSSAOBlur      0/1   depth-aware separable blur of the occlusion (default on); debug views 1, 3, 4 are never blurred
//   r_webSSAODebug     0 off, 1 view-space depth, 2 occlusion only (white = open), 3 normals (magenta = no depth), 4 raw occlusion x8
#include <universal/q_shared.h>
#include <Windows.h>
#include <qcommon/qcommon.h>

static const dvar_s *r_webSSAO, *r_webSSAORadius, *r_webSSAOStrength, *r_webSSAOBias, *r_webSSAODebug, *r_webSSAOBlur;

static void SsaoInit()
{
    if (r_webSSAO) return;
    const uint16_t f = (uint16_t)DVAR_ARCHIVE;
    r_webSSAO         = Dvar_RegisterBool("r_webSSAO", true, f, "web: screen-space ambient occlusion (forces the float-z pass on)");
    r_webSSAORadius   = Dvar_RegisterFloat("r_webSSAORadius", 40.0f, 4.0f, 200.0f, f, "web SSAO: sample radius in world units");
    r_webSSAOStrength = Dvar_RegisterFloat("r_webSSAOStrength", 1.0f, 0.0f, 3.0f, f, "web SSAO: darkness");
    r_webSSAOBias     = Dvar_RegisterFloat("r_webSSAOBias", 0.12f, 0.0f, 1.0f, f, "web SSAO: ignore neighbours flatter than this");
    r_webSSAOBlur     = Dvar_RegisterBool("r_webSSAOBlur", true, f, "web SSAO: depth-aware blur of the occlusion (removes the sampling grain; costs two more full-screen passes)");
    r_webSSAODebug    = Dvar_RegisterInt("r_webSSAODebug", 0, 0, 4, 0, "web SSAO: 1 = depth, 2 = AO only, 3 = normals, 4 = raw occlusion x8");
}

// r_scene.cpp: should this view build the float-z pass for SSAO?
extern "C" int kisak_ssao_wanted(void)
{
    SsaoInit();
    return r_webSSAO->current.enabled ? 1 : 0;
}

// GL device, once per scene: 0 = off, else out[0..4] = radius, strength, bias, debug mode, blur (0/1).
extern "C" int kisak_ssao_params(float *out)
{
    SsaoInit();
    if (!r_webSSAO->current.enabled) return 0;
    out[0] = r_webSSAORadius->current.value;
    out[1] = r_webSSAOStrength->current.value;
    out[2] = r_webSSAOBias->current.value;
    out[3] = (float)r_webSSAODebug->current.integer;
    out[4] = r_webSSAOBlur->current.enabled ? 1.0f : 0.0f;
    return 1;
}
