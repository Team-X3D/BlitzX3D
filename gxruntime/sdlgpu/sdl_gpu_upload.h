#ifndef SDL_GPU_UPLOAD_H
#define SDL_GPU_UPLOAD_H

namespace sdlgpu {

typedef bool(*UploadFn)(void* ctx);

struct UploadHandle;

UploadHandle* EnqueueUpload(UploadFn fn, void* ctx);
bool WaitUpload(UploadHandle* h);
void ShutdownUploads();

}

#endif
