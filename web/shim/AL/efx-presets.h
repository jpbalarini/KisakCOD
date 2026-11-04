// AL/efx-presets.h shim -- reverb preset table (EAX reverb parameters). Values are filled in with the web audio backend (M3);
// the structure matches OpenAL Soft's EFXEAXREVERBPROPERTIES so the existing table in snd_driver_openal.cpp compiles.
#pragma once
typedef struct { float flDensity, flDiffusion, flGain, flGainHF, flGainLF, flDecayTime, flDecayHFRatio, flDecayLFRatio,
    flReflectionsGain, flReflectionsDelay, flReflectionsPan[3], flLateReverbGain, flLateReverbDelay, flLateReverbPan[3],
    flEchoTime, flEchoDepth, flModulationTime, flModulationDepth, flAirAbsorptionGainHF, flHFReference, flLFReference,
    flRoomRolloffFactor; int iDecayHFLimit; } EFXEAXREVERBPROPERTIES, *LPEFXEAXREVERBPROPERTIES;
#define EFX_DEF_PRESET_(name, dens, diff, gain, gainhf, decay, decayhf, refl, refld, late, lated) \
    { dens, diff, gain, gainhf, 1.0f, decay, decayhf, 1.0f, refl, refld, {0.0f, 0.0f, 0.0f}, late, lated, {0.0f, 0.0f, 0.0f}, 0.25f, 0.0f, 0.25f, 0.0f, 0.994f, 5000.0f, 250.0f, 0.0f, 1 }
#define EFX_REVERB_PRESET_GENERIC EFX_DEF_PRESET_(GENERIC, 1.0f, 1.0f, 0.3162f, 0.8913f, 1.49f, 0.83f, 0.0500f, 0.007f, 1.2589f, 0.011f)
#define EFX_REVERB_PRESET_PADDEDCELL EFX_DEF_PRESET_(PADDEDCELL, 0.1715f, 1.0f, 0.3162f, 0.0010f, 0.17f, 0.10f, 0.2500f, 0.001f, 1.2691f, 0.002f)
#define EFX_REVERB_PRESET_ROOM EFX_DEF_PRESET_(ROOM, 0.4287f, 1.0f, 0.3162f, 0.5929f, 0.40f, 0.83f, 0.1503f, 0.002f, 1.0629f, 0.003f)
#define EFX_REVERB_PRESET_LIVINGROOM EFX_DEF_PRESET_(LIVINGROOM, 0.9766f, 1.0f, 0.3162f, 0.0010f, 0.50f, 0.10f, 0.2051f, 0.003f, 0.2805f, 0.004f)
#define EFX_REVERB_PRESET_STONEROOM EFX_DEF_PRESET_(STONEROOM, 1.0f, 1.0f, 0.3162f, 0.7079f, 2.31f, 0.64f, 0.4411f, 0.012f, 1.1003f, 0.017f)
#define EFX_REVERB_PRESET_HANGAR EFX_DEF_PRESET_(HANGAR, 1.0f, 1.0f, 0.3162f, 0.3162f, 10.05f, 0.23f, 0.5000f, 0.020f, 1.2560f, 0.030f)
#define EFX_REVERB_PRESET_HALLWAY EFX_DEF_PRESET_(HALLWAY, 0.3645f, 1.0f, 0.3162f, 0.7079f, 1.49f, 0.59f, 0.2458f, 0.007f, 1.6615f, 0.011f)
#define EFX_REVERB_PRESET_STONECORRIDOR EFX_DEF_PRESET_(STONECORRIDOR, 1.0f, 1.0f, 0.3162f, 0.7612f, 2.70f, 0.79f, 0.2472f, 0.013f, 1.5758f, 0.020f)
#define EFX_REVERB_PRESET_QUARRY EFX_DEF_PRESET_(QUARRY, 1.0f, 1.0f, 0.3162f, 0.3162f, 1.49f, 0.83f, 0.0000f, 0.061f, 1.7783f, 0.025f)
#define EFX_REVERB_PRESET_PLAIN EFX_DEF_PRESET_(PLAIN, 0.2f, 1.0f, 0.3162f, 0.1000f, 1.49f, 0.50f, 0.0585f, 0.179f, 0.1089f, 0.100f)
#define EFX_REVERB_PRESET_PARKINGLOT EFX_DEF_PRESET_(PARKINGLOT, 1.0f, 1.0f, 0.3162f, 1.0f, 1.65f, 1.50f, 0.2082f, 0.008f, 0.2652f, 0.012f)
#define EFX_REVERB_PRESET_SEWERPIPE EFX_DEF_PRESET_(SEWERPIPE, 0.3071f, 0.8f, 0.3162f, 0.3162f, 2.81f, 0.14f, 1.6387f, 0.014f, 3.2471f, 0.021f)
#define EFX_REVERB_PRESET_UNDERWATER EFX_DEF_PRESET_(UNDERWATER, 0.3645f, 1.0f, 0.3162f, 0.0100f, 1.49f, 0.10f, 0.5963f, 0.007f, 7.0795f, 0.011f)
#define EFX_REVERB_PRESET_MOUNTAINS EFX_DEF_PRESET_(MOUNTAINS, 1.0f, 0.27f, 0.3162f, 0.0562f, 1.49f, 0.21f, 0.0407f, 0.300f, 0.1919f, 0.100f)
#define EFX_REVERB_PRESET_PSYCHOTIC EFX_DEF_PRESET_(PSYCHOTIC, 0.0625f, 0.5f, 0.3162f, 0.8404f, 7.56f, 0.91f, 0.4864f, 0.020f, 2.4378f, 0.030f)

// Remaining presets used by snd_driver_openal.cpp: placeholder (generic-room) values until the web audio backend (M3)
#define EFX_REVERB_PRESET_ALLEY EFX_REVERB_PRESET_GENERIC
#define EFX_REVERB_PRESET_ARENA EFX_REVERB_PRESET_GENERIC
#define EFX_REVERB_PRESET_AUDITORIUM EFX_REVERB_PRESET_GENERIC
#define EFX_REVERB_PRESET_BATHROOM EFX_REVERB_PRESET_GENERIC
#define EFX_REVERB_PRESET_CARPETEDHALLWAY EFX_REVERB_PRESET_GENERIC
#define EFX_REVERB_PRESET_CAVE EFX_REVERB_PRESET_GENERIC
#define EFX_REVERB_PRESET_CITY EFX_REVERB_PRESET_GENERIC
#define EFX_REVERB_PRESET_CONCERTHALL EFX_REVERB_PRESET_GENERIC
#define EFX_REVERB_PRESET_DIZZY EFX_REVERB_PRESET_GENERIC
#define EFX_REVERB_PRESET_DRUGGED EFX_REVERB_PRESET_GENERIC
#define EFX_REVERB_PRESET_FOREST EFX_REVERB_PRESET_GENERIC
