// stubs_web.cpp -- web-build stand-ins for engine entry points that live in files not (yet) built for the browser:
//   win_net.cpp (Winsock) -> net_web.cpp                 win_input.cpp (DirectInput) -> browser input events
//   win_voice.cpp (voice chat)                           r_screenshot.cpp (GDI+ cubemap capture, dev tools only)
//   r_model_skin_sse.cpp (MMX skinning) -> wasm-simd rewrite
// Each stub logs once so we notice when a code path that needs a real implementation is reached.
#include <universal/q_shared.h>
#include <Windows.h>
#include <win32/win_local.h>
#include <win32/win_net.h>
#include <win32/win_input.h>
#include <gfx_d3d/r_model_skin.h>
#include <gfx_d3d/r_screenshot.h>
#include <stdio.h>

static void stubHit(const char *name)
{
    static const int kMax = 64; static const char *seen[kMax]; static int n = 0;
    for (int i = 0; i < n; ++i) if (seen[i] == name) return;
    if (n < kMax) seen[n++] = name;
    fprintf(stderr, "[web-stub] %s called (no web implementation yet)\n", name);
}
#define STUB(name) stubHit(name)

// ---- networking (win_net.cpp): see net_web.cpp ----

// ---- input: see input_web.cpp ----

// ---- voice (win_voice.cpp) ----
double __cdecl Voice_GetVoiceLevel() { return 0.0; }
bool __cdecl Voice_IsClientTalking(uint32_t) { return false; }

// ---- dev-only cubemap capture (r_screenshot.cpp) ----
void __cdecl R_BeginCubemapShot(int, int) { STUB("R_BeginCubemapShot"); }
void __cdecl R_EndCubemapShot(CubemapShot) { STUB("R_EndCubemapShot"); }
void __cdecl R_CreateReflectionRawDataFromCubemapShot(struct DiskGfxReflectionProbe *, int) { STUB("R_CreateReflectionRawDataFromCubemapShot"); }

// ======================================================================= more stubs needed to link the browser build
#include <binklib/bink.h>
#include <AL/efx.h>
#include <gfx_d3d/r_texturemem.h>
#include <win32/win_net.h>

// ---- input / net / voice lifecycle (real implementations arrive with M3/M4)
void __cdecl Sys_ShowConsole() {}
bool __cdecl Voice_Init() { return true; }
void __cdecl Voice_Playback() {}
void __cdecl Voice_Shutdown() {}
int __cdecl Voice_GetLocalVoiceData() { return 0; }
void __cdecl Voice_IncomingVoiceData(unsigned __int8, unsigned __int8 *, int) {}
uint32_t __cdecl R_AvailableTextureMemory() { return 512u << 20; }
void __cdecl R_SaveCubemapShot(char *, CubemapShot, float, float) { STUB("R_SaveCubemapShot"); }
void __cdecl R_ScreenshotCommand(GfxScreenshotType) { STUB("R_ScreenshotCommand"); }
void __cdecl R_LightingFromCubemapShots(const float *) { STUB("R_LightingFromCubemapShots"); }

// ---- Bink video: report "cannot open" so the engine skips cinematics
extern "C" {
HBINK BinkOpen(const char *, U32) { return 0; }
char *BinkGetError(void) { static char e[] = "bink video is not available in the web build"; return e; }
void BinkClose(HBINK) {}
S32 BinkDoFrame(HBINK) { return 0; }
void BinkNextFrame(HBINK) {}
S32 BinkWait(HBINK) { return 1; }
S32 BinkPause(HBINK, S32) { return 0; }
S32 BinkGetRects(HBINK, U32) { return 0; }
void BinkGetRealtime(HBINK, BINKREALTIME *, U32) {}
void BinkGetFrameBuffersInfo(HBINK, BINKFRAMEBUFFERS *) {}
void BinkRegisterFrameBuffers(HBINK, BINKFRAMEBUFFERS *) {}
S32 BinkControlBackgroundIO(HBINK, U32) { return 0; }
void BinkSetIOSize(U32) {}
void BinkSetMemory(void *(*)(U32), void (*)(void *)) {}
void BinkSetMixBinVolumes(HBINK, U32, U32 *, S32 *, U32) {}
void BinkSetSoundTrack(U32, U32 *) {}

// ---- Steam
int SteamAPI_GetHSteamUser() { return 0; }
void SteamAPI_RunCallbacks() {}
void *SteamInternal_ContextInit(void *) { return nullptr; }
void *SteamInternal_FindOrCreateUserInterface(int, const char *) { return nullptr; }

// ---- OpenAL EFX (reverb / filters): no-ops until the Web Audio backend (M3)
void alGenEffects(ALsizei n, ALuint *e) { for (ALsizei i = 0; i < n; ++i) e[i] = i + 1; }
void alDeleteEffects(ALsizei, const ALuint *) {}
void alEffecti(ALuint, ALenum, ALint) {}
void alEffectf(ALuint, ALenum, ALfloat) {}
void alEffectfv(ALuint, ALenum, const ALfloat *) {}
void alGenFilters(ALsizei n, ALuint *f) { for (ALsizei i = 0; i < n; ++i) f[i] = i + 1; }
void alDeleteFilters(ALsizei, const ALuint *) {}
void alFilteri(ALuint, ALenum, ALint) {}
void alFilterf(ALuint, ALenum, ALfloat) {}
void alGenAuxiliaryEffectSlots(ALsizei n, ALuint *s) { for (ALsizei i = 0; i < n; ++i) s[i] = i + 1; }
void alDeleteAuxiliaryEffectSlots(ALsizei, const ALuint *) {}
void alAuxiliaryEffectSloti(ALuint, ALenum, ALint) {}
}

// ---- MMX skinning kernel (r_model_skin_sse.cpp): never selected because sys_info.SSE is 0 in the web build
#include <gfx_d3d/r_model_skin.h>
void __cdecl R_SkinXSurfaceSkinnedSse(const XSurface *, const DObjSkelMat *, GfxPackedVertexNormal *, GfxPackedVertexNormal *, GfxPackedVertex *)
{
    fprintf(stderr, "R_SkinXSurfaceSkinnedSse called in the web build (sys_SSE should be off)\n");
    __builtin_trap();
}
