#pragma once
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <vector>
#include <unordered_set>
#include <set>
#include <tuple>
#include <cstring>
#include "../../protocol/hl2craft_protocol.h"

namespace hl2craft
{
	typedef void* (*CreateInterfaceFn)(const char *pName, int *pReturnCode);

	struct Vector { float x, y, z; };
	// 16-byte aligned like the SDK's VectorAligned: the engine may load Ray_t members with aligned SSE moves
	struct alignas(16) VectorAligned : public Vector { float w; };
	struct QAngle { float pitch, yaw, roll; };

	struct Ray_t {
		VectorAligned m_Start;
		VectorAligned m_Delta;
		VectorAligned m_StartOffset;
		VectorAligned m_Extents;
		const void *m_pWorldAxisTransform; // L4D2 (hl2sdk l4d2 cmodel.h): before the two flags
		bool m_IsRay;
		bool m_IsSwept;

		void Init(const Vector &start, const Vector &end) {
			m_Delta.x = end.x - start.x;
			m_Delta.y = end.y - start.y;
			m_Delta.z = end.z - start.z;
			m_Delta.w = 0;
			m_IsSwept = (m_Delta.x * m_Delta.x + m_Delta.y * m_Delta.y + m_Delta.z * m_Delta.z != 0);
			m_Extents.x = m_Extents.y = m_Extents.z = m_Extents.w = 0;
			m_pWorldAxisTransform = nullptr;
			m_IsRay = true;
			m_StartOffset.x = m_StartOffset.y = m_StartOffset.z = m_StartOffset.w = 0;
			m_Start.x = start.x; m_Start.y = start.y; m_Start.z = start.z; m_Start.w = 0;
		}

		void InitHull(const Vector &start, const Vector &end, const Vector &mins, const Vector &maxs) {
			m_Delta.x = end.x - start.x;
			m_Delta.y = end.y - start.y;
			m_Delta.z = end.z - start.z;
			m_Delta.w = 0;
			m_pWorldAxisTransform = nullptr;
			m_IsSwept = (m_Delta.x * m_Delta.x + m_Delta.y * m_Delta.y + m_Delta.z * m_Delta.z != 0);
			m_Extents.x = (maxs.x - mins.x) * 0.5f;
			m_Extents.y = (maxs.y - mins.y) * 0.5f;
			m_Extents.z = (maxs.z - mins.z) * 0.5f;
			m_Extents.w = 0;
			m_IsRay = (m_Extents.x * m_Extents.x + m_Extents.y * m_Extents.y + m_Extents.z * m_Extents.z < 1.0e-6f);
			m_StartOffset.x = (mins.x + maxs.x) * 0.5f;
			m_StartOffset.y = (mins.y + maxs.y) * 0.5f;
			m_StartOffset.z = (mins.z + maxs.z) * 0.5f;
			m_StartOffset.w = 0;
			m_Start.x = start.x + m_StartOffset.x;
			m_Start.y = start.y + m_StartOffset.y;
			m_Start.z = start.z + m_StartOffset.z;
			m_Start.w = 0;
			m_StartOffset.x = -m_StartOffset.x;
			m_StartOffset.y = -m_StartOffset.y;
			m_StartOffset.z = -m_StartOffset.z;
		}
	};

	struct cplane_t {
		Vector normal;
		float dist;
		unsigned char type;
		unsigned char signbits;
		unsigned char pad[2];
	};

	struct csurface_t {
		const char *name;
		short surfaceProps;
		unsigned short flags;
	};

	struct trace_t {
		Vector startpos;
		Vector endpos;
		cplane_t plane;
		float fraction;
		int contents;
		unsigned short dispFlags;
		bool allsolid;
		bool startsolid;
		float fractionleftsolid;
		csurface_t surface;
		int hitgroup;
		short physicsbone;
		unsigned short worldSurfaceIndex;
		void *m_pEnt;
		int hitbox;
		// Left4Craft: room for whatever L4D2's real CGameTrace has past the SDK layout. Without it, a trace in
		// UpdateHostState overwrote the neighbouring locals (host pose x = 3e279, y = NaN, z correct; 2026-10-19).
		unsigned char l4cGuard[256];
	};

	struct ColorRGBExp32 {
		uint8_t r, g, b;
		int8_t exponent;
	};

	struct dlight_t {
		int flags;
		Vector origin;
		float radius;
		ColorRGBExp32 color;
		float die;
		float decay;
		float minlight;
		int key;
		int style;
		Vector m_Direction;
		float m_InnerAngle;
		float m_OuterAngle;
	};

	class ITraceFilter {
	public:
		virtual bool ShouldHitEntity(void *pEntity, int contentsMask) = 0;
		virtual int GetTraceType() const = 0;
	};

	class CTraceFilterWorldOnly : public ITraceFilter {
	public:
		virtual bool ShouldHitEntity(void *pEntity, int contentsMask) { return false; }
		virtual int GetTraceType() const { return 1; } // TRACE_WORLD_ONLY = 1
	};

	// Left 4 Dead 2 (build 23990068) layout, from alliedmodders/hl2sdk branch l4d2.
	namespace l4d2
	{
		// IVEngineClient (VEngineClient013)
		constexpr int kClientCmd = 7;
		// ClientCmd [7] filters what it runs: setpos passed, script_execute and cl_drawhud were dropped. [107] adds
		// to the command buffer unfiltered, tagged cdll_engine_int.cpp (ClientCmd_Unrestricted; SDK index 99, the
		// real table is 8 longer by then), [103] does the same and runs the buffer at once (ExecuteClientCmd).
		constexpr int kClientCmdUnrestricted = 107;
		constexpr int kConIsVisible = 11;
		constexpr int kGetLocalPlayer = 12;
		constexpr int kTime = 14;
		constexpr int kGetViewAngles = 19;
		constexpr int kIsInGame = 26;
		constexpr int kIsConnected = 27;
		constexpr int kIsDrawingLoadingImage = 28;
		constexpr int kWorldToScreenMatrix = 36;
		// Checked in L4D2's engine.dll (build 23990068), not just the SDK header: the real vtable has one entry more
		// than hl2sdk-l4d2 somewhere between 37 and 52. [52] returns "Dedicated Server" / "" / the level name
		// (GetLevelName), [53] the short name. Index 51 (the header's GetLevelName) crashed the game in the menu.
		constexpr int kGetLevelName = 52;
		// IsPaused / IsLevelMainMenuBackground could not be identified with certainty past that shift, so they are
		// not called: a co-op listen server doesn't pause on Esc, and L4D2's menu background is a video, not a map.
		// IEngineTrace (EngineTraceClient003): GetPointContents(pos, contentsMask, ppEntity), TraceRay is 5 (4 in SDK 2013)
		constexpr int kGetPointContents = 0;
		constexpr int kTraceRay = 5;
		constexpr int kMaskAll = -1;
		// The survivor's eye height standing / crouched (Source units)
		constexpr float kEyeStand = 64.0f;
		constexpr float kEyeDuck = 46.0f;
		// IClientNetworkable as compiled into L4D2's client.dll (checked in IDA, 2026-10-19): one entry fewer than
		// hl2sdk-l4d2 before GetClientClass. [1] returns the static ClientClass, [2] NotifyShouldTransmit pops one
		// argument, [7] IsDormant checks the index at +0x50, [8] entindex returns it. Calling the SDK's [2] as
		// GetClientClass() left 4 bytes popped per call, shifting esp, so later outgoing arguments overwrote this
		// add-on's locals (host pose 3e279 / NaN, the crashes on map load).
		constexpr int kNetGetClientClass = 1;
		constexpr int kNetIsDormant = 7;
		constexpr int kNetEntIndex = 8;
		// L4D2's default fov_desired is 90 (horizontal at 4:3)
		constexpr float kHorizontalFov = 90.0f;

		/// Offset of a networked variable (m_iHealth, m_lifeState, m_fFlags, m_clrRender, m_hActiveWeapon...) in this
		/// client entity, found by name in its ClientClass's RecvTables and cached per class. -1 if it has none.
		int NetvarOffset(void *clientEntity, const char *name);
		/// Pointer to that variable, or nullptr.
		template <typename T> T *Netvar(void *clientEntity, const char *name)
		{
			int off = NetvarOffset(clientEntity, name);
			return off >= 0 ? (T*)((char*)clientEntity + off) : nullptr;
		}
		/// The entity's network class name ("Infected", "Witch", "CTerrorPlayer"...), or "".
		const char *ClassName(void *clientEntity);
	}

	/// The ClientClass's network name if it is readable and looks like one, else "".
	const char *SafeClassName(void *clientClass);

	class HL2Bridge
	{
	public:
		static HL2Bridge& Get();

		void Init();
		void Tick();
		void Shutdown();

		bool IsInGame();
		bool IsConnected();
		bool IsPaused();
		bool IsLevelMainMenuBackground();
		bool IsDrawingLoadingImage();
		bool HasPlayer();
		bool IsInPlayableWorld();

		bool ExecCmd(const char *cmd);
		bool GetAngles(QAngle &angles);
		bool GetPlayerPos(Vector &pos);
		void SetGuiOpen(bool open);
		bool IsGuiOpen() const { return m_guiOpen; }
		bool GetCursorState(int &x, int &y) const { x = m_cursorX; y = m_cursorY; return m_guiOpen; }

		HANDLE GetSharedMemoryHandle() const { return m_shmHandle; }
		uint8_t* GetSharedMemoryView() const { return m_shmView; }

		void PushInput(uint16_t type, uint16_t code, int32_t a, int32_t b = 0, int32_t c = 0);
		void PushTextInput(int32_t codepoint);
		static LRESULT CALLBACK SubclassWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
		void HookWindow();
		void UnhookWindow();
		void HitPicker(int extraDmg = 20);
		void HitCurrentTarget(int dmg = 35);
		void* GetLocalPlayerEntity();
		bool TraceWorldRay(const Vector &start, const Vector &end, trace_t &tr);
		bool IsNetworkableAt(int index, void *ent);
		bool IsHandleEntityClient(void *handleEntity);
		bool GetWorldToScreenMatrix(float outMat[16]);

	private:
		HL2Bridge();
		~HL2Bridge();

		void InitSharedMemory();
		void UpdateHostState(const Vector &pos, const QAngle &angles);
		void ScanCollision(const Vector &playerPos, int radius = 26);
		void ScanActors();
		void ProcessEvents();
		void ProcessInput();
		void ComputeUnstickPos(const Vector &playerPos, int ibx, int iby, int ibz, Vector &outPos);
		void StartUnstickLift(const Vector &targetPos, int ibx, int iby, int ibz);
		void StartNpcKnockback(int entIndex, const Vector &entPos, const Vector &dir, float strength, bool isCrit, int applyDmg = 35);
		void UpdateNpcKnockbacks();
		void PollVScript();
		bool MovementKeysToMinecraft();

		struct ActiveKnockback {
			int entIndex;
			Vector lastPos;
			DWORD startMs;
			DWORD flashEndMs;
			bool isCrit;
		};
		std::vector<ActiveKnockback> m_knockbacks;
		int m_lastHitEntIndex = -1;
		DWORD m_lastHitTimeMs = 0;
		int m_lastHitAppliedDmg = 0;
		double m_lastMcY = 64.0;
		DWORD m_lastFallingUntilMs = 0;

		bool m_initialized = false;
		void *m_engineClient = nullptr;
		void *m_engineTrace = nullptr;
		void *m_entityList = nullptr;
		void *m_vguiSurface = nullptr;
		void *m_engineVGui = nullptr;
		void *m_cvar = nullptr;

		HANDLE m_shmHandle = nullptr;
		uint8_t *m_shmView = nullptr;

		bool m_wasInWorld = false;
		bool m_lastPosValid = false;
		float m_lastPosX = 0, m_lastPosY = 0, m_lastPosZ = 0;
		bool m_guiOpen = false;
		bool m_creative = false;
		uint32_t m_collisionEpoch = 1;
		uint32_t m_teleportSeq = 0;
		char m_lastMapName[64] = {0};
		DWORD m_lastStripTime = 0;
		DWORD m_lastColScanTime = 0;
		DWORD m_lastFastScanTime = 0;
		DWORD m_lastActorScanTime = 0;
		DWORD m_lastVScriptPoll = 0;
		DWORD m_lastPuppetMs = 0;
		DWORD m_lastVScriptCheck = 0;
		DWORD m_lastVScriptAliveMs = 0;
		DWORD m_lastSpacePress = 0;
		bool m_lastSpaceKey = false;
		bool m_lastLButton = false;
		bool m_lastRButton = false;
		int m_lastMouseX = -1, m_lastMouseY = -1;
		bool m_guiLBtn = false, m_guiRBtn = false;
		Vector m_lastScanPos{0, 0, 0};
		Vector m_lastFastScanPos{0, 0, 0};
		int m_lastCamMode = 0;
		float m_lastCamDist = 0.0f;
		bool m_keysDown[256] = {false};
		bool m_lastSprinting = false;
		bool m_mcWasDead = false;
		int m_lastSyncedHp = -1;
		int m_lastPlayerHl2Hp = 100;
		int m_cursorX = 960;
		int m_cursorY = 540;
		int m_viewW = 1920;
		int m_viewH = 1080;
		HWND m_hValveWnd = nullptr;
		WNDPROC m_oldWndProc = nullptr;
		struct PlacedLight {
			int bx, by, bz;
			int lightLevel;
			int lightType; // 1: warm, 2: soul
			bool isLava;
		};
		std::vector<PlacedLight> m_placedLights;

		std::set<std::tuple<int, int, int>> m_placedBlocks;
		DWORD m_lastBlockSyncTime = 0;
		bool m_wasLoading = false;
		float m_lastGameTime = 0.0f;
		bool m_wasFlying = false;
		DWORD m_ignoreSelfDamageUntil = 0;
		bool m_unstickActive = false;
		DWORD m_unstickUntil = 0;
		Vector m_unstickPos{0, 0, 0};
		int m_unstickBx = 0, m_unstickBy = 0, m_unstickBz = 0;
		DWORD m_hl2OverrideUntil = 0;
		bool m_mcPhysicsActive = false;
		bool m_hasLastSentMcPos = false;
		Vector m_lastSentMcPos{0, 0, 0};
	};
}
