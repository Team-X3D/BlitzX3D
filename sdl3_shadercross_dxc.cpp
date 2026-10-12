#include <windows.h>

extern "C" long __stdcall DxcCreateInstance(const void* rclsid, const void* riid, void** ppv)
{
    typedef long(__stdcall * DxcCreateInstanceFn)(const void*, const void*, void**);
    static DxcCreateInstanceFn fn = nullptr;
    static bool tried = false;
    if (!fn && !tried) {
        tried = true;
        HMODULE mod = LoadLibraryA("dxcompiler.dll");
        if (mod) fn = (DxcCreateInstanceFn)GetProcAddress(mod, "DxcCreateInstance");
    }
    if (!fn) return (long)0x80004005L;
    return fn(rclsid, riid, ppv);
}
