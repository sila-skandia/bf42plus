// this file is responsible for the initial hooking
// DllMain is called first, that hooks the game's main() function
// the hooked main function initializes all the other hooks, then calls the original main function

#include "hooks.h"
#include "bfhook.h"
#include "debug.h"
#include "version.h"
#include "settings.h"
#include "profiling.h"
#include "bf/console.h"
#include "replay.h"

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>


typedef int __stdcall WinMain_t(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd);
WinMain_t* WinMain_orig = 0;
int __stdcall WinMain_hook(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{

    debuglogt("BF1942 started, plus version: %ls parameters: %s\n", build_version, lpCmdLine);

    g_settings.load();

    bfhook_init();

    bool forceWindowMode = false;

    if (strstr(lpCmdLine, " +forceWindow") != 0) {
        forceWindowMode = true;
    }

#if !defined(REDUCE_AV_SCORE)
    if (GetAsyncKeyState(VK_LSHIFT) & 0x8000) {
        if (MessageBoxA(0, "Left Shift pressed\nStart in window mode?", "BF42Plus", MB_YESNO) == IDYES) {
            forceWindowMode = true;
        }
    }
#endif

    if (forceWindowMode) {
#ifndef TARGET_BF1942_R
        nop_bytes(0x00442686, 2); // force 0 in Setup__setFullScreen
#else
        nop_bytes(0x004715D6, 2); // force 0 in Setup__setFullScreen
#endif
    }

    register_custom_console_commands();

    return WinMain_orig(hInstance, hPrevInstance, lpCmdLine, nShowCmd);
}

extern "C" __declspec(dllexport)
HRESULT WINAPI DirectSoundCreate8(LPCGUID lpcGuidDevice, void** ppDS8, void* pUnkOuter)
{
    char path[MAX_PATH] = ".\\dsound_next.dll";
    HMODULE dsounddll = LoadLibraryA(path);
    if (dsounddll == 0) {
        GetSystemDirectoryA(path, MAX_PATH);
        strcat_s(path, sizeof(path), "\\dsound.dll");
        dsounddll = LoadLibraryA(path);
    }

    if (dsounddll != 0) {
    }
    else {
        MessageBoxA(0, path, "failed to load dsound.dll", 0);
        return 0;
    }

    typedef HRESULT WINAPI Direct3DCreate8_t(LPCGUID lpcGuidDevice, void** ppDS8, void* pUnkOuter);
    Direct3DCreate8_t* DirectSoundCreate8_orig = (Direct3DCreate8_t*)(void*)GetProcAddress(dsounddll, "DirectSoundCreate8");

    return DirectSoundCreate8_orig(lpcGuidDevice, ppDS8, pUnkOuter);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    (void)lpReserved;
    switch (ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH:

#ifndef TARGET_BF1942_R
            if (memcmp((void*)0x00804DA6, "\xE8\x95\x19\xC0\xFF", 5) != 0)
#else
            if (memcmp((void*)0x0092227C, "\xE8\x84\xA1\xAE\xFF", 5) != 0)
#endif
            {
                MessageBoxA(NULL, "bf42++ is being injected into unsupported executable. The injection will be cancelled", NULL, MB_OK);
                break;
            }

#ifndef TARGET_BF1942_R
            WinMain_orig = (WinMain_t*)modify_call(0x00804DA6, (void*)WinMain_hook);
#else
            WinMain_orig = (WinMain_t*)modify_call(0x0092227C, (void*)WinMain_hook);
#endif
            break;
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;
        case DLL_PROCESS_DETACH:
            // The client exits and relaunches on every map change; close the
            // replay there so its tail is flushed and it ends with an "end" record.
            replay_stop();
            break;
    }
    return TRUE;
}

