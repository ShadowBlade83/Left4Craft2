#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <reshade.hpp>
#include <string>
#include "../../protocol/hl2craft_protocol.h"

namespace hl2craft
{
	struct Vector;

	class D3D9WorldRender
	{
	public:
		static D3D9WorldRender& Get();

		void Init(IDirect3DDevice9 *dev);
		/// Once per frame from reshade_begin_effects: creates the device resources and takes Minecraft's meshes
		/// and atlas updates off the render ring.
		void Prepare(reshade::api::effect_runtime *runtime);
		/// Draws Minecraft's world into L4D2's frame at the end of its 3D scene (the clear before the HUD), with
		/// the render target and depth buffer L4D2 has bound and viewProj = L4D2's own view-projection (vertex
		/// shader registers c8..c11 of its last world draw), so the blocks sit on the map exactly.
		void Render(IDirect3DDevice9 *dev, const float *viewProj);
		void OnDeviceLost();
		void OnDeviceReset(IDirect3DDevice9 *dev);
		void Shutdown();
		void ClearSections();
		void SetCapturedDepthStencil(IDirect3DSurface9 *surf) { m_capturedDS = surf; }
		void SetDepthCandidatesDiag(const char *diag) { m_depthCandidatesDiag = diag ? diag : ""; }
		void SetDepthSummaryDiag(const char *diag) { m_depthSummaryDiag = diag ? diag : ""; }

	private:
		D3D9WorldRender();
		~D3D9WorldRender();

		void DrainRing(IDirect3DDevice9 *dev);

		struct SectionKey {
			int32_t x, y, z;
			bool operator==(const SectionKey &o) const {
				return x == o.x && y == o.y && z == o.z;
			}
		};

		struct SectionKeyHash {
			std::size_t operator()(const SectionKey &k) const {
				return (std::size_t(k.x) * 73856093u) ^ (std::size_t(k.y) * 19349663u) ^ (std::size_t(k.z) * 83492791u);
			}
		};

		struct SectionMesh {
			IDirect3DVertexBuffer9 *vb = nullptr;
			uint32_t opaque = 0;
			uint32_t translucent = 0;
			int32_t sx = 0, sy = 0, sz = 0;
		};

		bool m_initialized = false;
		IDirect3DDevice9 *m_device = nullptr;
		IDirect3DVertexShader9 *m_vs = nullptr;
		IDirect3DPixelShader9 *m_ps = nullptr;
		IDirect3DVertexDeclaration9 *m_vdecl = nullptr;
		IDirect3DTexture9 *m_atlasTex = nullptr;
		IDirect3DTexture9 *m_atlasSysMem = nullptr;
		IDirect3DTexture9 *m_fallbackTex = nullptr;
		uint32_t m_atlasW = 0;
		uint32_t m_atlasH = 0;
		IDirect3DSurface9 *m_capturedDS = nullptr;

		void BuildEntities(const double o[3], const Vector &camPos);
		void BuildOutline(const double o[3], const Vector &camPos);
		void Quad(const float p[4][3], const float uv[4], uint32_t color, uint32_t flags);
		void Box(const float min[3], const float size[3], float yaw, const float side[4], const float top[4], const float bottom[4], uint32_t topTint, uint32_t flags, bool shaded);
		void BuildArrow(float px, float py, float pz, const float d[3], const float *uvSide, const float *uvBack, bool trident);
		void MobPart(float px, float py, float pz, float cx, float cy, float cz, float sx, float sy, float sz, float yaw, const float *mobUv, int tx, int ty, int tw, int th, int td, int sheetW, int sheetH, uint32_t tint, uint32_t fallbackColor);
		void BuildMob(float px, float py, float pz, float yawDeg, float pitchDeg, uint32_t mobType, float height, float width, float scale, const float *mobUv, uint32_t tint);
		bool UploadDynamic(IDirect3DDevice9 *dev);

		std::unordered_map<SectionKey, SectionMesh, SectionKeyHash> m_sections;
		std::vector<proto::RenVertex> m_scratch;

		proto::WorldEntities m_entities{};
		std::vector<proto::RenVertex> m_dynVerts;
		std::vector<proto::RenVertex> m_crackVerts;
		std::vector<proto::RenVertex> m_outlineVerts;
		std::vector<proto::RenVertex> m_uploadVerts;
		IDirect3DVertexBuffer9 *m_dynVb = nullptr;
		uint32_t m_dynVbCapacity = 0;

		std::unordered_map<uint32_t, IDirect3DTexture9*> m_entityTextures;
		IDirect3DVertexBuffer9 *m_sceneVb = nullptr;
		uint32_t m_sceneVbCapacity = 0;
		uint32_t m_sceneVertCount = 0;
		double m_sceneOriginX = 0, m_sceneOriginY = 0, m_sceneOriginZ = 0;
		std::vector<proto::RenBatch> m_sceneBatches;

		std::string m_depthCandidatesDiag;
		std::string m_depthSummaryDiag;
	};
}
