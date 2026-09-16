#ifndef SDL_GPU_CONTEXT_H
#define SDL_GPU_CONTEXT_H

struct SDL_Window;
struct SDL_GPUDevice;
class gxRuntime;

namespace sdlgpu {

SDL_Window* CreateGameWindow(int clientW, int clientH, bool resizable, bool borderless, bool fullscreen, const char* title);

void* GetHWND(SDL_Window* win);
void DestroyGameWindow(SDL_Window* win);
void SetWindowTitle(SDL_Window* win, const char* title);
void SizeWindowForClient(SDL_Window* win, int clientW, int clientH);
void CenterWindow(SDL_Window* win);
void ShowGameWindow(SDL_Window* win);
void SetWindowFullscreen(SDL_Window* win, bool fullscreen);
void SetCursorVisible(bool vis);
void PumpEvents(SDL_Window* win, ::gxRuntime* rt);
int SdlScancodeToDIK(int sdlScancode);
SDL_GPUDevice* CreateGPUDevice();
void DestroyGPUDevice(SDL_GPUDevice* dev);
bool ClaimWindow(SDL_GPUDevice* dev, SDL_Window* win);
void ReleaseWindow(SDL_GPUDevice* dev, SDL_Window* win);
bool PresentSwapchain(SDL_GPUDevice* dev, SDL_Window* win, float r, float g, float b);
void SetVSync(SDL_GPUDevice* dev, SDL_Window* win, bool vsync);

}

#endif
