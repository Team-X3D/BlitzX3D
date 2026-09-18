#include "sdl_gpu_text.h"
#include "sdl_gpu_lock.h"
#include "sdl_gpu_pipeline.h"
#include "sdl_gpu_texture.h"

#include "../std.h"
#include "../gxcanvas.h"

#include <cstring>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_properties.h>

#include "shaders/text_shaders.h"

namespace sdlgpu {

namespace {

struct TextVertex {
	float x = 0.0f;
	float y = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
	unsigned color = 0xffffffff;
};

struct PendingQuad {
	float dx = 0.0f;
	float dy = 0.0f;
	float dw = 0.0f;
	float dh = 0.0f;
	float u0 = 0.0f;
	float v0 = 0.0f;
	float u1 = 0.0f;
	float v1 = 0.0f;
	unsigned color = 0xffffffff;
};

struct PendingItem {
	::gxCanvas* target = nullptr;
	::gxCanvas* atlas = nullptr;
	SDL_GPUTexture* tex = nullptr;
	bool smooth = true;
	unsigned canvasW = 1;
	unsigned canvasH = 1;
	PendingQuad quad;
};

struct DrawRange {
	SDL_GPUTexture* tex = nullptr;
	bool smooth = true;
	unsigned first = 0;
	unsigned count = 0;
};

SDL_GPUDevice* g_textDev = nullptr;
std::unordered_map<int, SDL_GPUGraphicsPipeline*> g_textPipes;
SDL_GPUSampler* g_textLinear = nullptr;
SDL_GPUSampler* g_textNearest = nullptr;
SDL_GPUDevice* g_textWhiteDev = nullptr;
SDL_GPUTexture* g_textWhite = nullptr;
SDL_GPUDevice* g_textVbDev = nullptr;
SDL_GPUBuffer* g_textVb = nullptr;
unsigned g_textVbCap = 0;

std::vector<PendingItem> g_pending;
std::vector<TextVertex> g_staged;
std::vector<DrawRange> g_ranges;

::gxCanvas* g_activeTarget = nullptr;

bool g_backClearSet = false;
unsigned g_backClearColor = 0;

unsigned PackTextColor(unsigned argb) {
	unsigned a = (argb >> 24) & 0xff;
	unsigned r = (argb >> 16) & 0xff;
	unsigned g = (argb >> 8) & 0xff;
	unsigned b = argb & 0xff;
	return r | (g << 8) | (b << 16) | (a << 24);
}

void TeardownWhite() {
	GpuLock lock;
	if (g_textWhite && g_textWhiteDev) SDL_ReleaseGPUTexture(g_textWhiteDev, g_textWhite);
	g_textWhite = nullptr;
	g_textWhiteDev = nullptr;
}

void TeardownPipe() {
	GpuLock lock;
	if (g_textDev) {
		for (auto& kv : g_textPipes) if (kv.second) SDL_ReleaseGPUGraphicsPipeline(g_textDev, kv.second);
		if (g_textLinear) SDL_ReleaseGPUSampler(g_textDev, g_textLinear);
		if (g_textNearest) SDL_ReleaseGPUSampler(g_textDev, g_textNearest);
	}
	g_textPipes.clear();
	g_textLinear = nullptr;
	g_textNearest = nullptr;
	g_textDev = nullptr;
}

void TeardownVb() {
	GpuLock lock;
	if (g_textVb && g_textVbDev) SDL_ReleaseGPUBuffer(g_textVbDev, g_textVb);
	g_textVb = nullptr;
	g_textVbCap = 0;
	g_textVbDev = nullptr;
}

SDL_GPUTexture* EnsureTextWhite(SDL_GPUDevice* dev) {
	GpuLock lock;
	if (g_textWhite && g_textWhiteDev == dev) return g_textWhite;
	TeardownWhite();
	SDL_GPUTextureCreateInfo info{};
	info.type = SDL_GPU_TEXTURETYPE_2D;
	info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
	info.width = 1;
	info.height = 1;
	info.layer_count_or_depth = 1;
	info.num_levels = 1;
	info.sample_count = SDL_GPU_SAMPLECOUNT_1;
	SDL_GPUTexture* tex = SDL_CreateGPUTexture(dev, &info);
	if (!tex) return nullptr;
	unsigned char white[4] = { 255, 255, 255, 255 };
	SDL_GPUTransferBuffer* buf = AcquireUploadTransferBuffer(dev, 4);
	if (!buf) { SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	void* dst = SDL_MapGPUTransferBuffer(dev, buf, true);
	if (!dst) { ReleaseUploadTransferBuffer(dev, buf); SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	memcpy(dst, white, 4);
	SDL_UnmapGPUTransferBuffer(dev, buf);
	SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmds) { ReleaseUploadTransferBuffer(dev, buf); SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmds);
	SDL_GPUTextureTransferInfo src{};
	src.transfer_buffer = buf;
	src.pixels_per_row = 1;
	src.rows_per_layer = 1;
	SDL_GPUTextureRegion reg{};
	reg.texture = tex;
	reg.w = 1;
	reg.h = 1;
	reg.d = 1;
	SDL_UploadToGPUTexture(copy, &src, &reg, true);
	SDL_EndGPUCopyPass(copy);
	bool ok = SDL_SubmitGPUCommandBuffer(cmds);
	ReleaseUploadTransferBuffer(dev, buf);
	if (!ok) { SDL_ReleaseGPUTexture(dev, tex); return nullptr; }
	g_textWhiteDev = dev;
	g_textWhite = tex;
	return g_textWhite;
}

SDL_GPUShader* LoadTextShader(SDL_GPUDevice* dev, SDL_GPUShaderFormat fmt, SDL_GPUShaderStage stage, const char* entry, const uint8_t* code, size_t size, unsigned samplers) {
	SDL_GPUShaderCreateInfo info{};
	info.code = code;
	info.code_size = size;
	info.entrypoint = entry;
	info.format = fmt;
	info.stage = stage;
	info.num_samplers = samplers;
	info.num_uniform_buffers = 0;
	return SDL_CreateGPUShader(dev, &info);
}

SDL_GPUGraphicsPipeline* EnsureTextPipeForFormat(SDL_GPUDevice* dev, SDL_GPUTextureFormat fmt) {
	GpuLock lock;
	if (!dev || fmt == SDL_GPU_TEXTUREFORMAT_INVALID) return nullptr;
	if (g_textDev != dev) {
		TeardownPipe();
	}
	auto found = g_textPipes.find((int)fmt);
	if (found != g_textPipes.end() && found->second) return found->second;
	SDL_GPUShaderFormat sup = SDL_GetGPUShaderFormats(dev);
	const uint8_t* vsCode = nullptr;
	const uint8_t* psCode = nullptr;
	size_t vsSize = 0;
	size_t psSize = 0;
	SDL_GPUShaderFormat use = SDL_GPU_SHADERFORMAT_INVALID;
	if (sup & SDL_GPU_SHADERFORMAT_SPIRV) {
		use = SDL_GPU_SHADERFORMAT_SPIRV;
		vsCode = kTextVS_SPIRV;
		vsSize = kTextVS_SPIRV_size;
		psCode = kTextPS_SPIRV;
		psSize = kTextPS_SPIRV_size;
	} else if (sup & SDL_GPU_SHADERFORMAT_DXIL) {
		use = SDL_GPU_SHADERFORMAT_DXIL;
		vsCode = kTextVS_DXIL;
		vsSize = kTextVS_DXIL_size;
		psCode = kTextPS_DXIL;
		psSize = kTextPS_DXIL_size;
	}
	if (use == SDL_GPU_SHADERFORMAT_INVALID) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "No supported text shader format: %u", (unsigned)sup);
		return nullptr;
	}
	SDL_GPUShader* vs = LoadTextShader(dev, use, SDL_GPU_SHADERSTAGE_VERTEX, "VSMain", vsCode, vsSize, 0);
	if (!vs) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Text VS failed: %s", SDL_GetError());
		return nullptr;
	}
	SDL_GPUShader* ps = LoadTextShader(dev, use, SDL_GPU_SHADERSTAGE_FRAGMENT, "PSMain", psCode, psSize, 1);
	if (!ps) {
		SDL_ReleaseGPUShader(dev, vs);
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Text PS failed: %s", SDL_GetError());
		return nullptr;
	}
	SDL_GPUVertexBufferDescription vb{};
	vb.slot = 0;
	vb.pitch = sizeof(TextVertex);
	vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	SDL_GPUVertexAttribute attrs[3]{};
	attrs[0].location = 0;
	attrs[0].buffer_slot = 0;
	attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
	attrs[0].offset = 0;
	attrs[1].location = 1;
	attrs[1].buffer_slot = 0;
	attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
	attrs[1].offset = 8;
	attrs[2].location = 2;
	attrs[2].buffer_slot = 0;
	attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
	attrs[2].offset = 16;
	SDL_GPUVertexInputState vin{};
	vin.vertex_buffer_descriptions = &vb;
	vin.num_vertex_buffers = 1;
	vin.vertex_attributes = attrs;
	vin.num_vertex_attributes = 3;
	SDL_GPUColorTargetDescription tgt{};
	tgt.format = fmt;
	tgt.blend_state.enable_blend = true;
	tgt.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	tgt.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	tgt.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
	tgt.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	tgt.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	tgt.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
	SDL_GPUGraphicsPipelineCreateInfo info{};
	info.vertex_shader = vs;
	info.fragment_shader = ps;
	info.vertex_input_state = vin;
	info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
	info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
	info.rasterizer_state.enable_depth_clip = true;
	info.rasterizer_state.enable_depth_bias = false;
	info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
	info.multisample_state.sample_mask = 0;
	info.depth_stencil_state.enable_depth_test = false;
	info.depth_stencil_state.enable_depth_write = false;
	info.depth_stencil_state.enable_stencil_test = false;
	info.depth_stencil_state.back_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
	info.depth_stencil_state.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
	info.target_info.num_color_targets = 1;
	info.target_info.color_target_descriptions = &tgt;
	info.target_info.has_depth_stencil_target = false;
	SDL_PropertiesID textProps = SDL_CreateProperties();
	if (textProps) SDL_SetStringProperty(textProps, SDL_PROP_GPU_GRAPHICSPIPELINE_CREATE_NAME_STRING, "b3d_text");
	info.props = textProps;
	SDL_GPUGraphicsPipeline* pipe = SDL_CreateGPUGraphicsPipeline(dev, &info);
	if (textProps) SDL_DestroyProperties(textProps);
	info.props = 0;
	SDL_ReleaseGPUShader(dev, vs);
	SDL_ReleaseGPUShader(dev, ps);
	if (!pipe) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Text pipeline failed: %s", SDL_GetError());
		return nullptr;
	}
	if (!g_textLinear) {
		SDL_GPUSamplerCreateInfo lin{};
		lin.min_filter = SDL_GPU_FILTER_LINEAR;
		lin.mag_filter = SDL_GPU_FILTER_LINEAR;
		lin.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		lin.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		g_textLinear = SDL_CreateGPUSampler(dev, &lin);
	}
	if (!g_textNearest) {
		SDL_GPUSamplerCreateInfo pint{};
		pint.min_filter = SDL_GPU_FILTER_NEAREST;
		pint.mag_filter = SDL_GPU_FILTER_NEAREST;
		pint.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		pint.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		g_textNearest = SDL_CreateGPUSampler(dev, &pint);
	}
	if (!g_textLinear || !g_textNearest) {
		SDL_ReleaseGPUGraphicsPipeline(dev, pipe);
		TeardownPipe();
		return nullptr;
	}
	g_textDev = dev;
	g_textPipes[(int)fmt] = pipe;
	return pipe;
}

void EmitQuad(std::vector<TextVertex>& out, unsigned canvasW, unsigned canvasH, const PendingQuad& q) {
	float sx = 2.0f / (float)canvasW;
	float sy = 2.0f / (float)canvasH;
	float x0 = q.dx * sx - 1.0f;
	float x1 = (q.dx + q.dw) * sx - 1.0f;
	float y0 = 1.0f - q.dy * sy;
	float y1 = 1.0f - (q.dy + q.dh) * sy;
	TextVertex v0{ x0, y0, q.u0, q.v0, q.color };
	TextVertex v1{ x1, y0, q.u1, q.v0, q.color };
	TextVertex v2{ x1, y1, q.u1, q.v1, q.color };
	TextVertex v3{ x0, y0, q.u0, q.v0, q.color };
	TextVertex v4{ x1, y1, q.u1, q.v1, q.color };
	TextVertex v5{ x0, y1, q.u0, q.v1, q.color };
	out.push_back(v0);
	out.push_back(v1);
	out.push_back(v2);
	out.push_back(v3);
	out.push_back(v4);
	out.push_back(v5);
}

bool EnsureTextVb(SDL_GPUDevice* dev, unsigned needVerts) {
	GpuLock lock;
	if (g_textVb && g_textVbDev == dev && g_textVbCap >= needVerts) return true;
	TeardownVb();
	unsigned cap = needVerts < 1024 ? 1024 : needVerts;
	cap = (cap + 1023) & ~1023u;
	SDL_GPUBufferCreateInfo bi{};
	bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	bi.size = cap * (unsigned)sizeof(TextVertex);
	g_textVb = SDL_CreateGPUBuffer(dev, &bi);
	if (!g_textVb) return false;
	g_textVbDev = dev;
	g_textVbCap = cap;
	return true;
}

}

void InvalidatePendingTexture(SDL_GPUTexture* tex) {
	GpuLock lock;
	if (!tex) return;
	for (auto it = g_pending.begin(); it != g_pending.end();) {
		if (it->tex == tex) it = g_pending.erase(it);
		else ++it;
	}
}

bool QueueTextQuads(SDL_GPUDevice* dev, ::gxCanvas* target, ::gxCanvas* atlas, bool smooth, unsigned canvasW, unsigned canvasH, const TextQuad* quads, unsigned count) {
	GpuLock lock;
	if (!dev || !atlas || !quads || !count || !canvasW || !canvasH) return false;
	SDL_GPUTexture* tex = GetCanvasTexture(dev, atlas);
	if (!tex) return false;
	unsigned atlasW = (unsigned)atlas->getWidth();
	unsigned atlasH = (unsigned)atlas->getHeight();
	if (!atlasW || !atlasH) return false;
	for (unsigned i = 0; i < count; ++i) {
		const TextQuad& q = quads[i];
		if (q.destW <= 0.0f || q.destH <= 0.0f || q.srcW <= 0.0f || q.srcH <= 0.0f) continue;
		PendingItem item{};
		item.target = target;
		item.atlas = atlas;
		item.tex = tex;
		item.smooth = smooth;
		item.canvasW = canvasW;
		item.canvasH = canvasH;
		item.quad.dx = q.destX;
		item.quad.dy = q.destY;
		item.quad.dw = q.destW;
		item.quad.dh = q.destH;
		item.quad.u0 = q.srcX / (float)atlasW;
		item.quad.v0 = q.srcY / (float)atlasH;
		item.quad.u1 = (q.srcX + q.srcW) / (float)atlasW;
		item.quad.v1 = (q.srcY + q.srcH) / (float)atlasH;
		item.quad.color = PackTextColor(q.color);
		g_pending.push_back(item);
	}
	return true;
}

bool QueueTextSolid(SDL_GPUDevice* dev, ::gxCanvas* target, unsigned canvasW, unsigned canvasH, float dx, float dy, float dw, float dh, unsigned color) {
	GpuLock lock;
	if (!dev || !canvasW || !canvasH || dw <= 0.0f || dh <= 0.0f) return false;
	SDL_GPUTexture* tex = EnsureTextWhite(dev);
	if (!tex) return false;
	PendingItem item{};
	item.target = target;
	item.tex = tex;
	item.smooth = true;
	item.canvasW = canvasW;
	item.canvasH = canvasH;
	item.quad.dx = dx;
	item.quad.dy = dy;
	item.quad.dw = dw;
	item.quad.dh = dh;
	item.quad.u0 = 0.0f;
	item.quad.v0 = 0.0f;
	item.quad.u1 = 1.0f;
	item.quad.v1 = 1.0f;
	item.quad.color = PackTextColor(color);
	g_pending.push_back(item);
	return true;
}

bool QueueRectFilled(SDL_GPUDevice* dev, ::gxCanvas* target, unsigned canvasW, unsigned canvasH, float x, float y, float w, float h, unsigned color) {
	if (w <= 0.0f || h <= 0.0f) return true;
	return QueueTextSolid(dev, target, canvasW, canvasH, x, y, w, h, color);
}

bool QueueRectOutline(SDL_GPUDevice* dev, ::gxCanvas* target, unsigned canvasW, unsigned canvasH, float x, float y, float w, float h, unsigned color) {
	GpuLock lock;
	if (w <= 0.0f || h <= 0.0f) return true;
	if (!QueueTextSolid(dev, target, canvasW, canvasH, x, y, w, 1.0f, color)) return false;
	if (h > 1.0f) {
		if (!QueueTextSolid(dev, target, canvasW, canvasH, x, y + h - 1.0f, w, 1.0f, color)) return false;
		if (h > 2.0f) {
			if (!QueueTextSolid(dev, target, canvasW, canvasH, x, y + 1.0f, 1.0f, h - 2.0f, color)) return false;
			if (w > 1.0f && !QueueTextSolid(dev, target, canvasW, canvasH, x + w - 1.0f, y + 1.0f, 1.0f, h - 2.0f, color)) return false;
		}
	}
	return true;
}

bool QueueSpriteQuad(SDL_GPUDevice* dev, ::gxCanvas* target, SDL_GPUTexture* tex, bool smooth, unsigned canvasW, unsigned canvasH, unsigned texW, unsigned texH, const TextQuad* quad) {
	GpuLock lock;
	if (!dev || !tex || !quad || !canvasW || !canvasH || !texW || !texH) return false;
	if (quad->destW <= 0.0f || quad->destH <= 0.0f || quad->srcW <= 0.0f || quad->srcH <= 0.0f) return true;
	PendingItem item{};
	item.target = target;
	item.tex = tex;
	item.smooth = smooth;
	item.canvasW = canvasW;
	item.canvasH = canvasH;
	item.quad.dx = quad->destX;
	item.quad.dy = quad->destY;
	item.quad.dw = quad->destW;
	item.quad.dh = quad->destH;
	item.quad.u0 = quad->srcX / (float)texW;
	item.quad.v0 = quad->srcY / (float)texH;
	item.quad.u1 = (quad->srcX + quad->srcW) / (float)texW;
	item.quad.v1 = (quad->srcY + quad->srcH) / (float)texH;
	item.quad.color = PackTextColor(quad->color);
	g_pending.push_back(item);
	return true;
}

bool HasPendingText() {
	GpuLock lock;
	return !g_pending.empty();
}

bool PreparePendingText(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmds) {
	GpuLock lock;
	g_staged.clear();
	g_ranges.clear();
	if (!dev || !cmds) return false;
	unsigned total = (unsigned)g_pending.size() * 6;
	if (!total) return false;
	if (!EnsureTextVb(dev, total)) return false;
	g_staged.reserve(total);
	for (auto& item : g_pending) {
		if (item.target) continue;
		if (g_ranges.empty() || g_ranges.back().tex != item.tex || g_ranges.back().smooth != item.smooth) {
			DrawRange r{};
			r.tex = item.tex;
			r.smooth = item.smooth;
			r.first = (unsigned)g_staged.size();
			g_ranges.push_back(r);
		}
		EmitQuad(g_staged, item.canvasW, item.canvasH, item.quad);
		g_ranges.back().count += 6;
	}
	if (g_staged.empty() || g_ranges.empty()) return false;
	unsigned bytes = (unsigned)g_staged.size() * (unsigned)sizeof(TextVertex);
	SDL_GPUTransferBuffer* tb = AcquireUploadTransferBuffer(dev, bytes);
	if (!tb) return false;
	void* dst = SDL_MapGPUTransferBuffer(dev, tb, true);
	if (!dst) {
		ReleaseUploadTransferBuffer(dev, tb);
		return false;
	}
	memcpy(dst, g_staged.data(), bytes);
	SDL_UnmapGPUTransferBuffer(dev, tb);
	SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmds);
	SDL_GPUTransferBufferLocation src{};
	src.transfer_buffer = tb;
	SDL_GPUBufferRegion reg{};
	reg.buffer = g_textVb;
	reg.offset = 0;
	reg.size = bytes;
	SDL_UploadToGPUBuffer(cp, &src, &reg, true);
	SDL_EndGPUCopyPass(cp);
	ReleaseUploadTransferBuffer(dev, tb);
	return true;
}

static void DrawRanges(SDL_GPUDevice* dev, SDL_GPURenderPass* pass, SDL_GPUGraphicsPipeline* pipe, const std::vector<DrawRange>& ranges) {
	if (!dev || !pass || !pipe || !g_textVb || ranges.empty()) return;
	InvalidateMeshState();
	SDL_BindGPUGraphicsPipeline(pass, pipe);
	SDL_GPUBufferBinding vb{};
	vb.buffer = g_textVb;
	vb.offset = 0;
	SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
	for (auto& r : ranges) {
		if (!r.tex || !r.count) continue;
		SDL_GPUTextureSamplerBinding b{};
		b.texture = r.tex;
		b.sampler = r.smooth ? g_textLinear : g_textNearest;
		if (!b.sampler) continue;
		SDL_BindGPUFragmentSamplers(pass, 0, &b, 1);
		SDL_DrawGPUPrimitives(pass, r.count, 1, r.first, 0);
	}
}

void DrawPendingText(SDL_GPUDevice* dev, SDL_Window* win, SDL_GPURenderPass* pass) {
	GpuLock lock;
	if (!dev || !pass || g_ranges.empty()) return;
	SDL_GPUTextureFormat fmt = win ? SDL_GetGPUSwapchainTextureFormat(dev, win) : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	SDL_GPUGraphicsPipeline* pipe = EnsureTextPipeForFormat(dev, fmt);
	if (!pipe) return;
	DrawRanges(dev, pass, pipe, g_ranges);
}

static bool TakePendingForTarget(::gxCanvas* target, std::vector<DrawRange>& ranges) {
	std::vector<PendingItem> keep;
	keep.reserve(g_pending.size());
	for (auto& item : g_pending) {
		if (item.target != target) { keep.push_back(item); continue; }
		if (ranges.empty() || ranges.back().tex != item.tex || ranges.back().smooth != item.smooth) {
			DrawRange r{};
			r.tex = item.tex;
			r.smooth = item.smooth;
			r.first = (unsigned)g_staged.size();
			ranges.push_back(r);
		}
		EmitQuad(g_staged, item.canvasW, item.canvasH, item.quad);
		ranges.back().count += 6;
	}
	g_pending.swap(keep);
	return !ranges.empty();
}

bool FlushPendingTextToCanvas(SDL_GPUDevice* dev, ::gxCanvas* canvas) {
	GpuLock lock;
	if (!dev || !canvas) return false;
	bool has = false;
	for (auto& item : g_pending) { if (item.target == canvas) { has = true; break; } }
	if (!has) return true;
	SDL_GPUTexture* rt = EnsureCanvasRenderTarget(dev, canvas);
	if (!rt) return false;
	std::vector<DrawRange> ranges;
	g_staged.clear();
	if (!TakePendingForTarget(canvas, ranges)) return true;
	if (!EnsureTextVb(dev, (unsigned)g_staged.size())) return false;
	SDL_GPUGraphicsPipeline* pipe = EnsureTextPipeForFormat(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
	if (!pipe) return false;

	unsigned bytes = (unsigned)g_staged.size() * (unsigned)sizeof(TextVertex);
	SDL_GPUCommandBuffer* cmds = SDL_AcquireGPUCommandBuffer(dev);
	if (!cmds) return false;
	SDL_GPUTransferBuffer* tb = AcquireUploadTransferBuffer(dev, bytes);
	if (!tb) { SDL_CancelGPUCommandBuffer(cmds); return false; }
	void* dst = SDL_MapGPUTransferBuffer(dev, tb, true);
	if (!dst) { ReleaseUploadTransferBuffer(dev, tb); SDL_CancelGPUCommandBuffer(cmds); return false; }
	memcpy(dst, g_staged.data(), bytes);
	SDL_UnmapGPUTransferBuffer(dev, tb);
	SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmds);
	SDL_GPUTransferBufferLocation src{};
	src.transfer_buffer = tb;
	SDL_GPUBufferRegion reg{};
	reg.buffer = g_textVb;
	reg.offset = 0;
	reg.size = bytes;
	SDL_UploadToGPUBuffer(cp, &src, &reg, true);
	SDL_EndGPUCopyPass(cp);
	ReleaseUploadTransferBuffer(dev, tb);

	SDL_GPUColorTargetInfo ci{};
	ci.texture = rt;
	ci.load_op = SDL_GPU_LOADOP_LOAD;
	ci.store_op = SDL_GPU_STOREOP_STORE;
	ci.cycle = false;
	SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmds, &ci, 1, nullptr);
	if (!pass) { SDL_CancelGPUCommandBuffer(cmds); return false; }
	SDL_GPUViewport vp{};
	vp.x = 0; vp.y = 0;
	vp.w = (float)canvas->getWidth();
	vp.h = (float)canvas->getHeight();
	vp.min_depth = 0.0f; vp.max_depth = 1.0f;
	SDL_SetGPUViewport(pass, &vp);
	SDL_Rect sc{ 0, 0, canvas->getWidth(), canvas->getHeight() };
	SDL_SetGPUScissor(pass, &sc);
	DrawRanges(dev, pass, pipe, ranges);
	SDL_EndGPURenderPass(pass);
	return SDL_SubmitGPUCommandBuffer(cmds);
}

bool FlushPendingTextTargets(SDL_GPUDevice* dev) {
	GpuLock lock;
	if (!dev) return false;
	std::vector<::gxCanvas*> targets;
	for (auto& item : g_pending) {
		if (!item.target) continue;
		bool seen = false;
		for (auto* t : targets) if (t == item.target) { seen = true; break; }
		if (!seen) targets.push_back(item.target);
	}
	bool ok = true;
	for (auto* t : targets) {
		if (!FlushPendingTextToCanvas(dev, t)) ok = false;
	}
	return ok;
}

void SetActiveCanvasTarget(SDL_GPUDevice* dev, ::gxCanvas* canvas) {
	GpuLock lock;
	if (g_activeTarget == canvas) return;
	::gxCanvas* prev = g_activeTarget;
	g_activeTarget = canvas;
	if (prev && dev) FlushPendingTextToCanvas(dev, prev);
}

bool IsActiveCanvasTarget(::gxCanvas* canvas) {
	GpuLock lock;
	return canvas && g_activeTarget == canvas;
}

void QueueBackbufferClear(SDL_GPUDevice* dev, unsigned argb) {
	GpuLock lock;
	if (!dev) return;
	g_backClearSet = true;
	g_backClearColor = argb;
	std::vector<PendingItem> keep;
	keep.reserve(g_pending.size());
	for (auto& item : g_pending) {
		if (!item.target) continue;
		keep.push_back(item);
	}
	g_pending.swap(keep);
}

bool TakeBackbufferClear(unsigned* argb) {
	GpuLock lock;
	bool set = g_backClearSet;
	if (argb) *argb = g_backClearColor;
	g_backClearSet = false;
	return set;
}

void ClearPendingText() {
	GpuLock lock;
	g_pending.clear();
	g_staged.clear();
	g_ranges.clear();
	g_backClearSet = false;
}

void InvalidateTextAtlas(::gxCanvas* atlas) {
	GpuLock lock;
	if (!atlas) return;
	if (g_activeTarget == atlas) g_activeTarget = nullptr;
	for (auto it = g_pending.begin(); it != g_pending.end();) {
		if (it->atlas == atlas)
			it = g_pending.erase(it);
		else
			++it;
	}
}

void TeardownText() {
	GpuLock lock;
	g_activeTarget = nullptr;
	ClearPendingText();
	TeardownPipe();
	TeardownVb();
	TeardownWhite();
}

}
