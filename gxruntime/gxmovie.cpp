#include "std.h"
#include "gxmovie.h"
#include "gxgraphics.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

gxMovie::gxMovie(gxGraphics* g, const std::string& file) : gfx(g), filename(file) {
	if (!openStream(file)) {
		valid = false;
		playing = false;
		return;
	}
	valid = true;
	playing = true;
	quit_requested = false;
	decode_thread = std::thread(&gxMovie::decodeThreadMain, this);
}

gxMovie::~gxMovie() {
	releaseAfterPlayback();
}

void gxMovie::releaseAfterPlayback() {
	if (released) return;
	released = true;
	valid = false;
	playing = false;
	quit_requested = true;
	if (decode_thread.joinable()) decode_thread.join();
	closeStream();
	front_rgba.clear(); front_rgba.shrink_to_fit();
	back_rgba.clear(); back_rgba.shrink_to_fit();
	{
		std::lock_guard<std::mutex> lock(frame_mutex);
		has_frame = false;
	}
	if (scratch_front) { gfx->freeCanvas(scratch_front); scratch_front = nullptr; }
	if (scratch_back) { gfx->freeCanvas(scratch_back); scratch_back = nullptr; }
}

bool gxMovie::openStream(const std::string& file) {
	fmt_ctx = nullptr;
	if (avformat_open_input(&fmt_ctx, file.c_str(), nullptr, nullptr) != 0) {
		return false;
	}

	if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
		avformat_close_input(&fmt_ctx);
		fmt_ctx = nullptr;
		return false;
	}

	video_stream_index = -1;
	for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			video_stream_index = (int)i;
			break;
		}
	}

	if (video_stream_index < 0) {
		avformat_close_input(&fmt_ctx);
		fmt_ctx = nullptr;
		return false;
	}

	AVCodecParameters* codecpar = fmt_ctx->streams[video_stream_index]->codecpar;
	const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
	if (!decoder) {
		avformat_close_input(&fmt_ctx);
		fmt_ctx = nullptr;
		return false;
	}

	codec_ctx = avcodec_alloc_context3(decoder);
	if (!codec_ctx) {
		avformat_close_input(&fmt_ctx);
		fmt_ctx = nullptr;
		return false;
	}

	if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0) {
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&fmt_ctx);
		fmt_ctx = nullptr;
		return false;
	}

	codec_ctx->thread_count = 0; // auto

	if (avcodec_open2(codec_ctx, decoder, nullptr) < 0) {
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&fmt_ctx);
		fmt_ctx = nullptr;
		return false;
	}

	src_w = codec_ctx->width;
	src_h = codec_ctx->height;
	rgba_stride = src_w * 4;
	{
		std::lock_guard<std::mutex> lock(frame_mutex);
		front_rgba.assign((size_t)rgba_stride * src_h, 0);
		back_rgba.assign((size_t)rgba_stride * src_h, 0);
	}

	sws_ctx = sws_getContext(
		src_w, src_h, codec_ctx->pix_fmt,
		src_w, src_h, AV_PIX_FMT_RGBA,
		SWS_BILINEAR, nullptr, nullptr, nullptr);

	if (!sws_ctx) {
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&fmt_ctx);
		fmt_ctx = nullptr;
		return false;
	}

	return true;
}

void gxMovie::closeStream() {
	if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }
	if (codec_ctx) { avcodec_free_context(&codec_ctx); codec_ctx = nullptr; }
	if (fmt_ctx) { avformat_close_input(&fmt_ctx); fmt_ctx = nullptr; }
}

void gxMovie::decodeThreadMain() {
	AVFrame* frame = av_frame_alloc();
	AVFrame* rgba_frame = av_frame_alloc();
	AVPacket* packet = av_packet_alloc();

	if (!frame || !rgba_frame || !packet) {
		valid = false;
		playing = false;
		if (frame) av_frame_free(&frame);
		if (rgba_frame) av_frame_free(&rgba_frame);
		if (packet) av_packet_free(&packet);
		return;
	}

	std::vector<unsigned char> rgba_buf((size_t)rgba_stride * src_h);
	av_image_fill_arrays(rgba_frame->data, rgba_frame->linesize, rgba_buf.data(), AV_PIX_FMT_RGBA, src_w, src_h, 1);
	const AVRational tb = fmt_ctx->streams[video_stream_index]->time_base;
	bool have_first_pts = false;
	double first_pts_seconds = 0.0;
	playback_start = std::chrono::steady_clock::now();

	while (!quit_requested.load()) {
		int read_ret = av_read_frame(fmt_ctx, packet);
		if (read_ret < 0) {
			eof_reached = true;
			playing = false;
			av_packet_unref(packet);
			break;
		}

		if (packet->stream_index != video_stream_index) {
			av_packet_unref(packet);
			continue;
		}

		int send_ret = avcodec_send_packet(codec_ctx, packet);
		av_packet_unref(packet);
		if (send_ret < 0) {
			continue;
		}

		while (!quit_requested.load()) {
			int recv_ret = avcodec_receive_frame(codec_ctx, frame);
			if (recv_ret == AVERROR(EAGAIN) || recv_ret == AVERROR_EOF) {
				break;
			}
			else if (recv_ret < 0) {
				break;
			}

			int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
				? frame->best_effort_timestamp : frame->pts;
			double pts_seconds = (pts == AV_NOPTS_VALUE)
				? 0.0 : pts * av_q2d(tb);

			if (!have_first_pts) {
				first_pts_seconds = pts_seconds;
				have_first_pts = true;
			}
			double target_seconds = pts_seconds - first_pts_seconds;

			auto target_time = playback_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(target_seconds));
			auto now = std::chrono::steady_clock::now();
			if (target_time > now) {
				std::this_thread::sleep_for(target_time - now);
			}
			if (quit_requested.load()) break;

			sws_scale(sws_ctx, frame->data, frame->linesize, 0, src_h, rgba_frame->data, rgba_frame->linesize);
			{
				std::lock_guard<std::mutex> lock(frame_mutex);
				back_rgba.assign(rgba_buf.begin(), rgba_buf.end());
				std::swap(front_rgba, back_rgba);
				has_frame = true;
				++frame_serial;
				front_frame_pts = target_seconds;
			}
		}
	}

	av_frame_free(&frame);
	av_frame_free(&rgba_frame);
	av_packet_free(&packet);
}

bool gxMovie::draw(gxCanvas* dest, int x, int y, int w, int h) {
	if (!valid.load()) return false;
	bool got_new_frame = false;
	{
		std::lock_guard<std::mutex> lock(frame_mutex);
		if (has_frame && frame_serial != drawn_serial) {
			got_new_frame = true;
		}
	}

	if (!got_new_frame && eof_reached.load()) {
		releaseAfterPlayback();
		return false;
	}
	if (!has_frame) {
		bool p = playing.load();
		if (!p) releaseAfterPlayback();
		return p;
	}

	if (got_new_frame) {
		if (!scratch_back || scratch_w != src_w || scratch_h != src_h) {
			if (scratch_back) gfx->freeCanvas(scratch_back);
			scratch_back = gfx->createCanvas(src_w, src_h, 0);
			scratch_w = src_w;
			scratch_h = src_h;
		}
		if (scratch_back && scratch_back->lock()) {
			std::lock_guard<std::mutex> lock(frame_mutex);
			for (int row = 0; row < src_h; ++row) {
				const unsigned char* src_row = &front_rgba[(size_t)row * rgba_stride];
				for (int col = 0; col < src_w; ++col) {
					const unsigned char* px = src_row + (size_t)col * 4;
					unsigned argb = (0xFFu << 24) | ((unsigned)px[0] << 16) | ((unsigned)px[1] << 8) | (unsigned)px[2];
					scratch_back->setPixelFast(col, row, argb);
				}
			}
			scratch_back->unlock();
			std::swap(scratch_front, scratch_back);
			drawn_serial = frame_serial;
		}
	}
	if (scratch_front) {
		dest->blitstretch(x, y, w, h, scratch_front, 0, 0, src_w, src_h, true);
		RECT r = { x, y, x + w, y + h };
		dest->damage(r);
	}
	bool stillPlaying = playing.load() || !eof_reached.load();
	if (!stillPlaying) releaseAfterPlayback();
	return stillPlaying;
}