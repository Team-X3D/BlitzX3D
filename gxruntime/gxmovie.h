#ifndef GXMOVIE_H
#define GXMOVIE_H

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>
#include <cstdint>

#include "gxcanvas.h"

class gxGraphics;

struct AVFormatContext;
struct AVCodecContext;
struct SwsContext;
struct AVFrame;
struct AVPacket;

//Who the fuck actually uses this?
//kid named everyone:
class gxMovie {

public:
	gxMovie(gxGraphics* gfx, const std::string& file);
	~gxMovie();

	bool isValid() const { return valid; }

	/***** GX INTERFACE *****/
public:
	std::string filename;
	bool draw(gxCanvas* dest, int x, int y, int w, int h);

	bool isPlaying() const { return playing.load(); }
	int getWidth()const { return src_w; }
	int getHeight()const { return src_h; }

private:
	gxGraphics* gfx;
	int src_w = 0, src_h = 0;

	std::atomic<bool> playing{ false };
	std::atomic<bool> valid{ false };
	std::atomic<bool> quit_requested{ false };
	std::atomic<bool> eof_reached{ false };

	std::mutex frame_mutex;
	std::vector<unsigned char> front_rgba;
	std::vector<unsigned char> back_rgba;
	int rgba_stride = 0;
	bool has_frame = false;
	uint64_t frame_serial = 0;
	uint64_t drawn_serial = (uint64_t)-1;

	double front_frame_pts = 0.0;
	std::chrono::steady_clock::time_point playback_start;
	double stream_time_base_first_pts = 0.0;

	std::thread decode_thread;
	gxCanvas* scratch_front = nullptr;
	gxCanvas* scratch_back = nullptr;
	int scratch_w = 0, scratch_h = 0;

	AVFormatContext* fmt_ctx = nullptr;
	AVCodecContext* codec_ctx = nullptr;
	SwsContext* sws_ctx = nullptr;
	int video_stream_index = -1;

	void decodeThreadMain();
	bool openStream(const std::string& file);
	void closeStream();
	void releaseAfterPlayback();
	bool released = false;
};

#endif