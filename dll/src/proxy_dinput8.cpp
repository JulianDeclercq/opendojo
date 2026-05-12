// Forwarded exports for dinput8.dll proxy. Names match the .def file.
//
// Strategy: at DllMain attach we LoadLibraryW the real C:\Windows\System32\
// dinput8.dll and GetProcAddress each export. The exported symbols below
// are pure trampolines that call the resolved pointer. The game never
// sees the difference.

#include <windows.h>
#include <unknwn.h>   // IUnknown, REFCLSID, REFIID, LPUNKNOWN

#include "proxy.hpp"
#include "log.hpp"

namespace {

using DirectInput8Create_fn  = HRESULT (WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DllCanUnloadNow_fn     = HRESULT (WINAPI*)();
using DllGetClassObject_fn   = HRESULT (WINAPI*)(REFCLSID, REFIID, LPVOID*);
using DllRegisterServer_fn   = HRESULT (WINAPI*)();
using DllUnregisterServer_fn = HRESULT (WINAPI*)();
using GetdfDIJoystick_fn     = void*   (WINAPI*)();

HMODULE                g_real                  = nullptr;
DirectInput8Create_fn  p_DirectInput8Create    = nullptr;
DllCanUnloadNow_fn     p_DllCanUnloadNow       = nullptr;
DllGetClassObject_fn   p_DllGetClassObject     = nullptr;
DllRegisterServer_fn   p_DllRegisterServer     = nullptr;
DllUnregisterServer_fn p_DllUnregisterServer   = nullptr;
GetdfDIJoystick_fn     p_GetdfDIJoystick       = nullptr;

}  // namespace

bool opendojo::proxy::load() {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return false;
    wcscat_s(path, MAX_PATH, L"\\dinput8.dll");

    g_real = LoadLibraryW(path);
    if (!g_real) {
        OPENDOJO_LOG("proxy: LoadLibraryW(%ls) failed, GetLastError=%lu",
                    path, GetLastError());
        return false;
    }

    p_DirectInput8Create  = reinterpret_cast<DirectInput8Create_fn>(GetProcAddress(g_real, "DirectInput8Create"));
    p_DllCanUnloadNow     = reinterpret_cast<DllCanUnloadNow_fn>    (GetProcAddress(g_real, "DllCanUnloadNow"));
    p_DllGetClassObject   = reinterpret_cast<DllGetClassObject_fn>  (GetProcAddress(g_real, "DllGetClassObject"));
    p_DllRegisterServer   = reinterpret_cast<DllRegisterServer_fn>  (GetProcAddress(g_real, "DllRegisterServer"));
    p_DllUnregisterServer = reinterpret_cast<DllUnregisterServer_fn>(GetProcAddress(g_real, "DllUnregisterServer"));
    p_GetdfDIJoystick     = reinterpret_cast<GetdfDIJoystick_fn>    (GetProcAddress(g_real, "GetdfDIJoystick"));

    // DirectInput8Create is the only one Tekken is known to call. Missing it
    // means we have the wrong DLL or a Windows version we haven't seen.
    if (!p_DirectInput8Create) {
        OPENDOJO_LOG("proxy: real dinput8.dll missing DirectInput8Create — aborting");
        return false;
    }
    return true;
}

void opendojo::proxy::unload() {
    if (g_real) {
        FreeLibrary(g_real);
        g_real = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Forwarded exports. extern "C" + the matching .def entry pins these names
// so the linker exports them undecorated, exactly as dinput8.dll does.
// ---------------------------------------------------------------------------
extern "C" {

HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* p, LPUNKNOWN u) {
    return p_DirectInput8Create ? p_DirectInput8Create(h, v, r, p, u) : E_FAIL;
}
HRESULT WINAPI DllCanUnloadNow(void) {
    return p_DllCanUnloadNow ? p_DllCanUnloadNow() : S_OK;
}
HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, LPVOID* p) {
    return p_DllGetClassObject ? p_DllGetClassObject(c, r, p) : E_FAIL;
}
HRESULT WINAPI DllRegisterServer(void) {
    return p_DllRegisterServer ? p_DllRegisterServer() : S_OK;
}
HRESULT WINAPI DllUnregisterServer(void) {
    return p_DllUnregisterServer ? p_DllUnregisterServer() : S_OK;
}
void* WINAPI GetdfDIJoystick(void) {
    return p_GetdfDIJoystick ? p_GetdfDIJoystick() : nullptr;
}

}  // extern "C"
