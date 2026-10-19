#include "compositor.h"
#include "hl2_bridge.h"
#include "d3d9_render.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <reshade.hpp>

using namespace reshade::api;

namespace
{
	constexpr const wchar_t *kFrameMappingName = L"Local\\MCPassthroughFrame";
	constexpr uint32_t kMagic = 0x5450434D; // "MCPT"
	constexpr int kHeader = 4096;
	constexpr int kSlotDesc = 256;
	constexpr int kSlotDescBytes = 128;
	constexpr const char *kEffectName = "HL2Minecraft.fx";

	std::atomic<bool> g_registered{false};
	std::atomic<bool> g_active{true};

	HANDLE g_frameMapping = nullptr;
	const uint8_t *g_frameView = nullptr;
	int32_t g_slots = 0;
	int64_t g_stride = 0;
	int64_t g_lastPublish = -1;
	DWORD g_nextOpenAttempt = 0;

	struct Layer
	{
		resource tex = {0};
		resource_view srv = {0};
	};
	Layer g_world, g_depth, g_overlay;
	uint32_t g_width = 0, g_height = 0;
	bool g_hasFrame = false;
	float g_mcNear = 0.05f, g_mcFar = 1024.0f;
	int32_t g_mcFlags = 7;
	float g_mcFov = 75.0f;
	float g_mcYaw = 0.0f;
	float g_mcPitch = 0.0f;

	std::unordered_map<std::string, effect_uniform_variable> g_uniforms;

	void refresh_uniforms(effect_runtime *runtime)
	{
		if (!runtime) return;
		g_uniforms.clear();
		runtime->enumerate_uniform_variables(nullptr, [](effect_runtime *rt, effect_uniform_variable var, void *) {
			char name[128] = {0};
			rt->get_uniform_variable_name(var, name);
			if (name[0] != '\0') {
				g_uniforms[name] = var;
			}
		}, nullptr);
	}

	inline effect_uniform_variable find_uniform(effect_runtime *runtime, const char *name)
	{
		if (g_uniforms.empty() && runtime) {
			refresh_uniforms(runtime);
		}
		auto it = g_uniforms.find(name);
		return (it != g_uniforms.end()) ? it->second : effect_uniform_variable{0};
	}

	template <typename T>
	T read_val(const uint8_t *p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	bool open_frame_mapping()
	{
		if (g_frameView != nullptr) return true;
		if (GetTickCount() < g_nextOpenAttempt) return false;
		g_nextOpenAttempt = GetTickCount() + 1000;

		g_frameMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kFrameMappingName);
		if (!g_frameMapping) return false;

		const auto *hdr = static_cast<const uint8_t*>(MapViewOfFile(g_frameMapping, FILE_MAP_READ, 0, 0, kHeader));
		if (!hdr || read_val<uint32_t>(hdr) != kMagic) {
			if (hdr) UnmapViewOfFile(hdr);
			CloseHandle(g_frameMapping);
			g_frameMapping = nullptr;
			return false;
		}

		g_slots = read_val<int32_t>(hdr + 12);
		g_stride = read_val<int64_t>(hdr + 16);
		UnmapViewOfFile(hdr);

		g_frameView = static_cast<const uint8_t*>(MapViewOfFile(g_frameMapping, FILE_MAP_READ, 0, 0, (SIZE_T)(kHeader + g_stride * g_slots)));
		if (!g_frameView) {
			CloseHandle(g_frameMapping);
			g_frameMapping = nullptr;
			return false;
		}

		reshade::log::message(reshade::log::level::info, "[HL2Craft] Connected to Minecraft frame shared memory");
		return true;
	}

	void destroy_layers(device *dev)
	{
		if (!dev) return;
		for (Layer *l : {&g_world, &g_depth, &g_overlay}) {
			if (l->srv.handle != 0) dev->destroy_resource_view(l->srv);
			if (l->tex.handle != 0) dev->destroy_resource(l->tex);
			*l = Layer();
		}
		g_width = g_height = 0;
		g_hasFrame = false;
	}

	bool create_layer(device *dev, Layer &layer, uint32_t w, uint32_t h, format fmt)
	{
		if (!dev->create_resource(
				resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest, resource_flags::dynamic),
				nullptr, resource_usage::shader_resource, &layer.tex))
		{
			if (!dev->create_resource(
					resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource | resource_usage::copy_dest),
					nullptr, resource_usage::shader_resource, &layer.tex))
				return false;
		}
		return dev->create_resource_view(layer.tex, resource_usage::shader_resource, resource_view_desc(fmt), &layer.srv);
	}

	void upload_layer_data(device *dev, Layer &layer, const uint8_t *src, uint32_t w, uint32_t h)
	{
		subresource_data mapped;
		if (dev->map_texture_region(layer.tex, 0, nullptr, map_access::write_discard, &mapped) && mapped.data != nullptr) {
			if (mapped.row_pitch == w * 4) {
				std::memcpy(mapped.data, src, (size_t)w * h * 4);
			} else {
				uint8_t *dst = static_cast<uint8_t*>(mapped.data);
				for (uint32_t y = 0; y < h; ++y) {
					std::memcpy(dst + y * mapped.row_pitch, src + y * (w * 4), w * 4);
				}
			}
			dev->unmap_texture_region(layer.tex, 0);
		} else {
			subresource_data data;
			data.data = const_cast<uint8_t*>(src);
			data.row_pitch = w * 4;
			data.slice_pitch = (uint32_t)(w * h * 4);
			dev->update_texture_region(data, layer.tex, 0);
		}
	}

	void bind_textures(effect_runtime *runtime)
	{
		runtime->update_texture_bindings("MCWORLD", g_world.srv, g_world.srv);
		runtime->update_texture_bindings("MCDEPTH", g_depth.srv, g_depth.srv);
		runtime->update_texture_bindings("MCOVERLAY", g_overlay.srv, g_overlay.srv);
	}

	void upload_frame(effect_runtime *runtime)
	{
		if (!g_frameView) return;

		const int64_t published = read_val<int64_t>(g_frameView + 32);
		if (published == g_lastPublish) return;

		const int32_t slot = read_val<int32_t>(g_frameView + 40);
		if (slot < 0 || slot >= g_slots) return;

		const uint8_t *desc = g_frameView + kSlotDesc + kSlotDescBytes * slot;
		const int64_t seq = read_val<int64_t>(desc);
		if (seq & 1) return; // Currently being written

		const uint32_t w = read_val<uint32_t>(desc + 24);
		const uint32_t h = read_val<uint32_t>(desc + 28);
		if (w == 0 || h == 0) return;

		device *dev = runtime->get_device();
		if (w != g_width || h != g_height) {
			destroy_layers(dev);
			if (!create_layer(dev, g_overlay, w, h, format::r8g8b8a8_unorm)) {
				destroy_layers(dev);
				return;
			}
			g_width = w;
			g_height = h;
			runtime->update_texture_bindings("MCOVERLAY", g_overlay.srv, g_overlay.srv);
			reshade::log::message(reshade::log::level::info, (std::string("[HL2Craft] Allocated overlay layer: ") + std::to_string(w) + "x" + std::to_string(h)).c_str());
		}

		const uint8_t *base = g_frameView + kHeader + g_stride * slot;
		const size_t layerBytes = (size_t)w * h * 4;

		// Overlay (hand + HUD) at base + 2 * layerBytes
		upload_layer_data(dev, g_overlay, base + 2 * layerBytes, w, h);

		if (read_val<int64_t>(desc) != seq) return; // Torn read

		g_lastPublish = published;
		g_hasFrame = true;
	}

	struct DepthSurfaceInfo {
		IDirect3DSurface9 *surf = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t format = 0;
		uint32_t drawCount = 0;
		uint32_t lastFrameDrawCount = 0;
		uint32_t lastClearDrawCount = 0;
		uint32_t clearCount = 0;
		uint32_t clearsBlocked = 0;
		bool sceneCompleted = false;
	};

	static std::unordered_map<uintptr_t, DepthSurfaceInfo> g_depthSurfaces;
	static uintptr_t g_currentDSV = 0;
	static bool g_inEffects = false;
	// Left4Craft: depth clears are only ever held back while a map is being played and Minecraft is linked.
	// HL2Craft blocked them everywhere; in L4D2 that froze the menu (later draws failed the depth test) and the
	// loading screen. Updated once per frame in on_begin_effects.
	static std::atomic<bool> g_allowDepthHold{false};
	// L4D2's view-projection: vertex shader registers c8..c11 (cViewProj of Source's shaders) as last set by a
	// draw of the world. ReShade reports SetVertexShaderConstantF as push_constants with first/count in floats.
	static float g_viewProj[16];
	static bool g_haveViewProj = false;
	// The world's matrix: c8..c11 as they were at the last draw into a full-size depth buffer with a perspective
	// projection. By the end-of-scene clear L4D2 has already drawn screen-space passes that leave c8..c11 at
	// identity, so the plain latest value drew every block off screen.
	static float g_sceneViewProj[16];
	static bool g_haveSceneViewProj = false;

	static bool IsPerspective(const float *m)
	{
		// w row (c11): (0, 0, 0, 1) for screen-space and orthographic passes, a view direction for the world
		return std::fabs(m[12]) + std::fabs(m[13]) + std::fabs(m[14]) > 0.5f;
	}

	static void NoteSceneDraw(const DepthSurfaceInfo &info)
	{
		// the screen-sized depth buffer only: the flashlight's shadow map (1024x1024, square projection) also draws
		// with a perspective c8..c11 and was taken for the camera at first
		if (!g_haveViewProj || info.width < 1024 || info.width == info.height || !IsPerspective(g_viewProj)) return;
		std::memcpy(g_sceneViewProj, g_viewProj, sizeof(g_sceneViewProj));
		if (!g_haveSceneViewProj) {
			g_haveSceneViewProj = true;
			char m[256];
			const float *v = g_sceneViewProj;
			snprintf(m, sizeof(m), "[Left4Craft] world view-projection: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
				v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
			reshade::log::message(reshade::log::level::info, m);
		}
	}

	void on_push_constants(command_list *, shader_stage stages, pipeline_layout, uint32_t param, uint32_t first, uint32_t count, const void *values)
	{
		if ((stages & shader_stage::vertex) != shader_stage::vertex || !values) return;
		constexpr uint32_t kFirst = 8 * 4, kEnd = 12 * 4;
		if (first > kFirst || first + count < kEnd) return;
		std::memcpy(g_viewProj, (const float*)values + (kFirst - first), sizeof(g_viewProj));
		if (!g_haveViewProj) {
			char m[256];
			const float *v = g_viewProj;
			snprintf(m, sizeof(m), "[Left4Craft] view-projection from c8..c11 (param %u first %u count %u): %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
				param, first, count, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15]);
			reshade::log::message(reshade::log::level::info, m);
		}
		g_haveViewProj = true;
	}
	static std::mutex g_depthMutex;

	void on_present(effect_runtime *)
	{
		std::lock_guard<std::mutex> lock(g_depthMutex);
		g_inEffects = false;
		for (auto &pair : g_depthSurfaces) {
			pair.second.lastFrameDrawCount = pair.second.drawCount;
			pair.second.drawCount = 0;
			pair.second.lastClearDrawCount = 0;
			pair.second.clearCount = 0;
			pair.second.clearsBlocked = 0;
			pair.second.sceneCompleted = false;
		}
		g_currentDSV = 0;
	}

	void on_init_swapchain(swapchain *sc, bool)
	{
		if (!sc) return;
		device *dev = sc->get_device();
		if (dev && dev->get_api() == device_api::d3d9) {
			IDirect3DDevice9 *d3ddev = reinterpret_cast<IDirect3DDevice9*>(dev->get_native());
			if (d3ddev) {
				IDirect3DSurface9 *ds = nullptr;
				if (SUCCEEDED(d3ddev->GetDepthStencilSurface(&ds)) && ds) {
					std::lock_guard<std::mutex> lock(g_depthMutex);
					D3DSURFACE_DESC desc{};
					ds->GetDesc(&desc);
					uintptr_t handle = reinterpret_cast<uintptr_t>(ds);
					DepthSurfaceInfo &info = g_depthSurfaces[handle];
					info.surf = ds;
					info.width = desc.Width;
					info.height = desc.Height;
					info.format = (uint32_t)desc.Format;
				}
			}
		}
	}

	bool on_clear_depth_stencil_view(command_list *cmd_list, resource_view dsv, const float *depth, const uint8_t *, uint32_t, const rect *)
	{
		if (g_inEffects) return false;
		if (dsv.handle != 0) {
			std::lock_guard<std::mutex> lock(g_depthMutex);
			auto it = g_depthSurfaces.find(dsv.handle);
			if (it != g_depthSurfaces.end() && !g_allowDepthHold) {
				it->second.clearCount++;
				it->second.lastClearDrawCount = it->second.drawCount;
				return false; // menu, loading screen, no Minecraft: L4D2 clears as usual
			}
			if (it == g_depthSurfaces.end()) {
				IDirect3DSurface9 *surf = reinterpret_cast<IDirect3DSurface9*>(dsv.handle);
				D3DSURFACE_DESC desc{};
				if (SUCCEEDED(surf->GetDesc(&desc))) {
					DepthSurfaceInfo &info = g_depthSurfaces[dsv.handle];
					info.surf = surf;
					info.width = desc.Width;
					info.height = desc.Height;
					info.format = (uint32_t)desc.Format;
					info.clearCount = 1;
					info.lastClearDrawCount = 0;
				}
			} else {
				it->second.clearCount++;
				it->second.lastClearDrawCount = it->second.drawCount;

				if (depth != nullptr && it->second.width >= 640 && it->second.height >= 480) {
					if (it->second.sceneCompleted) {
						return false; // already drawn this frame
					}

					// Never block the initial buffer clear (drawCount == 0 or clearCount <= 1)
					if (it->second.drawCount == 0 || it->second.clearCount <= 1) {
						return false;
					}

					// In Source engine, 3D skybox and water passes happen early in the frame.
					// The post-3D-scene clear occurs at the very end of world rendering before 2D VGUI / HUD.
					bool isEndOfSceneClear = (it->second.lastFrameDrawCount >= 20 &&
					                          it->second.drawCount >= (uint32_t)(it->second.lastFrameDrawCount * 0.80f));

					if (isEndOfSceneClear) {
						// Left4Craft: the world is finished and still bound (colour + depth): draw Minecraft's blocks
						// into it now, with L4D2's own camera, then let the clear go through. No depth hold needed.
						it->second.sceneCompleted = true;
						if (g_haveSceneViewProj && cmd_list) {
							auto *dev = reinterpret_cast<IDirect3DDevice9*>(cmd_list->get_device()->get_native());
							hl2craft::D3D9WorldRender::Get().Render(dev, g_sceneViewProj);
							it->second.clearsBlocked++; // counts frames drawn (diagnostics)
						}
						return false;
					}
				}
			}
		}
		return false;
	}

	void on_bind_render_targets_and_depth_stencil(command_list *, uint32_t, const resource_view *, resource_view dsv)
	{
		std::lock_guard<std::mutex> lock(g_depthMutex);
		if (g_inEffects) return;
		g_currentDSV = dsv.handle;
		if (dsv.handle != 0) {
			auto it = g_depthSurfaces.find(dsv.handle);
			if (it == g_depthSurfaces.end()) {
				IDirect3DSurface9 *surf = reinterpret_cast<IDirect3DSurface9*>(dsv.handle);
				D3DSURFACE_DESC desc{};
				if (SUCCEEDED(surf->GetDesc(&desc))) {
					DepthSurfaceInfo &info = g_depthSurfaces[dsv.handle];
					info.surf = surf;
					info.width = desc.Width;
					info.height = desc.Height;
					info.format = (uint32_t)desc.Format;
				}
			}
		}
	}

	bool on_draw(command_list *, uint32_t, uint32_t instance_count, uint32_t, uint32_t)
	{
		if (g_currentDSV != 0) {
			std::lock_guard<std::mutex> lock(g_depthMutex);
			if (g_inEffects) return false;
			auto it = g_depthSurfaces.find(g_currentDSV);
			if (it != g_depthSurfaces.end()) {
				it->second.drawCount += std::max(1u, instance_count);
				NoteSceneDraw(it->second);
			}
		}
		return false;
	}

	bool on_draw_indexed(command_list *, uint32_t, uint32_t instance_count, uint32_t, int32_t, uint32_t)
	{
		if (g_currentDSV != 0) {
			std::lock_guard<std::mutex> lock(g_depthMutex);
			if (g_inEffects) return false;
			auto it = g_depthSurfaces.find(g_currentDSV);
			if (it != g_depthSurfaces.end()) {
				it->second.drawCount += std::max(1u, instance_count);
				NoteSceneDraw(it->second);
			}
		}
		return false;
	}

	void on_begin_effects(effect_runtime *runtime, command_list *, resource_view rtv, resource_view)
	{
		if (!runtime) return;

		try
		{
			{
				std::lock_guard<std::mutex> lock(g_depthMutex);
				g_inEffects = true;
			}
			hl2craft::HL2Bridge::Get().Tick();
			bool inPlayable = hl2craft::HL2Bridge::Get().IsInPlayableWorld();
			bool paused = hl2craft::HL2Bridge::Get().IsPaused();
			g_allowDepthHold = g_active && inPlayable && !paused && g_hasFrame;

			if (g_active && inPlayable && !paused) {
				IDirect3DSurface9 *bestDS = nullptr;
				uint32_t maxDraws = 0;
				std::string diag = "";
				std::string bestSummary = "none";
				{
					std::lock_guard<std::mutex> lock(g_depthMutex);
					for (auto &pair : g_depthSurfaces) {
						const auto &info = pair.second;
						uint32_t effectiveDraws = std::max(info.drawCount, info.lastFrameDrawCount);
						char item[128];
						snprintf(item, sizeof(item), "[%p: %ux%u fmt=%u draws=%u clr=%u blk=%u] ",
							(void*)info.surf, info.width, info.height, info.format, effectiveDraws, info.clearCount, info.clearsBlocked);
						diag += item;
						if (info.width >= 640 && info.height >= 480) {
							if (effectiveDraws > maxDraws) {
								maxDraws = effectiveDraws;
								bestDS = info.surf;
								char sum[64];
								snprintf(sum, sizeof(sum), "draws=%u clr=%u blk=%u", effectiveDraws, info.clearCount, info.clearsBlocked);
								bestSummary = sum;
							}
						}
					}
				}
				(void)bestDS; // L4D2's own depth buffer is bound when the world is drawn (on_clear_depth_stencil_view)
				hl2craft::D3D9WorldRender::Get().SetDepthCandidatesDiag(diag.c_str());
				hl2craft::D3D9WorldRender::Get().SetDepthSummaryDiag(bestSummary.c_str());
				hl2craft::D3D9WorldRender::Get().Prepare(runtime);
			}

			open_frame_mapping();
			upload_frame(runtime);

			if (const effect_uniform_variable v = find_uniform(runtime, "McActive"); v.handle != 0) {
				runtime->set_uniform_value_bool(v, inPlayable && !paused && g_hasFrame);
			}
			if (const effect_uniform_variable v = find_uniform(runtime, "EnableReprojection"); v.handle != 0) {
				runtime->set_uniform_value_bool(v, false);
			}

			int curX = 0, curY = 0;
			bool curOn = hl2craft::HL2Bridge::Get().GetCursorState(curX, curY);
			if (const effect_uniform_variable v = find_uniform(runtime, "McCursor"); v.handle != 0) {
				const float cur[2] = {(float)curX, (float)curY};
				runtime->set_uniform_value_float(v, cur, 2);
			}
			if (const effect_uniform_variable v = find_uniform(runtime, "McCursorOn"); v.handle != 0) {
				runtime->set_uniform_value_bool(v, curOn);
			}
		}
		catch (...) {}
	}

	void on_reloaded_effects(effect_runtime *runtime)
	{
		refresh_uniforms(runtime);
		if (g_width != 0) {
			bind_textures(runtime);
		}
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		{
			std::lock_guard<std::mutex> lock(g_depthMutex);
			g_depthSurfaces.clear();
			g_currentDSV = 0;
		}
		hl2craft::D3D9WorldRender::Get().SetCapturedDepthStencil(nullptr);
		destroy_layers(runtime->get_device());
	}
}

namespace hl2craft
{
namespace compositor
{
	bool try_register(void *module)
	{
		if (g_registered) return true;
		if (!reshade::register_addon(module)) return false;

		reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
		reshade::register_event<reshade::addon_event::clear_depth_stencil_view>(on_clear_depth_stencil_view);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets_and_depth_stencil);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::push_constants>(on_push_constants);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);

		g_registered = true;
		reshade::log::message(reshade::log::level::info, "[HL2Craft] ReShade addon registered successfully");
		return true;
	}

	void unregister(void *module)
	{
		if (g_registered.exchange(false)) {
			reshade::unregister_addon(module);
		}
	}

	void set_active(bool active)
	{
		g_active = active;
	}
}
}
