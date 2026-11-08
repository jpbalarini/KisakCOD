// grade_web.cpp -- dvars for the web build's final colour-grade pass (web/gfx/d3d9_gl_device.cpp, Device::applyGrade).
//
// The grade runs on the finished frame (after the engine's own glow / depth of field / colour manipulation), in LDR, right before the
// browser presents it. It is a *look* (filmic curve, contrast, saturation, vignette, sharpen), not HDR tone mapping: the engine's
// scene target is 8 bit, so there is nothing brighter than 1.0 left to compress by then.
//
// Console / URL (`?args=%2Bset%20r_webGrade%200`): all of these are live, no restart.
//   r_webGrade       0/1   master switch (0 = the old plain blit)
//   r_webExposure    0.25..3   multiplier before the curve
//   r_webFilmic      0..1  blend towards an ACES-style S-curve (white point kept at white)
//   r_webContrast    0.5..2    around mid grey, after the curve
//   r_webSaturation  0..2
//   r_webVignette    0..1
//   r_webSharpen     0..1  unsharp mask (4 neighbours)
#include <universal/q_shared.h>
#include <Windows.h>
#include <qcommon/qcommon.h>

static const dvar_s *r_webGrade, *r_webExposure, *r_webFilmic, *r_webContrast, *r_webSaturation, *r_webVignette, *r_webSharpen;

// Called by the GL device once per presented frame. Returns 0 when the grade is off; otherwise fills
// out[0..5] = exposure, filmic, contrast, saturation, vignette, sharpen.
extern "C" int kisak_grade_params(float *out)
{
    if (!r_webGrade) {   // first present: the engine's dvar system is up by now
        const uint16_t f = (uint16_t)DVAR_ARCHIVE;
        r_webGrade      = Dvar_RegisterBool("r_webGrade", true, f, "web: final colour-grade pass (filmic curve, contrast, saturation, vignette, sharpen)");
        r_webExposure   = Dvar_RegisterFloat("r_webExposure", 1.0f, 0.25f, 3.0f, f, "web grade: exposure multiplier");
        r_webFilmic     = Dvar_RegisterFloat("r_webFilmic", 0.35f, 0.0f, 1.0f, f, "web grade: blend towards an ACES-style filmic curve");
        r_webContrast   = Dvar_RegisterFloat("r_webContrast", 1.05f, 0.5f, 2.0f, f, "web grade: contrast around mid grey");
        r_webSaturation = Dvar_RegisterFloat("r_webSaturation", 1.10f, 0.0f, 2.0f, f, "web grade: saturation");
        r_webVignette   = Dvar_RegisterFloat("r_webVignette", 0.15f, 0.0f, 1.0f, f, "web grade: edge darkening");
        r_webSharpen    = Dvar_RegisterFloat("r_webSharpen", 0.25f, 0.0f, 1.0f, f, "web grade: unsharp-mask strength");
    }
    if (!r_webGrade->current.enabled) return 0;
    out[0] = r_webExposure->current.value;
    out[1] = r_webFilmic->current.value;
    out[2] = r_webContrast->current.value;
    out[3] = r_webSaturation->current.value;
    out[4] = r_webVignette->current.value;
    out[5] = r_webSharpen->current.value;
    return 1;
}
