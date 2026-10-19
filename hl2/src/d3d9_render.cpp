#include "d3d9_render.h"
#include "hl2_bridge.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <array>

namespace hl2craft
{
	namespace
	{
		typedef HRESULT (WINAPI *pD3DCompileFn)(
			LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
			LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**
		);

		pD3DCompileFn g_compileFn = nullptr;
		HMODULE g_d3dCompilerMod = nullptr;

		bool EnsureCompiler()
		{
			if (g_compileFn) return true;
			g_d3dCompilerMod = LoadLibraryA("d3dcompiler_47.dll");
			if (!g_d3dCompilerMod) {
				g_d3dCompilerMod = LoadLibraryA("d3dcompiler_43.dll");
			}
			if (!g_d3dCompilerMod) return false;
			g_compileFn = (pD3DCompileFn)GetProcAddress(g_d3dCompilerMod, "D3DCompile");
			return g_compileFn != nullptr;
		}

		constexpr char kVertexShaderSrc[] = R"(
			float4 WorldToScreen_R0 : register(c1);
			float4 WorldToScreen_R1 : register(c2);
			float4 WorldToScreen_R2 : register(c3);
			float4 WorldToScreen_R3 : register(c4);
			float4 SectionOffset : register(c0);

			struct VS_IN
			{
				float3 pos   : POSITION;
				float2 uv    : TEXCOORD0;
				float4 color : COLOR0;
				float4 light : TEXCOORD1;
				float4 flags : TEXCOORD2;
			};

			struct VS_OUT
			{
				float4 pos   : POSITION;
				float2 uv    : TEXCOORD0;
				float4 color : COLOR0;
				float4 flags : TEXCOORD1;
			};

			VS_OUT VS_Main(VS_IN i)
			{
				VS_OUT o;
				// Minecraft -> Source Engine units:
				// 1 block = 39.37007874 Source units
				// Source X = MC X
				// Source Y = -MC Z
				// Source Z = MC Y
				float3 worldPos = SectionOffset.xyz + float3(
					i.pos.x * 39.37007874f,
					-i.pos.z * 39.37007874f,
					i.pos.y * 39.37007874f
				);
				float4 p4 = float4(worldPos, 1.0f);
				o.pos.x = dot(WorldToScreen_R0, p4);
				o.pos.y = dot(WorldToScreen_R1, p4);
				o.pos.z = dot(WorldToScreen_R2, p4);
				o.pos.w = dot(WorldToScreen_R3, p4);
				o.uv = i.uv;

				float blockL = clamp(i.light.x / 15.0f, 0.0f, 1.0f);
				float skyL   = clamp(i.light.y / 15.0f, 0.0f, 1.0f);
				float l = max(0.18f, max(blockL, skyL * 0.90f));

				o.color.rgb = i.color.rgb * l;
				o.color.a = i.color.a;
				o.flags = i.flags;
				return o;
			}
		)";

		constexpr char kPixelShaderSrc[] = R"(
			sampler2D AtlasSampler : register(s0);
			float4 AlphaParams : register(c0);

			struct PS_IN
			{
				float4 pos   : POSITION;
				float2 uv    : TEXCOORD0;
				float4 color : COLOR0;
				float4 flags : TEXCOORD1;
			};

			float4 PS_Main(PS_IN i) : COLOR
			{
				float4 tex = lerp(tex2D(AtlasSampler, i.uv), float4(1.0f, 1.0f, 1.0f, 1.0f), AlphaParams.y);
				if (i.flags.y > 0.002f) {
					tex.rgb = lerp(tex.rgb, float3(1.0f, 1.0f, 1.0f), 0.85f);
				}
				float4 col = tex * i.color;
				clip(col.a - AlphaParams.x);
				return col;
			}
		)";

		constexpr std::uint32_t kFlagCutout = 1;
		constexpr std::uint32_t kFlagTranslucent = 2;
		constexpr std::uint32_t kFlagUntextured = 4;
		constexpr std::uint32_t kFlagNoMip = 8;
		constexpr std::uint32_t kFlagNormalFromFaces = 7u << 4;
		constexpr std::uint32_t kFullSkyLight = 15u << 8;
		constexpr float kPi = 3.14159265f;
		constexpr float kArrowScale = 0.55f;

		struct Matrix4x4 {
			float m[4][4];

			static Matrix4x4 Multiply(const Matrix4x4 &a, const Matrix4x4 &b) {
				Matrix4x4 r{};
				for (int i = 0; i < 4; ++i) {
					for (int j = 0; j < 4; ++j) {
						r.m[i][j] = a.m[i][0] * b.m[0][j] +
						            a.m[i][1] * b.m[1][j] +
						            a.m[i][2] * b.m[2][j] +
						            a.m[i][3] * b.m[3][j];
					}
				}
				return r;
			}
		};
	}

	D3D9WorldRender& D3D9WorldRender::Get()
	{
		static D3D9WorldRender instance;
		return instance;
	}

	D3D9WorldRender::D3D9WorldRender() = default;

	D3D9WorldRender::~D3D9WorldRender()
	{
		Shutdown();
	}

	void D3D9WorldRender::ClearSections()
	{
		for (auto &pair : m_sections) {
			if (pair.second.vb) {
				pair.second.vb->Release();
				pair.second.vb = nullptr;
			}
		}
		m_sections.clear();
		m_sceneBatches.clear();
		m_sceneVertCount = 0;
	}

	void D3D9WorldRender::Shutdown()
	{
		ClearSections();
		for (auto &pair : m_entityTextures) {
			if (pair.second) {
				pair.second->Release();
			}
		}
		m_entityTextures.clear();
		if (m_atlasTex) { m_atlasTex->Release(); m_atlasTex = nullptr; }
		if (m_atlasSysMem) { m_atlasSysMem->Release(); m_atlasSysMem = nullptr; }
		if (m_fallbackTex) { m_fallbackTex->Release(); m_fallbackTex = nullptr; }
		if (m_dynVb) { m_dynVb->Release(); m_dynVb = nullptr; m_dynVbCapacity = 0; }
		if (m_sceneVb) { m_sceneVb->Release(); m_sceneVb = nullptr; m_sceneVbCapacity = 0; }
		if (m_vdecl) { m_vdecl->Release(); m_vdecl = nullptr; }
		if (m_vs) { m_vs->Release(); m_vs = nullptr; }
		if (m_ps) { m_ps->Release(); m_ps = nullptr; }
		m_device = nullptr;
		m_capturedDS = nullptr;
		m_initialized = false;
	}

	void D3D9WorldRender::OnDeviceLost()
	{
		m_capturedDS = nullptr;
		if (m_dynVb) { m_dynVb->Release(); m_dynVb = nullptr; m_dynVbCapacity = 0; }
		if (m_sceneVb) { m_sceneVb->Release(); m_sceneVb = nullptr; m_sceneVbCapacity = 0; }
	}

	void D3D9WorldRender::OnDeviceReset(IDirect3DDevice9 *dev)
	{
		m_device = dev;
	}

	void D3D9WorldRender::Init(IDirect3DDevice9 *dev)
	{
		if (m_initialized && m_device == dev) return;
		if (m_device != dev) {
			Shutdown();
		}
		m_device = dev;
		if (!dev) return;

		if (!EnsureCompiler()) {
			reshade::log::message(reshade::log::level::error, "[HL2Craft] D3D9WorldRender: d3dcompiler dll not found");
			return;
		}

		ID3DBlob *vsBlob = nullptr, *errBlob = nullptr;
		HRESULT hr = g_compileFn(kVertexShaderSrc, strlen(kVertexShaderSrc), nullptr, nullptr, nullptr, "VS_Main", "vs_3_0", 1 << 18 /* D3DCOMPILE_PACK_MATRIX_ROW_MAJOR */, 0, &vsBlob, &errBlob);
		if (FAILED(hr)) {
			std::string err = errBlob ? (const char*)errBlob->GetBufferPointer() : "unknown";
			reshade::log::message(reshade::log::level::error, ("[HL2Craft] D3D9WorldRender VS compile error: " + err).c_str());
			if (errBlob) errBlob->Release();
			return;
		}
		hr = dev->CreateVertexShader((const DWORD*)vsBlob->GetBufferPointer(), &m_vs);
		vsBlob->Release();
		if (FAILED(hr)) {
			reshade::log::message(reshade::log::level::error, "[HL2Craft] D3D9WorldRender: CreateVertexShader failed");
			return;
		}

		ID3DBlob *psBlob = nullptr;
		hr = g_compileFn(kPixelShaderSrc, strlen(kPixelShaderSrc), nullptr, nullptr, nullptr, "PS_Main", "ps_3_0", 0, 0, &psBlob, &errBlob);
		if (FAILED(hr)) {
			std::string err = errBlob ? (const char*)errBlob->GetBufferPointer() : "unknown";
			reshade::log::message(reshade::log::level::error, ("[HL2Craft] D3D9WorldRender PS compile error: " + err).c_str());
			if (errBlob) errBlob->Release();
			return;
		}
		hr = dev->CreatePixelShader((const DWORD*)psBlob->GetBufferPointer(), &m_ps);
		psBlob->Release();
		if (FAILED(hr)) {
			reshade::log::message(reshade::log::level::error, "[HL2Craft] D3D9WorldRender: CreatePixelShader failed");
			return;
		}

		D3DVERTEXELEMENT9 decl[] = {
			{ 0,  0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 12, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			{ 0, 20, D3DDECLTYPE_UBYTE4N,  D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
			{ 0, 24, D3DDECLTYPE_UBYTE4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
			{ 0, 28, D3DDECLTYPE_UBYTE4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2 },
			D3DDECL_END()
		};

		hr = dev->CreateVertexDeclaration(decl, &m_vdecl);
		if (FAILED(hr)) {
			reshade::log::message(reshade::log::level::error, "[HL2Craft] D3D9WorldRender: CreateVertexDeclaration failed");
			return;
		}

		// Create 16x16 solid white fallback texture
		if (!m_fallbackTex) {
			HRESULT hrFb = dev->CreateTexture(16, 16, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_fallbackTex, nullptr);
			if (SUCCEEDED(hrFb)) {
				D3DLOCKED_RECT lr;
				if (SUCCEEDED(m_fallbackTex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
					std::memset(lr.pBits, 0xFF, 16 * lr.Pitch);
					m_fallbackTex->UnlockRect(0);
				}
			} else {
				hrFb = dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_fallbackTex, nullptr);
				if (SUCCEEDED(hrFb)) {
					D3DLOCKED_RECT lr;
					if (SUCCEEDED(m_fallbackTex->LockRect(0, &lr, nullptr, 0))) {
						std::memset(lr.pBits, 0xFF, 16 * lr.Pitch);
						m_fallbackTex->UnlockRect(0);
					}
				}
			}
		}

		m_initialized = true;
		reshade::log::message(reshade::log::level::info, "[HL2Craft] D3D9WorldRender initialized successfully!");
	}

	void D3D9WorldRender::DrainRing(IDirect3DDevice9 *dev)
	{
		const uint8_t *shmView = HL2Bridge::Get().GetSharedMemoryView();
		if (!shmView) return;

		const uint64_t head = *(const volatile uint64_t*)(shmView + proto::kOffRenderRing + proto::kRenRingHeadOff);
		uint64_t tail = *(const volatile uint64_t*)(shmView + proto::kOffRenderRing + proto::kRenRingTailOff);
		if (tail >= head) return;

		const uint64_t budget = 120ull << 20;
		const uint64_t stop = std::min(head, tail + budget);

		while (tail < stop) {
			uint64_t pos = tail % proto::kRenRingDataBytes;
			const uint8_t *msg = shmView + proto::kOffRenderRing + proto::kRenRingDataOff + pos;
			uint32_t type = *(const uint32_t*)msg;
			uint32_t payload = *(const uint32_t*)(msg + 4);
			uint64_t msgBytes = (8 + payload + 7) & ~7ull;

			if (type == proto::kRenPad) {
				tail += proto::kRenRingDataBytes - pos;
				continue;
			}

			const uint8_t *data = msg + 8;
			switch (type) {
			case proto::kRenClearAll: {
				ClearSections();
				break;
			}
			case proto::kRenAtlas: {
				if (payload >= sizeof(proto::RenAtlas)) {
					const auto *hdr = reinterpret_cast<const proto::RenAtlas*>(data);
					if (hdr->width > 0 && hdr->height > 0 && payload >= sizeof(proto::RenAtlas) + (size_t)hdr->width * hdr->height * 4) {
						if (m_atlasTex) {
							m_atlasTex->Release();
							m_atlasTex = nullptr;
						}
						if (m_atlasSysMem) {
							m_atlasSysMem->Release();
							m_atlasSysMem = nullptr;
						}
						m_atlasW = hdr->width;
						m_atlasH = hdr->height;

						// Preferred D3D9 / D3D9Ex: SYSTEMMEM staging texture + DEFAULT pool GPU texture updated via UpdateTexture
						HRESULT hrSys = dev->CreateTexture(m_atlasW, m_atlasH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &m_atlasSysMem, nullptr);
						HRESULT hrGpu = dev->CreateTexture(m_atlasW, m_atlasH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_atlasTex, nullptr);

						if (SUCCEEDED(hrSys) && SUCCEEDED(hrGpu)) {
							D3DLOCKED_RECT lr;
							if (SUCCEEDED(m_atlasSysMem->LockRect(0, &lr, nullptr, 0))) {
								const uint8_t *src = data + sizeof(proto::RenAtlas);
								for (uint32_t y = 0; y < m_atlasH; ++y) {
									std::memcpy((uint8_t*)lr.pBits + y * lr.Pitch, src + y * (m_atlasW * 4), m_atlasW * 4);
								}
								m_atlasSysMem->UnlockRect(0);
								dev->UpdateTexture(m_atlasSysMem, m_atlasTex);
							}
							char sbuf[128];
							snprintf(sbuf, sizeof(sbuf), "[HL2Craft] Atlas texture %ux%u loaded into D3D9 (UpdateTexture pair)", m_atlasW, m_atlasH);
							reshade::log::message(reshade::log::level::info, sbuf);
						} else {
							if (m_atlasSysMem) { m_atlasSysMem->Release(); m_atlasSysMem = nullptr; }
							if (m_atlasTex) { m_atlasTex->Release(); m_atlasTex = nullptr; }

							// Fallback 1: Dynamic default pool texture
							HRESULT hrTex = dev->CreateTexture(m_atlasW, m_atlasH, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_atlasTex, nullptr);
							DWORD lockFlags = D3DLOCK_DISCARD;
							if (FAILED(hrTex)) {
								// Fallback 2: Managed pool (standard D3D9 non-Ex)
								hrTex = dev->CreateTexture(m_atlasW, m_atlasH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m_atlasTex, nullptr);
								lockFlags = 0;
							}
							if (SUCCEEDED(hrTex)) {
								D3DLOCKED_RECT lr;
								if (SUCCEEDED(m_atlasTex->LockRect(0, &lr, nullptr, lockFlags))) {
									const uint8_t *src = data + sizeof(proto::RenAtlas);
									for (uint32_t y = 0; y < m_atlasH; ++y) {
										std::memcpy((uint8_t*)lr.pBits + y * lr.Pitch, src + y * (m_atlasW * 4), m_atlasW * 4);
									}
									m_atlasTex->UnlockRect(0);
								}
								char sbuf[128];
								snprintf(sbuf, sizeof(sbuf), "[HL2Craft] Atlas texture %ux%u loaded into D3D9 (fallback)", m_atlasW, m_atlasH);
								reshade::log::message(reshade::log::level::info, sbuf);
							} else {
								char buf[128]; snprintf(buf, sizeof(buf), "[HL2Craft] CreateTexture failed: sys=0x%08X gpu=0x%08X dyn=0x%08X", (unsigned)hrSys, (unsigned)hrGpu, (unsigned)hrTex);
								reshade::log::message(reshade::log::level::error, buf);
							}
						}
					}
				}
				break;
			}
			case proto::kRenAtlasRegion: {
				if (m_atlasTex && payload >= sizeof(proto::RenAtlasRegion)) {
					const auto *hdr = reinterpret_cast<const proto::RenAtlasRegion*>(data);
					if (payload >= sizeof(proto::RenAtlasRegion) + (size_t)hdr->width * hdr->height * 4) {
						RECT r;
						r.left = hdr->x;
						r.top = hdr->y;
						r.right = hdr->x + hdr->width;
						r.bottom = hdr->y + hdr->height;
						if (m_atlasSysMem) {
							D3DLOCKED_RECT lr;
							if (SUCCEEDED(m_atlasSysMem->LockRect(0, &lr, &r, 0))) {
								const uint8_t *src = data + sizeof(proto::RenAtlasRegion);
								for (uint32_t y = 0; y < hdr->height; ++y) {
									std::memcpy((uint8_t*)lr.pBits + y * lr.Pitch, src + y * (hdr->width * 4), hdr->width * 4);
								}
								m_atlasSysMem->UnlockRect(0);
								dev->UpdateTexture(m_atlasSysMem, m_atlasTex);
							}
						} else {
							D3DLOCKED_RECT lr;
							if (SUCCEEDED(m_atlasTex->LockRect(0, &lr, &r, 0))) {
								const uint8_t *src = data + sizeof(proto::RenAtlasRegion);
								for (uint32_t y = 0; y < hdr->height; ++y) {
									std::memcpy((uint8_t*)lr.pBits + y * lr.Pitch, src + y * (hdr->width * 4), hdr->width * 4);
								}
								m_atlasTex->UnlockRect(0);
							}
						}
					}
				}
				break;
			}
			case proto::kRenSection: {
				if (payload >= sizeof(proto::RenSection)) {
					const auto *hdr = reinterpret_cast<const proto::RenSection*>(data);
					SectionKey key{ hdr->sx, hdr->sy, hdr->sz };
					auto it = m_sections.find(key);
					if (it != m_sections.end()) {
						if (it->second.vb) it->second.vb->Release();
						m_sections.erase(it);
					}

					uint32_t count = hdr->vertexCount - (hdr->vertexCount % 3);
					if (count > 0 && payload >= sizeof(proto::RenSection) + (size_t)count * sizeof(proto::RenVertex)) {
						const auto *src = reinterpret_cast<const proto::RenVertex*>(data + sizeof(proto::RenSection));
						m_scratch.clear();
						m_scratch.reserve(count);

						uint32_t opaqueCount = 0;
						for (int pass = 0; pass < 2; ++pass) {
							for (uint32_t t = 0; t < count; t += 3) {
								bool translucent = (src[t].flags & 2) != 0;
								if (translucent == (pass == 1)) {
									m_scratch.insert(m_scratch.end(), src + t, src + t + 3);
								}
							}
							if (pass == 0) {
								opaqueCount = (uint32_t)m_scratch.size();
							}
						}

						SectionMesh &mesh = m_sections[key];
						mesh.sx = hdr->sx;
						mesh.sy = hdr->sy;
						mesh.sz = hdr->sz;
						mesh.opaque = opaqueCount;
						mesh.translucent = (uint32_t)m_scratch.size() - opaqueCount;

						UINT byteSize = (UINT)(m_scratch.size() * sizeof(proto::RenVertex));
						HRESULT hrVB = dev->CreateVertexBuffer(byteSize, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &mesh.vb, nullptr);
						DWORD lockFlags = D3DLOCK_DISCARD;
						if (FAILED(hrVB)) {
							hrVB = dev->CreateVertexBuffer(byteSize, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &mesh.vb, nullptr);
							lockFlags = 0;
						}
						if (FAILED(hrVB)) {
							hrVB = dev->CreateVertexBuffer(byteSize, 0, 0, D3DPOOL_MANAGED, &mesh.vb, nullptr);
							lockFlags = 0;
						}

						if (SUCCEEDED(hrVB) && mesh.vb) {
							void *dst = nullptr;
							HRESULT hrLock = mesh.vb->Lock(0, byteSize, &dst, lockFlags);
							if (FAILED(hrLock) && lockFlags != 0) {
								hrLock = mesh.vb->Lock(0, byteSize, &dst, 0);
							}
							if (SUCCEEDED(hrLock) && dst) {
								std::memcpy(dst, m_scratch.data(), byteSize);
								mesh.vb->Unlock();
							}
							if (m_sections.size() <= 10 || m_sections.size() % 50 == 0) {
								char sbuf[128];
								snprintf(sbuf, sizeof(sbuf), "[HL2Craft] Section (%d, %d, %d) added (%u verts, total %zu sections)",
									hdr->sx, hdr->sy, hdr->sz, hdr->vertexCount, m_sections.size());
								reshade::log::message(reshade::log::level::info, sbuf);
							}
						} else {
							char sbuf[128];
							snprintf(sbuf, sizeof(sbuf), "[HL2Craft] CreateVertexBuffer failed: 0x%08X (size %u)", (unsigned)hrVB, byteSize);
							reshade::log::message(reshade::log::level::error, sbuf);
							m_sections.erase(key);
						}
					}
				}
				break;
			}
			case proto::kRenTexture: {
				if (payload >= sizeof(proto::RenTexture)) {
					const auto *hdr = reinterpret_cast<const proto::RenTexture*>(data);
					if (hdr->id > 0 && hdr->width > 0 && hdr->height > 0 &&
						payload >= sizeof(proto::RenTexture) + (size_t)hdr->width * hdr->height * 4) {
						auto it = m_entityTextures.find(hdr->id);
						if (it != m_entityTextures.end()) {
							if (it->second) it->second->Release();
							m_entityTextures.erase(it);
						}

						IDirect3DTexture9 *sysTex = nullptr;
						IDirect3DTexture9 *gpuTex = nullptr;
						HRESULT hrSys = dev->CreateTexture(hdr->width, hdr->height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysTex, nullptr);
						HRESULT hrGpu = dev->CreateTexture(hdr->width, hdr->height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &gpuTex, nullptr);
						if (SUCCEEDED(hrSys) && SUCCEEDED(hrGpu)) {
							D3DLOCKED_RECT lr;
							if (SUCCEEDED(sysTex->LockRect(0, &lr, nullptr, 0))) {
								const uint8_t *src = data + sizeof(proto::RenTexture);
								for (uint32_t y = 0; y < hdr->height; ++y) {
									std::memcpy((uint8_t*)lr.pBits + y * lr.Pitch, src + y * (hdr->width * 4), hdr->width * 4);
								}
								sysTex->UnlockRect(0);
								dev->UpdateTexture(sysTex, gpuTex);
							}
							sysTex->Release();
							m_entityTextures[hdr->id] = gpuTex;
						} else {
							if (sysTex) sysTex->Release();
							if (gpuTex) gpuTex->Release();
							IDirect3DTexture9 *manTex = nullptr;
							if (SUCCEEDED(dev->CreateTexture(hdr->width, hdr->height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &manTex, nullptr)) && manTex) {
								D3DLOCKED_RECT lr;
								if (SUCCEEDED(manTex->LockRect(0, &lr, nullptr, 0))) {
									const uint8_t *src = data + sizeof(proto::RenTexture);
									for (uint32_t y = 0; y < hdr->height; ++y) {
										std::memcpy((uint8_t*)lr.pBits + y * lr.Pitch, src + y * (hdr->width * 4), hdr->width * 4);
									}
									manTex->UnlockRect(0);
								}
								m_entityTextures[hdr->id] = manTex;
							}
						}
						char sbuf[128];
						snprintf(sbuf, sizeof(sbuf), "[HL2Craft] Entity texture %u (%ux%u) created in D3D9", hdr->id, hdr->width, hdr->height);
						reshade::log::message(reshade::log::level::info, sbuf);
					}
				}
				break;
			}
			case proto::kRenScene: {
				if (payload >= sizeof(proto::RenScene)) {
					const auto *hdr = reinterpret_cast<const proto::RenScene*>(data);
					m_sceneOriginX = hdr->originX;
					m_sceneOriginY = hdr->originY;
					m_sceneOriginZ = hdr->originZ;
					m_sceneBatches.clear();

					size_t head = sizeof(proto::RenScene);
					size_t need = head + (size_t)hdr->batchCount * sizeof(proto::RenBatch) + (size_t)hdr->vertexCount * sizeof(proto::RenVertex);
					if (hdr->batchCount > 0 && hdr->vertexCount > 0 && payload >= need) {
						const auto *batches = reinterpret_cast<const proto::RenBatch*>(data + head);
						const auto *verts = reinterpret_cast<const proto::RenVertex*>(batches + hdr->batchCount);

						for (uint32_t b = 0; b < hdr->batchCount; ++b) {
							if (batches[b].first + batches[b].count <= hdr->vertexCount) {
								m_sceneBatches.push_back(batches[b]);
							}
						}

						UINT bytes = (UINT)(hdr->vertexCount * sizeof(proto::RenVertex));
						if (bytes > m_sceneVbCapacity || !m_sceneVb) {
							if (m_sceneVb) { m_sceneVb->Release(); m_sceneVb = nullptr; }
							UINT allocBytes = std::max<UINT>(bytes * 2, 256 * 1024);
							HRESULT hr = dev->CreateVertexBuffer(allocBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &m_sceneVb, nullptr);
							if (SUCCEEDED(hr)) {
								m_sceneVbCapacity = allocBytes;
							} else {
								m_sceneVbCapacity = 0;
							}
						}

						if (m_sceneVb) {
							void *dst = nullptr;
							if (SUCCEEDED(m_sceneVb->Lock(0, bytes, &dst, D3DLOCK_DISCARD)) && dst) {
								std::memcpy(dst, verts, bytes);
								m_sceneVb->Unlock();
								m_sceneVertCount = hdr->vertexCount;
							}
						}
					} else {
						m_sceneVertCount = 0;
					}
				}
				break;
			}
			default:
				break;
			}

			tail += msgBytes;
		}

		*(volatile uint64_t*)(shmView + proto::kOffRenderRing + proto::kRenRingTailOff) = tail;
	}

	static inline uint32_t ShadeColor(float shade, uint32_t tint = 0)
	{
		float r = shade, g = shade, b = shade;
		if (tint) {
			r *= float(tint & 0xFF) / 255.0f;
			g *= float((tint >> 8) & 0xFF) / 255.0f;
			b *= float((tint >> 16) & 0xFF) / 255.0f;
		}
		return 0xFF000000u | (uint32_t(b * 255.0f) << 16) | (uint32_t(g * 255.0f) << 8) | uint32_t(r * 255.0f);
	}

	void D3D9WorldRender::Quad(const float p[4][3], const float uv[4], uint32_t color, uint32_t flags)
	{
		const float uvs[4][2] = { { uv[0], uv[1] }, { uv[2], uv[1] }, { uv[2], uv[3] }, { uv[0], uv[3] } };
		for (int k : { 0, 1, 2, 0, 2, 3 }) {
			m_dynVerts.push_back({ p[k][0], p[k][1], p[k][2], uvs[k][0], uvs[k][1], color, kFullSkyLight, flags });
		}
	}

	void D3D9WorldRender::Box(const float min[3], const float size[3], float yaw, const float side[4], const float top[4], const float bottom[4],
		uint32_t topTint, uint32_t flags, bool shaded)
	{
		const float cx = min[0] + size[0] * 0.5f, cz = min[2] + size[2] * 0.5f;
		const float c = std::cos(yaw), s = std::sin(yaw);
		auto corner = [&](int i, float out[3]) {
			const float lx = ((i & 1) ? 0.5f : -0.5f) * size[0], lz = ((i & 4) ? 0.5f : -0.5f) * size[2];
			out[0] = cx + lx * c - lz * s;
			out[1] = min[1] + ((i & 2) ? size[1] : 0.0f);
			out[2] = cz + lx * s + lz * c;
		};
		static constexpr int kFaces[6][4] = {
			{ 6, 7, 5, 4 }, // south (+z)
			{ 3, 2, 0, 1 }, // north (-z)
			{ 7, 3, 1, 5 }, // east (+x)
			{ 2, 6, 4, 0 }, // west (-x)
			{ 2, 3, 7, 6 }, // top
			{ 4, 5, 1, 0 }, // bottom
		};
		for (int f = 0; f < 6; ++f) {
			float p[4][3];
			for (int k = 0; k < 4; ++k) {
				corner(kFaces[f][k], p[k]);
			}
			const float *faceUv = f == 4 ? top : f == 5 ? bottom : side;
			const uint32_t color = shaded ? ShadeColor(1.0f, f == 4 ? topTint : 0) : 0xFFFFFFFFu;
			Quad(p, faceUv, color, flags | kFlagNormalFromFaces);
		}
	}

	void D3D9WorldRender::BuildArrow(float px, float py, float pz, const float d[3], const float *uvSide, const float *uvBack, bool trident)
	{
		float s[3] = { d[2], 0.0f, -d[0] };
		float sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);
		if (sl < 1e-3f) {
			s[0] = 1.0f; s[2] = 0.0f; sl = 1.0f;
		}
		s[0] /= sl; s[2] /= sl;
		const float u[3] = { s[1] * d[2] - s[2] * d[1], s[2] * d[0] - s[0] * d[2], s[0] * d[1] - s[1] * d[0] };
		constexpr float r = 0.70710678f;
		const float fins[2][3] = {
			{ (u[0] + s[0]) * r, (u[1] + s[1]) * r, (u[2] + s[2]) * r },
			{ (u[0] - s[0]) * r, (u[1] - s[1]) * r, (u[2] - s[2]) * r }
		};
		auto at = [&](float along, const float *q, float side, const float *q2, float side2, float out[3]) {
			for (int k = 0; k < 3; ++k) {
				out[k] = (k == 0 ? px : k == 1 ? py : pz) + d[k] * along + q[k] * side + (q2 ? q2[k] * side2 : 0.0f);
			}
		};
		if (!trident) {
			constexpr float k = 0.9f / 16.0f * kArrowScale;
			for (const auto &q : fins) {
				float p[4][3];
				at(-12 * k, q, -2 * k, nullptr, 0, p[0]);
				at(4 * k, q, -2 * k, nullptr, 0, p[1]);
				at(4 * k, q, 2 * k, nullptr, 0, p[2]);
				at(-12 * k, q, 2 * k, nullptr, 0, p[3]);
				Quad(p, uvSide, 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
			}
			float p[4][3];
			at(-11 * k, fins[0], -2 * k, fins[1], -2 * k, p[0]);
			at(-11 * k, fins[0], 2 * k, fins[1], -2 * k, p[1]);
			at(-11 * k, fins[0], 2 * k, fins[1], 2 * k, p[2]);
			at(-11 * k, fins[0], -2 * k, fins[1], 2 * k, p[3]);
			Quad(p, uvBack, 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
		} else {
			constexpr float h = 0.9f;
			for (const auto &q : fins) {
				float p[4][3];
				at(0, q, h, nullptr, 0, p[0]);
				at(h, q, 0, nullptr, 0, p[1]);
				at(0, q, -h, nullptr, 0, p[2]);
				at(-h, q, 0, nullptr, 0, p[3]);
				Quad(p, uvSide, 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
			}
		}
	}

	void D3D9WorldRender::MobPart(
		float px, float py, float pz,
		float cx, float cy, float cz,
		float sx, float sy, float sz,
		float yaw,
		const float *mobUv,
		int tx, int ty, int tw, int th, int td,
		int sheetW, int sheetH,
		uint32_t tint,
		uint32_t fallbackColor)
	{
		const float c = std::cos(yaw), s = std::sin(yaw);
		const float rotCx = cx * c - cz * s;
		const float rotCz = cx * s + cz * c;
		const float min[3] = {
			px + rotCx - sx * 0.5f,
			cy - sy * 0.5f,
			pz + rotCz - sz * 0.5f
		};
		const float size[3] = { sx, sy, sz };

		bool hasTexture = (mobUv != nullptr && mobUv[2] > mobUv[0] && mobUv[3] > mobUv[1]);
		if (!hasTexture) {
			auto corner = [&](int i, float out[3]) {
				const float lx = ((i & 1) ? 0.5f : -0.5f) * size[0], lz = ((i & 4) ? 0.5f : -0.5f) * size[2];
				out[0] = px + rotCx + lx * c - lz * s;
				out[1] = min[1] + ((i & 2) ? size[1] : 0.0f);
				out[2] = pz + rotCz + lx * s + lz * c;
			};
			static constexpr int kFaces[6][4] = {
				{ 6, 7, 5, 4 }, // back (+z)
				{ 3, 2, 0, 1 }, // front (-z)
				{ 7, 3, 1, 5 }, // right (+x)
				{ 2, 6, 4, 0 }, // left (-x)
				{ 2, 3, 7, 6 }, // top
				{ 4, 5, 1, 0 }, // bottom
			};
			uint32_t col = tint ? tint : fallbackColor;
			float dummyUv[4] = { 0, 0, 0, 0 };
			for (int f = 0; f < 6; ++f) {
				float p[4][3];
				for (int k = 0; k < 4; ++k) corner(kFaces[f][k], p[k]);
				Quad(p, dummyUv, col, kFlagUntextured | kFlagNormalFromFaces);
			}
			return;
		}

		float uScale = (mobUv[2] - mobUv[0]) / (float)sheetW;
		float vScale = (mobUv[3] - mobUv[1]) / (float)sheetH;
		float u0 = mobUv[0], v0 = mobUv[1];

		float topUv[4] = {
			u0 + (tx + td) * uScale, v0 + ty * vScale,
			u0 + (tx + td + tw) * uScale, v0 + (ty + td) * vScale
		};
		float bottomUv[4] = {
			u0 + (tx + td + tw) * uScale, v0 + ty * vScale,
			u0 + (tx + td + 2 * tw) * uScale, v0 + (ty + td) * vScale
		};
		float rightUv[4] = {
			u0 + tx * uScale, v0 + (ty + td) * vScale,
			u0 + (tx + td) * uScale, v0 + (ty + td + th) * vScale
		};
		float frontUv[4] = {
			u0 + (tx + td) * uScale, v0 + (ty + td) * vScale,
			u0 + (tx + td + tw) * uScale, v0 + (ty + td + th) * vScale
		};
		float leftUv[4] = {
			u0 + (tx + td + tw) * uScale, v0 + (ty + td) * vScale,
			u0 + (tx + 2 * td + tw) * uScale, v0 + (ty + td + th) * vScale
		};
		float backUv[4] = {
			u0 + (tx + 2 * td + tw) * uScale, v0 + (ty + td) * vScale,
			u0 + (tx + 2 * td + 2 * tw) * uScale, v0 + (ty + td + th) * vScale
		};

		auto corner = [&](int i, float out[3]) {
			const float lx = ((i & 1) ? 0.5f : -0.5f) * size[0], lz = ((i & 4) ? 0.5f : -0.5f) * size[2];
			out[0] = px + rotCx + lx * c - lz * s;
			out[1] = min[1] + ((i & 2) ? size[1] : 0.0f);
			out[2] = pz + rotCz + lx * s + lz * c;
		};

		static constexpr int kFaces[6][4] = {
			{ 6, 7, 5, 4 }, // back (+z)
			{ 3, 2, 0, 1 }, // front (-z)
			{ 7, 3, 1, 5 }, // right (+x)
			{ 2, 6, 4, 0 }, // left (-x)
			{ 2, 3, 7, 6 }, // top
			{ 4, 5, 1, 0 }, // bottom
		};
		const float *faceUvs[6] = { backUv, frontUv, rightUv, leftUv, topUv, bottomUv };
		uint32_t col = tint ? tint : 0xFFFFFFFFu;
		for (int f = 0; f < 6; ++f) {
			float p[4][3];
			for (int k = 0; k < 4; ++k) corner(kFaces[f][k], p[k]);
			Quad(p, faceUvs[f], col, kFlagCutout | kFlagNoMip | kFlagNormalFromFaces);
		}
	}

	void D3D9WorldRender::BuildMob(
		float px, float py, float pz,
		float yawDeg, float pitchDeg,
		uint32_t mobType, float height, float width, float scale,
		const float *mobUv, uint32_t tint)
	{
		float yaw = yawDeg * kPi / 180.0f;
		float s = scale > 0.01f ? scale : 1.0f;

		switch (mobType) {
		case 1: { // Creeper
			// Head: (w=8, h=8, d=8)
			MobPart(px, py, pz, 0, py + 1.45f * s, 0, 0.5f * s, 0.5f * s, 0.5f * s, yaw, mobUv, 0, 0, 8, 8, 8, 64, 32, tint, 0xFF44AA44);
			// Body: (w=8, h=12, d=4)
			MobPart(px, py, pz, 0, py + 0.825f * s, 0, 0.5f * s, 0.75f * s, 0.25f * s, yaw, mobUv, 16, 16, 8, 12, 4, 64, 32, tint, 0xFF389838);
			// 4 Legs: (w=4, h=6, d=4)
			float ly = py + 0.1875f * s;
			float lsz[3] = { 0.25f * s, 0.375f * s, 0.25f * s };
			MobPart(px, py, pz, +0.125f * s, ly, -0.1875f * s, lsz[0], lsz[1], lsz[2], yaw, mobUv, 0, 16, 4, 6, 4, 64, 32, tint, 0xFF338833);
			MobPart(px, py, pz, -0.125f * s, ly, -0.1875f * s, lsz[0], lsz[1], lsz[2], yaw, mobUv, 0, 16, 4, 6, 4, 64, 32, tint, 0xFF338833);
			MobPart(px, py, pz, +0.125f * s, ly, +0.1875f * s, lsz[0], lsz[1], lsz[2], yaw, mobUv, 0, 16, 4, 6, 4, 64, 32, tint, 0xFF338833);
			MobPart(px, py, pz, -0.125f * s, ly, +0.1875f * s, lsz[0], lsz[1], lsz[2], yaw, mobUv, 0, 16, 4, 6, 4, 64, 32, tint, 0xFF338833);
			break;
		}
		case 2: // Zombie
		case 3: // Skeleton
		case 10: { // Player / Humanoid
			uint32_t defCol = (mobType == 2) ? 0xFF227744 : (mobType == 3 ? 0xFFD0D0D0 : 0xFF336699);
			// Head: (w=8, h=8, d=8)
			MobPart(px, py, pz, 0, py + 1.625f * s, 0, 0.5f * s, 0.5f * s, 0.5f * s, yaw, mobUv, 0, 0, 8, 8, 8, 64, 64, tint, defCol);
			// Body: (w=8, h=12, d=4)
			MobPart(px, py, pz, 0, py + 1.0f * s, 0, 0.5f * s, 0.75f * s, 0.25f * s, yaw, mobUv, 16, 16, 8, 12, 4, 64, 64, tint, defCol);
			// Arms (Zombie reaches forward: cz = -0.25f)
			float armZ = (mobType == 2) ? -0.25f * s : 0.0f;
			MobPart(px, py, pz, +0.375f * s, py + 1.0f * s, armZ, 0.25f * s, 0.75f * s, 0.25f * s, yaw, mobUv, 40, 16, 4, 12, 4, 64, 64, tint, defCol);
			MobPart(px, py, pz, -0.375f * s, py + 1.0f * s, armZ, 0.25f * s, 0.75f * s, 0.25f * s, yaw, mobUv, 40, 16, 4, 12, 4, 64, 64, tint, defCol);
			// Legs
			MobPart(px, py, pz, +0.125f * s, py + 0.375f * s, 0, 0.25f * s, 0.75f * s, 0.25f * s, yaw, mobUv, 0, 16, 4, 12, 4, 64, 64, tint, defCol);
			MobPart(px, py, pz, -0.125f * s, py + 0.375f * s, 0, 0.25f * s, 0.75f * s, 0.25f * s, yaw, mobUv, 0, 16, 4, 12, 4, 64, 64, tint, defCol);
			break;
		}
		case 4: { // Spider
			// Head: (w=8, h=8, d=8)
			MobPart(px, py, pz, 0, py + 0.35f * s, -0.4f * s, 0.5f * s, 0.5f * s, 0.5f * s, yaw, mobUv, 32, 4, 8, 8, 8, 64, 32, tint, 0xFF332222);
			// Abdomen: (w=12, h=8, d=12)
			MobPart(px, py, pz, 0, py + 0.45f * s, +0.3f * s, 0.75f * s, 0.5f * s, 0.75f * s, yaw, mobUv, 0, 12, 12, 8, 12, 64, 32, tint, 0xFF2A1C1C);
			// Left & right legs
			MobPart(px, py, pz, +0.55f * s, py + 0.25f * s, 0, 0.45f * s, 0.15f * s, 0.6f * s, yaw, mobUv, 18, 0, 4, 4, 4, 64, 32, tint, 0xFF221111);
			MobPart(px, py, pz, -0.55f * s, py + 0.25f * s, 0, 0.45f * s, 0.15f * s, 0.6f * s, yaw, mobUv, 18, 0, 4, 4, 4, 64, 32, tint, 0xFF221111);
			break;
		}
		case 5: { // Enderman
			// Head: (w=8, h=8, d=8)
			MobPart(px, py, pz, 0, py + 2.6f * s, 0, 0.5f * s, 0.5f * s, 0.5f * s, yaw, mobUv, 0, 0, 8, 8, 8, 64, 32, tint, 0xFF141414);
			// Body: (w=8, h=12, d=4)
			MobPart(px, py, pz, 0, py + 1.9f * s, 0, 0.5f * s, 0.85f * s, 0.25f * s, yaw, mobUv, 16, 16, 8, 12, 4, 64, 32, tint, 0xFF141414);
			// Long arms
			MobPart(px, py, pz, +0.3f * s, py + 1.4f * s, 0, 0.125f * s, 1.8f * s, 0.125f * s, yaw, mobUv, 56, 0, 2, 16, 2, 64, 32, tint, 0xFF141414);
			MobPart(px, py, pz, -0.3f * s, py + 1.4f * s, 0, 0.125f * s, 1.8f * s, 0.125f * s, yaw, mobUv, 56, 0, 2, 16, 2, 64, 32, tint, 0xFF141414);
			// Long legs
			MobPart(px, py, pz, +0.1f * s, py + 0.75f * s, 0, 0.125f * s, 1.5f * s, 0.125f * s, yaw, mobUv, 56, 0, 2, 16, 2, 64, 32, tint, 0xFF141414);
			MobPart(px, py, pz, -0.1f * s, py + 0.75f * s, 0, 0.125f * s, 1.5f * s, 0.125f * s, yaw, mobUv, 56, 0, 2, 16, 2, 64, 32, tint, 0xFF141414);
			break;
		}
		case 6: // Pig
		case 7: // Cow
		case 8: { // Sheep
			uint32_t qCol = (mobType == 6) ? 0xFFF09999 : (mobType == 7 ? 0xFF503828 : 0xFFEEEEEE);
			// Head
			MobPart(px, py, pz, 0, py + 0.75f * s, -0.55f * s, 0.5f * s, 0.5f * s, 0.5f * s, yaw, mobUv, 0, 0, 8, 8, 8, 64, 32, tint, qCol);
			// Body (horizontal)
			MobPart(px, py, pz, 0, py + 0.6f * s, 0, 0.625f * s, 0.55f * s, 1.0f * s, yaw, mobUv, 28, 8, 10, 16, 8, 64, 32, tint, qCol);
			// 4 Legs
			float ly = py + 0.2f * s;
			float lw = 0.25f * s, lh = 0.4f * s;
			MobPart(px, py, pz, +0.2f * s, ly, -0.35f * s, lw, lh, lw, yaw, mobUv, 0, 16, 4, 6, 4, 64, 32, tint, qCol);
			MobPart(px, py, pz, -0.2f * s, ly, -0.35f * s, lw, lh, lw, yaw, mobUv, 0, 16, 4, 6, 4, 64, 32, tint, qCol);
			MobPart(px, py, pz, +0.2f * s, ly, +0.35f * s, lw, lh, lw, yaw, mobUv, 0, 16, 4, 6, 4, 64, 32, tint, qCol);
			MobPart(px, py, pz, -0.2f * s, ly, +0.35f * s, lw, lh, lw, yaw, mobUv, 0, 16, 4, 6, 4, 64, 32, tint, qCol);
			break;
		}
		case 9: { // Chicken
			// Head
			MobPart(px, py, pz, 0, py + 0.55f * s, -0.2f * s, 0.25f * s, 0.35f * s, 0.25f * s, yaw, mobUv, 0, 0, 4, 6, 3, 64, 32, tint, 0xFFFFFFFF);
			// Body
			MobPart(px, py, pz, 0, py + 0.3f * s, 0, 0.35f * s, 0.35f * s, 0.45f * s, yaw, mobUv, 0, 9, 6, 8, 6, 64, 32, tint, 0xFFEEEEEE);
			// 2 Legs
			MobPart(px, py, pz, +0.08f * s, py + 0.1f * s, 0, 0.1f * s, 0.2f * s, 0.1f * s, yaw, mobUv, 26, 0, 3, 5, 3, 64, 32, tint, 0xFFFFAA00);
			MobPart(px, py, pz, -0.08f * s, py + 0.1f * s, 0, 0.1f * s, 0.2f * s, 0.1f * s, yaw, mobUv, 26, 0, 3, 5, 3, 64, 32, tint, 0xFFFFAA00);
			break;
		}
		default: { // Generic fallback
			float h = height > 0.1f ? height * s : 1.8f * s;
			float w = width > 0.1f ? width * s : 0.6f * s;
			MobPart(px, py, pz, 0, py + h * 0.5f, 0, w, h, w, yaw, nullptr, 0, 0, 0, 0, 0, 64, 32, tint, 0xFFAAAAAA);
			break;
		}
		}
	}

	void D3D9WorldRender::BuildEntities(const double o[3], const Vector &camPos)
	{
		for (uint32_t i = 0; i < m_entities.count && i < proto::kMaxWorldEntities; ++i) {
			const auto &e = m_entities.entities[i];
			const float px = float(e.x - o[0]), py = float(e.y - o[1]), pz = float(e.z - o[2]);
			if (e.kind == proto::kWeShadow) {
				continue;
			}

			if (e.kind != proto::kWeCrack) {
				float checkH = (e.kind == proto::kWeMob) ? (e.ext[1] * 0.6f) : 0.0f;
				Vector entWorldPos(
					(float)(e.x * proto::kUnitsPerBlock),
					(float)(-e.z * proto::kUnitsPerBlock),
					(float)((e.y + checkH - 64.0) * proto::kUnitsPerBlock)
				);
				trace_t tr;
				if (HL2Bridge::Get().TraceWorldRay(camPos, entWorldPos, tr)) {
					if (tr.fraction < 0.95f && !tr.startsolid) {
						continue;
					}
				}
			}

			if (e.kind == proto::kWeMob) {
				if (m_sceneVertCount == 0) {
					BuildMob(px, py, pz, e.yaw, e.pitch, (uint32_t)e.ext[0], e.ext[1], e.ext[2], e.scale, e.uv[0], e.tint);
				}
				continue;
			}
			if (e.kind == proto::kWeBlock) {
				const float s = e.scale;
				const float mn[3] = { px - s * 0.5f, py - s * 0.5f, pz - s * 0.5f };
				const float sz[3] = { s, s, s };
				Box(mn, sz, e.yaw * kPi / 180.0f, e.uv[0], e.uv[1], e.uv[2], e.tint, kFlagCutout | kFlagNoMip, true);
				continue;
			}
			if (e.kind == proto::kWeCrack) {
				const float mn[3] = { px, py, pz };
				const size_t before = m_dynVerts.size();
				Box(mn, e.ext, 0.0f, e.uv[0], e.uv[0], e.uv[0], 0, kFlagTranslucent | kFlagNoMip, false);
				m_crackVerts.insert(m_crackVerts.end(), m_dynVerts.begin() + before, m_dynVerts.end());
				m_dynVerts.resize(before);
				continue;
			}
			if (e.kind == proto::kWeArrow || e.kind == proto::kWeTrident) {
				const float yaw = e.yaw * kPi / 180.0f, pitch = e.pitch * kPi / 180.0f;
				const float d[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
				BuildArrow(px, py, pz, d, e.uv[0], e.uv[1], e.kind == proto::kWeTrident);
			} else if (e.kind == proto::kWeItem) {
				const float spin = e.yaw * kPi / 180.0f, half = e.scale * 0.5f;
				const float rx = std::cos(spin) * half, rz = std::sin(spin) * half;
				const float p[4][3] = {
					{ px - rx, py + half, pz - rz },
					{ px + rx, py + half, pz + rz },
					{ px + rx, py - half, pz + rz },
					{ px - rx, py - half, pz - rz },
				};
				Quad(p, e.uv[0], 0xFFFFFFFFu, kFlagCutout | kFlagNoMip);
			}
		}
	}

	void D3D9WorldRender::BuildOutline(const double o[3], const Vector &camPos)
	{
		if (!m_entities.hasSelection) return;
		Vector targetWorldPos(
			(float)(((m_entities.selMin[0] + m_entities.selMax[0]) * 0.5) * proto::kUnitsPerBlock),
			(float)((-(m_entities.selMin[2] + m_entities.selMax[2]) * 0.5) * proto::kUnitsPerBlock),
			(float)((((m_entities.selMin[1] + m_entities.selMax[1]) * 0.5) - 64.0) * proto::kUnitsPerBlock)
		);
		trace_t tr;
		if (HL2Bridge::Get().TraceWorldRay(camPos, targetWorldPos, tr)) {
			if (tr.fraction < 0.95f && !tr.startsolid) {
				return;
			}
		}
		constexpr float g = 0.002f;
		const float lo[3] = { float(m_entities.selMin[0] - o[0]) - g, float(m_entities.selMin[1] - o[1]) - g, float(m_entities.selMin[2] - o[2]) - g };
		const float hi[3] = { float(m_entities.selMax[0] - o[0]) + g, float(m_entities.selMax[1] - o[1]) + g, float(m_entities.selMax[2] - o[2]) + g };
		auto corner = [&](int i) -> std::array<float, 3> {
			return { (i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2] };
		};
		static constexpr int kEdges[12][2] = {
			{ 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },
			{ 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },
			{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
		};
		constexpr uint32_t kOutline = 0x73000000u; // black, 45% (Minecraft's wireframe outline)
		for (const auto &edge : kEdges) {
			for (int k : edge) {
				const auto c = corner(k);
				m_outlineVerts.push_back({ c[0], c[1], c[2], 0, 0, kOutline, kFullSkyLight, kFlagUntextured });
			}
		}
	}

	bool D3D9WorldRender::UploadDynamic(IDirect3DDevice9 *dev)
	{
		if (m_uploadVerts.empty() || !dev) return false;
		uint32_t vertCount = (uint32_t)m_uploadVerts.size();
		if (!m_dynVb || m_dynVbCapacity < vertCount) {
			if (m_dynVb) { m_dynVb->Release(); m_dynVb = nullptr; }
			m_dynVbCapacity = std::max(vertCount + 512, 4096u);
			HRESULT hr = dev->CreateVertexBuffer(
				m_dynVbCapacity * sizeof(proto::RenVertex),
				D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
				0,
				D3DPOOL_DEFAULT,
				&m_dynVb,
				nullptr
			);
			if (FAILED(hr)) {
				m_dynVbCapacity = 0;
				return false;
			}
		}
		void *dst = nullptr;
		HRESULT hrLock = m_dynVb->Lock(0, vertCount * sizeof(proto::RenVertex), &dst, D3DLOCK_DISCARD);
		if (FAILED(hrLock) || !dst) return false;
		std::memcpy(dst, m_uploadVerts.data(), vertCount * sizeof(proto::RenVertex));
		m_dynVb->Unlock();
		return true;
	}

	void D3D9WorldRender::Prepare(reshade::api::effect_runtime *runtime)
	{
		if (!runtime) return;
		reshade::api::device *rdev = runtime->get_device();
		if (!rdev || rdev->get_api() != reshade::api::device_api::d3d9) return;

		IDirect3DDevice9 *dev = reinterpret_cast<IDirect3DDevice9*>(rdev->get_native());
		if (!dev) return;

		Init(dev);
		if (!m_initialized) return;

		DrainRing(dev);
	}

	void D3D9WorldRender::Render(IDirect3DDevice9 *dev, const float *gameViewProj)
	{
		if (!dev || !gameViewProj || !m_initialized) return;
		IDirect3DSurface9 *target = nullptr; // L4D2's own render target stays bound (Left4Craft)

		if (!HL2Bridge::Get().IsInPlayableWorld() || HL2Bridge::Get().IsPaused()) {
			return;
		}

		Vector playerPos{0, 0, 0};
		QAngle playerAngles{0, 0, 0};
		if (!HL2Bridge::Get().GetPlayerPos(playerPos) || !HL2Bridge::Get().GetAngles(playerAngles)) {
			return;
		}

		Vector camPos = { 0, 0, 0 };
		Vector fwd = { 0, 0, 0 };
		Vector right = { 0, 0, 0 };
		Vector up = { 0, 0, 0 };

		void *pPlayer = HL2Bridge::Get().GetLocalPlayerEntity();
		bool isDucking = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		float eyeHeight = isDucking ? l4d2::kEyeDuck : l4d2::kEyeStand;
		if (int *pFlags = pPlayer ? l4d2::Netvar<int>(pPlayer, "m_fFlags") : nullptr) {
			if (*pFlags & 2) eyeHeight = l4d2::kEyeDuck; // FL_DUCKING
		}
		camPos = { playerPos.x, playerPos.y, playerPos.z + eyeHeight };

		constexpr float kDeg2Rad = 0.017453292519943295f;
		float p = playerAngles.pitch * kDeg2Rad;
		float y = playerAngles.yaw * kDeg2Rad;
		fwd = { std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p) };
		right = { std::sin(y), -std::cos(y), 0.0f };
		up = { std::cos(y) * std::sin(p), std::sin(y) * std::sin(p), std::cos(p) };

		Matrix4x4 viewMat{};
		viewMat.m[0][0] = right.x; viewMat.m[0][1] = up.x; viewMat.m[0][2] = fwd.x; viewMat.m[0][3] = 0.0f;
		viewMat.m[1][0] = right.y; viewMat.m[1][1] = up.y; viewMat.m[1][2] = fwd.y; viewMat.m[1][3] = 0.0f;
		viewMat.m[2][0] = right.z; viewMat.m[2][1] = up.z; viewMat.m[2][2] = fwd.z; viewMat.m[2][3] = 0.0f;
		viewMat.m[3][0] = 0.0f;    viewMat.m[3][1] = 0.0f; viewMat.m[3][2] = 0.0f;  viewMat.m[3][3] = 1.0f;

		D3DVIEWPORT9 origVp{};
		dev->GetViewport(&origVp);
		float width = (origVp.Width > 0) ? (float)origVp.Width : 1920.0f;
		float height = (origVp.Height > 0) ? (float)origVp.Height : 1080.0f;
		float aspect = width / height;

		// Source engine vertical FOV from shared memory (default 75 deg 4:3 -> ~59 deg vert)
		float vertFovDeg = 59.0f;
		const uint8_t *shm = HL2Bridge::Get().GetSharedMemoryView();
		if (shm) {
			const auto *hs = reinterpret_cast<const proto::HostState*>(shm + proto::kOffHostState);
			if (hs->fov >= 20.0f && hs->fov <= 160.0f) {
				vertFovDeg = hs->fov;
			}
		}
		float fovYRad = vertFovDeg * kDeg2Rad;
		float f_y = 1.0f / std::tan(fovYRad * 0.5f);
		float f_x = f_y / aspect;
		float nearZ = 7.0f;
		float farZ = 28400.0f;

		Matrix4x4 projMat{};
		projMat.m[0][0] = f_x;
		projMat.m[1][1] = f_y;
		projMat.m[2][2] = farZ / (farZ - nearZ);
		projMat.m[2][3] = 1.0f;
		projMat.m[3][2] = -(nearZ * farZ) / (farZ - nearZ);

		Matrix4x4 viewProj = Matrix4x4::Multiply(viewMat, projMat);

		IDirect3DSurface9 *pCurRT = nullptr;
		D3DSURFACE_DESC rtDesc{};
		if (SUCCEEDED(dev->GetRenderTarget(0, &pCurRT)) && pCurRT) {
			pCurRT->GetDesc(&rtDesc);
		}
		// Left4Craft: in L4D2 the surface bound at reshade_begin_effects isn't the one the effects (and the
		// screen) use, so blocks drawn there never showed while the overlay did. Draw into ReShade's target.
		bool reboundRT = false;
		if (target && target != pCurRT && SUCCEEDED(dev->SetRenderTarget(0, target))) {
			reboundRT = true;
			target->GetDesc(&rtDesc);
			static bool s_logged = false;
			if (!s_logged) {
				s_logged = true;
				char m[160];
				snprintf(m, sizeof(m), "[Left4Craft] drawing the world into ReShade's target %p (device had %p bound)", (void*)target, (void*)pCurRT);
				reshade::log::message(reshade::log::level::info, m);
			}
		}

		IDirect3DSurface9 *pCurDS = nullptr;
		D3DSURFACE_DESC curDsDesc{};
		if (SUCCEEDED(dev->GetDepthStencilSurface(&pCurDS)) && pCurDS) {
			pCurDS->GetDesc(&curDsDesc);
		}

		D3DSURFACE_DESC capDsDesc{};
		if (m_capturedDS) {
			m_capturedDS->GetDesc(&capDsDesc);
		}

		bool reboundDS = false;
		HRESULT hrSetDS = S_OK;
		if (m_capturedDS != nullptr && m_capturedDS != pCurDS) {
			hrSetDS = dev->SetDepthStencilSurface(m_capturedDS);
			if (SUCCEEDED(hrSetDS)) {
				reboundDS = true;
			}
		}

		IDirect3DStateBlock9 *stateBlock = nullptr;
		if (SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &stateBlock)) && stateBlock) {
			stateBlock->Capture();
		}

		bool needEndScene = SUCCEEDED(dev->BeginScene());

		D3DVIEWPORT9 renderVp{};
		renderVp.X = 0;
		renderVp.Y = 0;
		renderVp.Width = rtDesc.Width ? rtDesc.Width : (origVp.Width ? origVp.Width : 1920);
		renderVp.Height = rtDesc.Height ? rtDesc.Height : (origVp.Height ? origVp.Height : 1080);
		renderVp.MinZ = 0.0f;
		renderVp.MaxZ = 1.0f;
		dev->SetViewport(&renderVp);

		dev->SetVertexDeclaration(m_vdecl);
		dev->SetVertexShader(m_vs);
		dev->SetPixelShader(m_ps);
		dev->SetTexture(0, m_atlasTex ? m_atlasTex : m_fallbackTex);

		dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

		dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0000000F);
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
		dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
		dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		float depthBias = -0.00005f;
		dev->SetRenderState(D3DRS_DEPTHBIAS, *(const DWORD*)&depthBias);
		float slopeBias = 0.0f;
		dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *(const DWORD*)&slopeBias);
		dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);

		// L4D2's own view-projection (captured c8..c11): uploaded as is, the shader's dot(c1, p) is Source's
		// column-major mul(p, cViewProj). Section offsets are absolute world positions on this path.
		float w2s[16]{};
		std::memcpy(w2s, gameViewProj, sizeof(w2s));
		bool hasW2S = true;
		if (hasW2S) {
			dev->SetVertexShaderConstantF(1, w2s, 4);
		} else {
			// viewProj is built for row vectors (p * V * P); the vertex shader takes rows of a column-vector matrix
			// (clip.x = dot(c1, p), like Source's WorldToScreenMatrix), so upload the transpose. Left4Craft: this
			// path was never used in HL2 (its engine matrix was always there) and drew off-screen as written.
			float fallbackMat[16];
			for (int r = 0; r < 4; ++r) {
				for (int c = 0; c < 4; ++c) {
					fallbackMat[r * 4 + c] = viewProj.m[c][r];
				}
			}
			dev->SetVertexShaderConstantF(1, fallbackMat, 4);
		}

		uint32_t drawnTris = 0;
		constexpr float kBlockUnits = 39.37007874f;

		// Read dynamic world entities (items, arrows, tridents, block cracks, selection outline)
		m_entities.count = 0;
		m_entities.hasSelection = 0;
		if (shm) {
			const auto *we = reinterpret_cast<const proto::WorldEntities*>(shm + proto::kOffWorldEntities);
			for (int attempt = 0; attempt < 3; ++attempt) {
				uint32_t seq1 = we->seq;
				if (seq1 & 1) continue;
				std::memcpy(&m_entities, we, sizeof(proto::WorldEntities));
				uint32_t seq2 = we->seq;
				if (seq1 == seq2) break;
			}
		}

		double camMc[3] = {
			camPos.x * proto::kMetersPerUnit,
			(camPos.z * proto::kMetersPerUnit) + 64.0,
			-camPos.y * proto::kMetersPerUnit
		};
		double o[3] = { std::floor(camMc[0]), std::floor(camMc[1]), std::floor(camMc[2]) };

		m_dynVerts.clear();
		m_crackVerts.clear();
		m_outlineVerts.clear();
		m_uploadVerts.clear();

		BuildEntities(o, camPos);
		uint32_t entityVertCount = (uint32_t)m_dynVerts.size();
		uint32_t crackVertCount = (uint32_t)m_crackVerts.size();
		BuildOutline(o, camPos);
		uint32_t outlineVertCount = (uint32_t)m_outlineVerts.size();

		m_uploadVerts.reserve(entityVertCount + crackVertCount + outlineVertCount);
		m_uploadVerts.insert(m_uploadVerts.end(), m_dynVerts.begin(), m_dynVerts.end());
		m_uploadVerts.insert(m_uploadVerts.end(), m_crackVerts.begin(), m_crackVerts.end());
		m_uploadVerts.insert(m_uploadVerts.end(), m_outlineVerts.begin(), m_outlineVerts.end());

		bool haveDyn = UploadDynamic(dev);

		float worldDynX = (float)(o[0] * kBlockUnits);
		float worldDynY = (float)(-o[2] * kBlockUnits);
		float worldDynZ = (float)((o[1] - 64.0) * kBlockUnits);

		float dynOff[4] = {
			hasW2S ? worldDynX : (worldDynX - camPos.x),
			hasW2S ? worldDynY : (worldDynY - camPos.y),
			hasW2S ? worldDynZ : (worldDynZ - camPos.z),
			1.0f
		};

		// Pass 1: Opaque & Cutout
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		float alphaCutOpaque[4] = { 0.1f, 0.0f, 0.0f, 0.0f };
		dev->SetPixelShaderConstantF(0, alphaCutOpaque, 1);

		for (auto &pair : m_sections) {
			const SectionMesh &sec = pair.second;
			if (sec.opaque == 0 || !sec.vb || sec.sx == 0x7FFFFFFF) continue;

			float worldSecX = (sec.sx * 16.0f) * kBlockUnits;
			float worldSecY = -(sec.sz * 16.0f) * kBlockUnits;
			float worldSecZ = (sec.sy * 16.0f - 64.0f) * kBlockUnits;

			// Quick distance cull (128 blocks ~ 5000 units)
			float dx = worldSecX - camPos.x;
			float dy = worldSecY - camPos.y;
			float dz = worldSecZ - camPos.z;
			if (dx * dx + dy * dy + dz * dz > 5500.0f * 5500.0f) continue;

			float offset[4] = {
				hasW2S ? worldSecX : dx,
				hasW2S ? worldSecY : dy,
				hasW2S ? worldSecZ : dz,
				1.0f
			};
			dev->SetVertexShaderConstantF(0, offset, 1);
			dev->SetStreamSource(0, sec.vb, 0, sizeof(proto::RenVertex));
			dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, sec.opaque / 3);
			drawnTris += sec.opaque / 3;
		}

		if (haveDyn && entityVertCount > 0) {
			dev->SetVertexShaderConstantF(0, dynOff, 1);
			dev->SetStreamSource(0, m_dynVb, 0, sizeof(proto::RenVertex));
			dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, entityVertCount / 3);
			drawnTris += entityVertCount / 3;
		}

		if (m_sceneVb && !m_sceneBatches.empty() && m_sceneVertCount > 0) {
			float sceneWorldX = (float)(m_sceneOriginX * kBlockUnits);
			float sceneWorldY = (float)(-m_sceneOriginZ * kBlockUnits);
			float sceneWorldZ = (float)((m_sceneOriginY - 64.0) * kBlockUnits);
			float dx = sceneWorldX - camPos.x;
			float dy = sceneWorldY - camPos.y;
			float dz = sceneWorldZ - camPos.z;
			float sceneOffset[4] = {
				hasW2S ? sceneWorldX : dx,
				hasW2S ? sceneWorldY : dy,
				hasW2S ? sceneWorldZ : dz,
				1.0f
			};
			dev->SetVertexShaderConstantF(0, sceneOffset, 1);
			dev->SetStreamSource(0, m_sceneVb, 0, sizeof(proto::RenVertex));
			for (const auto &b : m_sceneBatches) {
				if ((b.flags & 1) != 0) continue;
				IDirect3DTexture9 *tex = m_atlasTex ? m_atlasTex : m_fallbackTex;
				if (b.texture != 0) {
					tex = m_fallbackTex ? m_fallbackTex : m_atlasTex;
					auto it = m_entityTextures.find(b.texture);
					if (it != m_entityTextures.end() && it->second) {
						tex = it->second;
					}
				}
				dev->SetTexture(0, tex);
				dev->DrawPrimitive(D3DPT_TRIANGLELIST, b.first, b.count / 3);
				drawnTris += b.count / 3;
			}
			dev->SetTexture(0, m_atlasTex ? m_atlasTex : m_fallbackTex);
		}

		// Pass 2: Translucent (water, stained glass, cracks, selection outline)
		dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		float alphaCutTrans[4] = { 0.003f, 0.0f, 0.0f, 0.0f };
		dev->SetPixelShaderConstantF(0, alphaCutTrans, 1);

		for (auto &pair : m_sections) {
			const SectionMesh &sec = pair.second;
			if (sec.translucent == 0 || !sec.vb || sec.sx == 0x7FFFFFFF) continue;

			float worldSecX = (sec.sx * 16.0f) * kBlockUnits;
			float worldSecY = -(sec.sz * 16.0f) * kBlockUnits;
			float worldSecZ = (sec.sy * 16.0f - 64.0f) * kBlockUnits;

			float dx = worldSecX - camPos.x;
			float dy = worldSecY - camPos.y;
			float dz = worldSecZ - camPos.z;
			if (dx * dx + dy * dy + dz * dz > 5500.0f * 5500.0f) continue;

			float offset[4] = {
				hasW2S ? worldSecX : dx,
				hasW2S ? worldSecY : dy,
				hasW2S ? worldSecZ : dz,
				1.0f
			};
			dev->SetVertexShaderConstantF(0, offset, 1);
			dev->SetStreamSource(0, sec.vb, 0, sizeof(proto::RenVertex));
			dev->DrawPrimitive(D3DPT_TRIANGLELIST, sec.opaque, sec.translucent / 3);
			drawnTris += sec.translucent / 3;
		}

		if (m_sceneVb && !m_sceneBatches.empty() && m_sceneVertCount > 0) {
			float sceneWorldX = (float)(m_sceneOriginX * kBlockUnits);
			float sceneWorldY = (float)(-m_sceneOriginZ * kBlockUnits);
			float sceneWorldZ = (float)((m_sceneOriginY - 64.0) * kBlockUnits);
			float dx = sceneWorldX - camPos.x;
			float dy = sceneWorldY - camPos.y;
			float dz = sceneWorldZ - camPos.z;
			float sceneOffset[4] = {
				hasW2S ? sceneWorldX : dx,
				hasW2S ? sceneWorldY : dy,
				hasW2S ? sceneWorldZ : dz,
				1.0f
			};
			dev->SetVertexShaderConstantF(0, sceneOffset, 1);
			dev->SetStreamSource(0, m_sceneVb, 0, sizeof(proto::RenVertex));
			for (const auto &b : m_sceneBatches) {
				if ((b.flags & 1) == 0) continue;
				IDirect3DTexture9 *tex = m_atlasTex ? m_atlasTex : m_fallbackTex;
				if (b.texture != 0) {
					tex = m_fallbackTex ? m_fallbackTex : m_atlasTex;
					auto it = m_entityTextures.find(b.texture);
					if (it != m_entityTextures.end() && it->second) {
						tex = it->second;
					}
				}
				dev->SetTexture(0, tex);
				dev->DrawPrimitive(D3DPT_TRIANGLELIST, b.first, b.count / 3);
				drawnTris += b.count / 3;
			}
			dev->SetTexture(0, m_atlasTex ? m_atlasTex : m_fallbackTex);
		}

		if (haveDyn && crackVertCount > 0) {
			dev->SetVertexShaderConstantF(0, dynOff, 1);
			dev->SetStreamSource(0, m_dynVb, 0, sizeof(proto::RenVertex));
			dev->DrawPrimitive(D3DPT_TRIANGLELIST, entityVertCount, crackVertCount / 3);
			drawnTris += crackVertCount / 3;
		}

		if (haveDyn && outlineVertCount > 0) {
			float alphaOutline[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
			dev->SetPixelShaderConstantF(0, alphaOutline, 1);
			dev->SetVertexShaderConstantF(0, dynOff, 1);
			dev->SetStreamSource(0, m_dynVb, 0, sizeof(proto::RenVertex));
			dev->DrawPrimitive(D3DPT_LINELIST, entityVertCount + crackVertCount, outlineVertCount / 2);
		}

		static DWORD s_lastLog = 0;
		DWORD now = GetTickCount();
		bool f8Pressed = (GetAsyncKeyState(VK_F8) & 1) != 0;
		if (now - s_lastLog > 2000 || f8Pressed) {
			s_lastLog = now;
			char buf[1024];
			float testNdc[4]{0};
			if (hasW2S) {
				Vector testP = { camPos.x + fwd.x * 100.0f, camPos.y + fwd.y * 100.0f, camPos.z + fwd.z * 100.0f };
				float tw = w2s[12]*testP.x + w2s[13]*testP.y + w2s[14]*testP.z + w2s[15];
				float invW = (tw > 0.001f) ? (1.0f / tw) : 1.0f;
				testNdc[0] = (w2s[0]*testP.x + w2s[1]*testP.y + w2s[2]*testP.z + w2s[3]) * invW;
				testNdc[1] = (w2s[4]*testP.x + w2s[5]*testP.y + w2s[6]*testP.z + w2s[7]) * invW;
				testNdc[2] = (w2s[8]*testP.x + w2s[9]*testP.y + w2s[10]*testP.z + w2s[11]) * invW;
				testNdc[3] = tw;
			}
			snprintf(buf, sizeof(buf),
				"[HL2Craft Depth] RT: %ux%u (fmt=%d) | CurDS: %ux%u (fmt=%d %p) | CapDS: %ux%u (fmt=%d %p) | Rebound: %d (hr=0x%08X) | Tris: %u | W2S: %s (ndc=%.2f,%.2f,%.2f w=%.1f) | Candidates: %s",
				rtDesc.Width, rtDesc.Height, (int)rtDesc.Format,
				curDsDesc.Width, curDsDesc.Height, (int)curDsDesc.Format, (void*)pCurDS,
				capDsDesc.Width, capDsDesc.Height, (int)capDsDesc.Format, (void*)m_capturedDS,
				reboundDS ? 1 : 0, (unsigned int)hrSetDS, drawnTris,
				hasW2S ? "OK" : "NONE",
				testNdc[0], testNdc[1], testNdc[2], testNdc[3],
				m_depthCandidatesDiag.c_str());
			reshade::log::message(reshade::log::level::info, buf);

			char conCmd[256];
			snprintf(conCmd, sizeof(conCmd),
				"echo [HL2Craft Depth] DS: %p (fmt=%d) | Tris: %u | W2S: %s (z=%.2f) | %s",
				(void*)m_capturedDS, (int)capDsDesc.Format, drawnTris,
				hasW2S ? "OK" : "NONE", testNdc[2],
				m_depthSummaryDiag.c_str());
			HL2Bridge::Get().ExecCmd(conCmd);
		}

		if (needEndScene) {
			dev->EndScene();
		}

		if (reboundRT) {
			dev->SetRenderTarget(0, pCurRT); // resets the viewport, so before restoring it
		}
		dev->SetViewport(&origVp);

		if (reboundDS) {
			dev->SetDepthStencilSurface(pCurDS);
		}
		if (pCurDS) {
			pCurDS->Release();
			pCurDS = nullptr;
		}
		if (pCurRT) {
			pCurRT->Release();
			pCurRT = nullptr;
		}

		if (stateBlock) {
			stateBlock->Apply();
			stateBlock->Release();
		}
	}
}
