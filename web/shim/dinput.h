// dinput.h shim -- DirectInput is replaced by browser input events in the web build (see web/platform/input_web.cpp).
// Only the typedefs that engine headers (win_local.h) mention are provided.
#pragma once
#include <Windows.h>
struct IDirectInput8A; struct IDirectInputDevice8A;
typedef IDirectInput8A *LPDIRECTINPUT, *LPDIRECTINPUT8;
typedef IDirectInputDevice8A *LPDIRECTINPUTDEVICE, *LPDIRECTINPUTDEVICE8;
