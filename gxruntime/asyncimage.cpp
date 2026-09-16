#include "std.h"
#include "asyncimage.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <chrono>

AsyncImageLoader& AsyncImageLoader::instance() {
	static AsyncImageLoader loader;
	return loader;
}

std::unique_ptr<DecodedImage> DecodeImageFile(const std::string& file, std::string* err) {
	auto fail = [&](const char* what) -> std::unique_ptr<DecodedImage> {
		if (err) {
			*err = std::string(what) + ": " + file + " (" + SDL_GetError() + ")";
			SDL_ClearError();
		}
		return nullptr;
	};

	SDL_Surface* surf = IMG_Load(file.c_str());
	if (!surf) return fail("Load failed");
	if (surf->w <= 0 || surf->h <= 0 || !surf->pixels) {
		SDL_DestroySurface(surf);
		return fail("Empty image");
	}
	if (surf->w > 8192 || surf->h > 8192 ||
		(size_t)surf->w * (size_t)surf->h > (size_t)32 * 1024 * 1024) {
		SDL_DestroySurface(surf);
		return fail("Image too large");
	}

	auto img = std::make_unique<DecodedImage>();
	img->w = surf->w;
	img->h = surf->h;
	try {
		img->rgba.resize((size_t)img->w * (size_t)img->h * 4);
	} catch (const std::bad_alloc&) {
		SDL_DestroySurface(surf);
		return fail("Out of memory");
	}

	bool converted = SDL_ConvertPixels(img->w, img->h, surf->format, surf->pixels, surf->pitch,
		SDL_PIXELFORMAT_RGBA32, img->rgba.data(), img->w * 4);
	if (!converted) {
		SDL_ClearError();
		SDL_Surface* cvt = SDL_ConvertSurface(surf, SDL_PIXELFORMAT_RGBA32);
		if (!cvt) {
			SDL_DestroySurface(surf);
			return fail("Convert failed");
		}
		for (int y = 0; y < img->h; ++y) {
			memcpy(img->rgba.data() + (size_t)y * img->w * 4,
				(const Uint8*)cvt->pixels + (size_t)y * cvt->pitch,
				(size_t)img->w * 4);
		}
		SDL_DestroySurface(cvt);
	}

	Uint8 keyR = 0, keyG = 0, keyB = 0;
	bool hasKey = false;
	if (SDL_SurfaceHasColorKey(surf)) {
		Uint32 key = 0;
		if (SDL_GetSurfaceColorKey(surf, &key)) {
			if (SDL_Palette* pal = SDL_GetSurfacePalette(surf)) {
				if (key < (Uint32)pal->ncolors) {
					keyR = pal->colors[key].r;
					keyG = pal->colors[key].g;
					keyB = pal->colors[key].b;
					hasKey = true;
				}
			} else if (const SDL_PixelFormatDetails* det = SDL_GetPixelFormatDetails(surf->format)) {
				SDL_GetRGB(key, det, nullptr, &keyR, &keyG, &keyB);
				hasKey = true;
			}
		}
	}
	SDL_DestroySurface(surf);

	bool hasAlpha = false;
	Uint8* px = img->rgba.data();
	for (int i = 0, n = img->w * img->h; i < n; ++i) {
		Uint8 r = px[0], g = px[1], b = px[2], a = px[3];
		if (hasKey && r == keyR && g == keyG && b == keyB) a = 0;
		if (a != 255) hasAlpha = true;
		px[0] = r;
		px[1] = g;
		px[2] = b;
		px[3] = a;
		px += 4;
	}
	img->hasAlpha = hasAlpha;

	return img;
}

AsyncImageLoader::AsyncImageLoader() :
	inFlight(0), shutdown(false) {
	unsigned count = std::thread::hardware_concurrency() / 2;
	if (count < 2) count = 2;
	if (count > 4) count = 4;
	for (unsigned n = 0; n < count; ++n) threads.push_back(std::thread(&AsyncImageLoader::worker, this));
}

AsyncImageLoader::~AsyncImageLoader() {
	{
		std::unique_lock<std::mutex> lock(mutex);
		shutdown = true;
	}
	cv.notify_all();
	for (auto& t : threads) t.join();
}

std::shared_ptr<AsyncImageLoader::Job> AsyncImageLoader::load(const std::string& file) {
	auto job = std::make_shared<Job>(file);
	{
		std::unique_lock<std::mutex> lock(mutex);
		queue.push_back(job);
	}
	cv.notify_one();
	return job;
}

void AsyncImageLoader::worker() {
	for (;;) {
		std::shared_ptr<Job> job;
		{
			std::unique_lock<std::mutex> lock(mutex);
			cv.wait(lock, [this]() { return shutdown || !queue.empty(); });
			if (shutdown && queue.empty()) return;
		job = queue.back();
		queue.pop_back();
		++inFlight;
		job->state.store(STATE_DECODING);
	}

	std::unique_ptr<DecodedImage> img;
	try {
		img = DecodeImageFile(job->file);
	} catch (...) {
		img.reset();
	}

		std::unique_lock<std::mutex> lock(mutex);
		if (job->state.load() == STATE_CANCELLED) {
			--inFlight;
			cv.notify_all();
			continue;
		}
		if (img) {
			job->image = std::move(img);
			job->state.store(STATE_DONE);
		}
		else {
			job->state.store(STATE_FAILED);
		}
		--inFlight;
		cv.notify_all();
	}
}

void AsyncImageLoader::wait(const std::shared_ptr<Job>& job) {
	std::unique_lock<std::mutex> lock(mutex);

	//decode synchronously as fallback to avoid infinite loads
	if (job->state.load() == STATE_QUEUED) {
		for (auto it = queue.begin(); it != queue.end(); ++it) {
			if (*it == job) { queue.erase(it); break; }
		}
		job->state.store(STATE_DECODING);
		lock.unlock();

		std::unique_ptr<DecodedImage> img;
		try {
			img = DecodeImageFile(job->file);
		} catch (...) {
			img.reset();
		}

		lock.lock();
		if (img) {
			job->image = std::move(img);
			job->state.store(STATE_DONE);
		}
		else {
			job->state.store(STATE_FAILED);
		}
		cv.notify_all();
		return;
	}

	cv.wait(lock, [&]() {
		int s = job->state.load();
		return s == STATE_DONE || s == STATE_FAILED || s == STATE_CANCELLED;
	});
}

void AsyncImageLoader::waitAll() {
	std::unique_lock<std::mutex> lock(mutex);
	cv.wait_for(lock, std::chrono::seconds(20), [this]() { return queue.empty() && inFlight == 0; });
}

void AsyncImageLoader::cancel(const std::shared_ptr<Job>& job) {
	std::unique_lock<std::mutex> lock(mutex);
	for (auto it = queue.begin(); it != queue.end(); ++it) {
		if (*it == job) {
			queue.erase(it);
			break;
		}
	}
	job->image.reset();
	job->state.store(STATE_CANCELLED);
}