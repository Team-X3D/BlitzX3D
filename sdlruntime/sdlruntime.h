#ifndef SDLRUNTIME_H
#define SDLRUNTIME_H

#include <Windows.h>
#include <d3d9.h>
#include <string>
#include <vector>
#include <intrin.h>

#include "sdlaudio.h"
#include "sdlinput.h"
#include "sdlgraphics.h"
#include "sdlfilesystem.h"
#include "sdltimer.h"

#include "../debugger/debugger.h"

struct SDL_Window;
struct SDL_GPUDevice;

class sdlRuntime {
	/***** INTERNAL INTERFACE *****/
public:
    HWND hwnd;
    HINSTANCE hinst;

    SDL_Window* sdlWindow = nullptr;
    SDL_GPUDevice* sdlGpu = nullptr;
    bool vwaitPending = false;
    bool vwaitValue = true;
    bool sceneBeganSinceFlip = false;
    HWND savedHwnd = nullptr;
    bool usingSDLWindow() const { return sdlWindow != nullptr; }
    void pumpSDLWindowEvents();
    void destroySDLWindow();
    void setFullscreenState(bool fullscreen);

    sdlAudio* audio;
    sdlInput* input;
    sdlGraphics* graphics;
    sdlFileSystem* fileSystem;

    IDirect3D9Ex* d3d;
    IDirect3DDevice9Ex* d3dDevice;
    IDirect3DSurface9* backBuffer;
    IDirect3DSurface9* frontBuffer;

    IDirect3DSurface9* stretchRT;
    int stretchRT_w, stretchRT_h;

    D3DPRESENT_PARAMETERS d3dpp;
    D3DDISPLAYMODEEX d3ddmEx;

    bool requested_antialias = false;

    float scale_x = .0f, scale_y = .0f;

    void flip(bool vwait);
    void pumpMessages();
    void moveMouse(int x, int y);
    LRESULT windowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);

    void applyAntialiasToParams(D3DPRESENT_PARAMETERS& pp);

    struct GfxMode;
    struct GfxDriver;

private:
    sdlRuntime(HINSTANCE hinst, const std::string& cmd_line, HWND hwnd);
    ~sdlRuntime();

    void paint();
    void maybePresentConsole();
    void suspend();
    void forceSuspend();
    void resume();
    void forceResume();
    void backupWindowState();
    void restoreWindowState();

    RECT t_rect;
    int t_style;
    sdlCanvas* console_canvas = nullptr;
    int console_mod = 0;
    std::string cmd_line;
    bool pointer_visible;
    std::string app_title;
    std::string app_close;

    bool setDisplayMode(int w, int h, int d, bool d3d);
    sdlGraphics* openWindowedGraphics(int w, int h, int d, bool d3d);
    sdlGraphics* openExclusiveGraphics(int w, int h, int d, bool d3d);

    bool enum_all;
    std::vector<GfxDriver*> drivers;
    GfxDriver* curr_driver;
    int use_di;

    void ensureD3D();
    void enumGfx();
    void denumGfx();

    void resetInput();
    void pauseAudio();
    void resumeAudio();
    void restoreGraphics();
    void acquireInput();
    void unacquireInput();

public:
    static sdlRuntime* openRuntime(HINSTANCE hinst, const std::string& cmd_line, Debugger* debugger);
    static void closeRuntime(sdlRuntime* runtime);

    void asyncStop();
    void asyncRun();
    void asyncEnd();

    enum {
        GFXMODECAPS_3D = 1
    };
    enum {
        GMODE_NONE = 0,
        GMODE_SCALED = 1,
        GMODE_FIXED = 2,
        GMODE_EXCLUSIVE = 3
    };

    bool idle();
    bool delay(int ms);
    bool execute(const std::string& cmd);
    void setTitle(const std::string& title, const std::string& close);
    int  getMilliSecs();
    void setPointerVisible(bool vis);
    std::string commandLine();
    std::string systemProperty(const std::string& t);

    void debugStop();
    bool debugStmt(int pos, const char* file);
    void debugEnter(void* frame, void* env, const char* func);
    void debugLeave();
    void debugInfo(const char* t);
    void debugError(const char* t);
    void debugLog(const char* t);
    void debugSys(void* msg);

    int numGraphicsDrivers();
    void graphicsDriverInfo(int driver, std::string* name, int* caps);
    int numGraphicsModes(int driver);
    void graphicsModeInfo(int driver, int mode, int* w, int* h, int* d, int* caps);
    void windowedModeInfo(int* caps);

    sdlAudio* openAudio(int flags);
    void closeAudio(sdlAudio* audio);

    sdlInput* openInput(int flags);
    void closeInput(sdlInput* input);

    sdlGraphics* openGraphics(int w, int h, int d, int driver, int flags);
    void closeGraphics(sdlGraphics* graphics);
    bool graphicsLost();
    bool focus();
    int desktopWidth();
    int desktopHeight();

    void setAntialiasRequest(bool enable) { requested_antialias = enable; }
    bool antialiasRequested() const { return requested_antialias; }

    D3DMULTISAMPLE_TYPE chooseMultisampleType(D3DFORMAT fmt, BOOL windowed, DWORD* quality);

    sdlFileSystem* openFileSystem(int flags);
    void closeFileSystem(sdlFileSystem* filesys);

    sdlTimer* createTimer(int hertz);
    void freeTimer(sdlTimer* timer);

    void calculateDPI();
    void enableDirectInput(bool enable);
    int  directInputEnabled() { return use_di; }

    int callDll(const std::string& dll, const std::string& func, const void* in, int in_sz, void* out, int out_sz);

    OSVERSIONINFO osinfo;
    MEMORYSTATUSEX statex;
    DEVMODE devmode;

    int getMemoryLoad();
    int getTotalPhys();
    int getAvailPhys();
    int getTotalVirtual();
    int getAvailVirtual();
};

#endif