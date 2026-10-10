#ifndef SDL_GPU_UPLOAD_H
#define SDL_GPU_UPLOAD_H

struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;

namespace sdlgpu {

typedef bool(*UploadFn)(void* ctx, SDL_GPUCommandBuffer* cmds);

struct UploadHandle;

UploadHandle* EnqueueUpload(SDL_GPUDevice* dev, UploadFn fn, void* ctx);
bool WaitUpload(UploadHandle* h);
void ShutdownUploads();

}

#endif
