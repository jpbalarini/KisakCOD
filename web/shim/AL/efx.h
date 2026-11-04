// AL/efx.h shim -- Emscripten's OpenAL has no EFX. KisakCOD's OpenAL backend uses EFX filters/reverb; for the
// web build these entry points are provided by a Web Audio backend (milestone M3). Declarations only.
#pragma once
#include <AL/al.h>
#ifdef __cplusplus
extern "C" {
#endif
#define AL_FILTER_TYPE 0x8001
#define AL_FILTER_NULL 0x0000
#define AL_FILTER_LOWPASS 0x0001
#define AL_LOWPASS_GAIN 0x0001
#define AL_LOWPASS_GAINHF 0x0002
#define AL_DIRECT_FILTER 0x20005
#define AL_AUXILIARY_SEND_FILTER 0x20006
#define AL_EFFECT_TYPE 0x8001
#define AL_EFFECT_NULL 0x0000
#define AL_EFFECT_REVERB 0x0001
#define AL_EFFECT_EAXREVERB 0x8000
#define AL_EFFECTSLOT_EFFECT 0x0001
#define AL_EFFECTSLOT_GAIN 0x0002
#define AL_EFFECTSLOT_AUXILIARY_SEND_AUTO 0x0003
#define AL_EFFECTSLOT_NULL 0x0000
#define AL_EAXREVERB_DENSITY 0x0001
#define AL_EAXREVERB_DIFFUSION 0x0002
#define AL_EAXREVERB_GAIN 0x0003
#define AL_EAXREVERB_GAINHF 0x0004
#define AL_EAXREVERB_GAINLF 0x0005
#define AL_EAXREVERB_DECAY_TIME 0x0006
#define AL_EAXREVERB_DECAY_HFRATIO 0x0007
#define AL_EAXREVERB_DECAY_LFRATIO 0x0008
#define AL_EAXREVERB_REFLECTIONS_GAIN 0x0009
#define AL_EAXREVERB_REFLECTIONS_DELAY 0x000A
#define AL_EAXREVERB_REFLECTIONS_PAN 0x000B
#define AL_EAXREVERB_LATE_REVERB_GAIN 0x000C
#define AL_EAXREVERB_LATE_REVERB_DELAY 0x000D
#define AL_EAXREVERB_LATE_REVERB_PAN 0x000E
#define AL_EAXREVERB_ECHO_TIME 0x000F
#define AL_EAXREVERB_ECHO_DEPTH 0x0010
#define AL_EAXREVERB_MODULATION_TIME 0x0011
#define AL_EAXREVERB_MODULATION_DEPTH 0x0012
#define AL_EAXREVERB_AIR_ABSORPTION_GAINHF 0x0013
#define AL_EAXREVERB_HFREFERENCE 0x0014
#define AL_EAXREVERB_LFREFERENCE 0x0015
#define AL_EAXREVERB_ROOM_ROLLOFF_FACTOR 0x0016
#define AL_EAXREVERB_DECAY_HFLIMIT 0x0017
typedef ALuint ALuint_efx;
void alGenEffects(ALsizei n, ALuint *effects);
void alDeleteEffects(ALsizei n, const ALuint *effects);
void alEffecti(ALuint effect, ALenum param, ALint value);
void alEffectf(ALuint effect, ALenum param, ALfloat value);
void alEffectfv(ALuint effect, ALenum param, const ALfloat *values);
void alGenFilters(ALsizei n, ALuint *filters);
void alDeleteFilters(ALsizei n, const ALuint *filters);
void alFilteri(ALuint filter, ALenum param, ALint value);
void alFilterf(ALuint filter, ALenum param, ALfloat value);
void alGenAuxiliaryEffectSlots(ALsizei n, ALuint *slots);
void alDeleteAuxiliaryEffectSlots(ALsizei n, const ALuint *slots);
void alAuxiliaryEffectSloti(ALuint slot, ALenum param, ALint value);
#ifdef __cplusplus
}
#endif
