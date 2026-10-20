#include "hl2_bridge.h"
#include "d3d9_render.h"
#include <cstring>
#include <setjmp.h>
#include <cstdarg>

namespace
{
	static jmp_buf g_traceJmp;
	static volatile bool g_inTrace = false;
	static volatile DWORD g_traceThread = 0;

	// Only a fault inside engine.dll's own code on the scanning thread aborts the scan. A vectored handler sees every
	// first-chance exception in the process first: IsBadReadPtr probes (raised in kernelbase), other threads'
	// handled exceptions... HL2Craft longjmp'd out of all of them, skipping half-run frames of our code and the
	// engine's, which in L4D2 corrupted the stack (host pose x = 3e279 / NaN, crashes reading `pos`; 2026-10-19).
	static bool InEngineCode(const void *address)
	{
		static uintptr_t s_base = 0, s_end = 0;
		if (!s_base) {
			HMODULE engine = GetModuleHandleA("engine.dll");
			if (!engine) return false;
			auto *dos = (IMAGE_DOS_HEADER*)engine;
			auto *nt = (IMAGE_NT_HEADERS32*)((uint8_t*)engine + dos->e_lfanew);
			s_base = (uintptr_t)engine;
			s_end = s_base + nt->OptionalHeader.SizeOfImage;
		}
		uintptr_t a = (uintptr_t)address;
		return a >= s_base && a < s_end;
	}

	static LONG WINAPI TraceVehHandler(PEXCEPTION_POINTERS info)
	{
		if (g_inTrace && GetCurrentThreadId() == g_traceThread && info && info->ExceptionRecord &&
		    info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
		    InEngineCode(info->ExceptionRecord->ExceptionAddress)) {
			g_inTrace = false;
			longjmp(g_traceJmp, 1);
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

namespace hl2craft
{
	// Left4Craft: L4D2's entity list holds objects whose "+8 networkable" isn't one, so the ClientClass read through
	// it can be garbage whose name pointer faults (crash in IsVehicleClass on map load, 2026-10-19). Only a readable,
	// identifier-like name is used; anything else counts as "" (skipped).
	// left4craft_addon.log next to left4dead2.exe: what the bridge saw, for checking a run afterwards
	void DiagLog(const char *fmt, ...)
	{
		static FILE *f = nullptr;
		if (!f) {
			char exe[MAX_PATH];
			GetModuleFileNameA(nullptr, exe, MAX_PATH);
			char *slash = strrchr(exe, '\\');
			if (slash) *slash = 0;
			char path[MAX_PATH];
			snprintf(path, sizeof(path), "%s\\left4craft_addon.log", exe);
			f = fopen(path, "w");
			if (!f) return;
		}
		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list ap;
		va_start(ap, fmt);
		vfprintf(f, fmt, ap);
		va_end(ap);
		fputc('\n', f);
		fflush(f);
	}

	const char *SafeClassName(void *clientClass)
	{
		if (!clientClass || IsBadReadPtr(clientClass, 12)) return "";
		const char *name = *(const char**)((char*)clientClass + 8);
		if (!name || IsBadReadPtr(name, 2)) return "";
		for (int i = 0; i < 64; ++i) {
			char c = name[i];
			if (c == 0) return i > 0 ? name : "";
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return "";
			if (IsBadReadPtr(name + i + 1, 1)) return "";
		}
		return "";
	}

	HL2Bridge& HL2Bridge::Get()
	{
		static HL2Bridge instance;
		return instance;
	}

	HL2Bridge::HL2Bridge() {}

	HL2Bridge::~HL2Bridge()
	{
		Shutdown();
	}

	static void PatchJumpHeightModule(HMODULE hMod, float targetHeight)
	{
		if (!hMod) return;
		auto *dos = (IMAGE_DOS_HEADER*)hMod;
		if (IsBadReadPtr(dos, sizeof(IMAGE_DOS_HEADER)) || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
		auto *nt = (IMAGE_NT_HEADERS32*)((uint8_t*)hMod + dos->e_lfanew);
		if (IsBadReadPtr(nt, sizeof(IMAGE_NT_HEADERS32)) || nt->Signature != IMAGE_NT_SIGNATURE) return;
		uint8_t *base = (uint8_t*)hMod;
		uint32_t size = nt->OptionalHeader.SizeOfImage;
		for (uint32_t i = 0x1000; i + 32 < size; ++i) {
			if ((base[i] == 0x74 || base[i] == 0xEB) && base[i+1] == 0x0A &&
			    base[i+2] == 0xF3 && base[i+3] == 0x0F && base[i+4] == 0x10 && base[i+5] == 0x05 &&
			    base[i+10] == 0xEB && base[i+11] == 0x17 && base[i+12] == 0xE8 &&
			    base[i+17] == 0xDC && base[i+18] == 0xC0 && base[i+19] == 0xD8 && base[i+20] == 0x0D) {
				DWORD oldProt = 0;
				if (VirtualProtect(base + i, 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
					base[i] = 0xEB; // Always use sqrt(2 * gravity * jumpHeight)
					VirtualProtect(base + i, 2, oldProt, &oldProt);
				}
				float *pJumpHeight = *(float**)(base + i + 21);
				if (pJumpHeight && !IsBadReadPtr(pJumpHeight, sizeof(float))) {
					DWORD oldDataProt = 0;
					if (VirtualProtect(pJumpHeight, sizeof(float), PAGE_READWRITE, &oldDataProt)) {
						*pJumpHeight = targetHeight;
						VirtualProtect(pJumpHeight, sizeof(float), oldDataProt, &oldDataProt);
					}
				}
			}
		}
	}

	static void PatchMinecraftJumpHeight()
	{
		// 52.0 Source units (~1.252 Minecraft blocks actual peak height after first-tick FinishGravity)
		constexpr float kMcJumpHeightUnits = 52.0f;
		// HL2-only byte pattern: never scanned in L4D2's binaries (Minecraft owns the jump there anyway)
		(void)kMcJumpHeightUnits;
	}

	void HL2Bridge::Init()
	{
		if (m_initialized) return;

		static bool s_vehInstalled = false;
		if (!s_vehInstalled) {
			AddVectoredExceptionHandler(1, TraceVehHandler);
			s_vehInstalled = true;
		}

		InitSharedMemory();

		HMODULE hEngine = GetModuleHandleA("engine.dll");
		HMODULE hClient = GetModuleHandleA("client.dll");
		if (!hEngine || !hClient) return;

		CreateInterfaceFn engineFactory = (CreateInterfaceFn)GetProcAddress(hEngine, "CreateInterface");
		CreateInterfaceFn clientFactory = (CreateInterfaceFn)GetProcAddress(hClient, "CreateInterface");
		if (!engineFactory || !clientFactory) return;

		m_engineClient = engineFactory("VEngineClient014", nullptr);
		if (!m_engineClient)
			m_engineClient = engineFactory("VEngineClient013", nullptr);

		m_engineTrace = engineFactory("EngineTraceClient003", nullptr);
		m_entityList = clientFactory("VClientEntityList003", nullptr);
		m_engineVGui = engineFactory("VEngineVGui001", nullptr);

		HMODULE hVgui = GetModuleHandleA("vguimatsurface.dll");
		if (!hVgui) hVgui = LoadLibraryA("bin\\vguimatsurface.dll");
		if (hVgui) {
			CreateInterfaceFn vguiFactory = (CreateInterfaceFn)GetProcAddress(hVgui, "CreateInterface");
			if (vguiFactory) {
				m_vguiSurface = vguiFactory("VGUI_Surface030", nullptr);
			}
		}

		HMODULE hVstdlib = GetModuleHandleA("vstdlib.dll");
		if (!hVstdlib) hVstdlib = LoadLibraryA("bin\\vstdlib.dll");
		if (hVstdlib) {
			CreateInterfaceFn vstdlibFactory = (CreateInterfaceFn)GetProcAddress(hVstdlib, "CreateInterface");
			if (vstdlibFactory) {
				m_cvar = vstdlibFactory("VEngineCvar004", nullptr);
				if (!m_cvar) m_cvar = vstdlibFactory("VEngineCvar007", nullptr);
			}
		}
		if (!m_cvar && engineFactory) {
			m_cvar = engineFactory("VEngineCvar004", nullptr);
			if (!m_cvar) m_cvar = engineFactory("VEngineCvar007", nullptr);
		}

		if (m_entityList && hClient) {
			void **lvt = *(void***)m_entityList;
			for (int k = 0; k < 8; ++k) {
				DiagLog("VClientEntityList003[%d] = client.dll+0x%X", k, (unsigned)((char*)lvt[k] - (char*)hClient));
			}
		}
		PatchMinecraftJumpHeight();
		ExecCmd("con_logfile left4craft_console.log; sv_cheats 1; echo [left4craft] unrestricted command path ok"); // L4D2: never rebind keys or change sv_gravity (they persist)

		InitSharedMemory();
		m_initialized = (m_engineClient != nullptr && m_entityList != nullptr && m_shmView != nullptr);
		HookWindow();
	}

	void HL2Bridge::Shutdown()
	{
		UnhookWindow();
		if (m_shmView) {
			UnmapViewOfFile(m_shmView);
			m_shmView = nullptr;
		}
		if (m_shmHandle) {
			CloseHandle(m_shmHandle);
			m_shmHandle = nullptr;
		}
		m_initialized = false;
	}

	void HL2Bridge::InitSharedMemory()
	{
		if (m_shmView) return;
		m_shmHandle = CreateFileMappingW(
			INVALID_HANDLE_VALUE,
			nullptr,
			PAGE_READWRITE,
			(DWORD)(proto::kMappingBytes >> 32),
			(DWORD)(proto::kMappingBytes & 0xFFFFFFFF),
			proto::kMappingName
		);
		if (!m_shmHandle) {
			m_shmHandle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, proto::kMappingName);
		}
		if (!m_shmHandle) return;

		m_shmView = (uint8_t*)MapViewOfFile(m_shmHandle, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)proto::kMappingBytes);
		if (!m_shmView) return;

		proto::Header *hdr = (proto::Header*)m_shmView;
		hdr->magic = proto::kMagic;
		hdr->version = proto::kVersion;
		hdr->hostPid = GetCurrentProcessId();
		hdr->hostHeartbeatMs = (uint64_t)GetTickCount();
	}

	bool HL2Bridge::IsInGame()
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef bool (__thiscall *Fn)(void*);
		return ((Fn)vt[26])(m_engineClient);
	}

	bool HL2Bridge::IsConnected()
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef bool (__thiscall *Fn)(void*);
		return ((Fn)vt[27])(m_engineClient);
	}

	bool HL2Bridge::IsPaused()
	{
		if (!m_engineClient) return false;
		return false; // see l4d2::kGetLevelName
	}

	bool HL2Bridge::IsLevelMainMenuBackground()
	{
		if (!m_engineClient) return false;
		return false; // see l4d2::kGetLevelName
	}

	bool HL2Bridge::IsDrawingLoadingImage()
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef bool (__thiscall *Fn)(void*);
		return ((Fn)vt[28])(m_engineClient);
	}

	bool HL2Bridge::HasPlayer()
	{
		Vector p{0, 0, 0};
		return GetPlayerPos(p);
	}

	bool HL2Bridge::IsInPlayableWorld()
	{
		if (!m_engineClient) return false;
		if (!IsInGame() || !IsConnected()) return false;
		if (IsLevelMainMenuBackground()) return false;
		if (IsDrawingLoadingImage()) return false;

		void **vt = *(void***)m_engineClient;
		typedef int (__thiscall *GetLocalPlayerFn)(void*);
		int localPlayer = ((GetLocalPlayerFn)vt[12])(m_engineClient);
		if (localPlayer <= 0) return false;

		return HasPlayer();
	}

	bool HL2Bridge::ExecCmd(const char *cmd)
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef void (__thiscall *ClientCmdFn)(void*, const char*);
		((ClientCmdFn)vt[l4d2::kClientCmdUnrestricted])(m_engineClient, cmd);
		return true;
	}

	bool HL2Bridge::GetAngles(QAngle &angles)
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef void (__thiscall *GetViewAnglesFn)(void*, QAngle&);
		((GetViewAnglesFn)vt[19])(m_engineClient, angles);
		return true;
	}

	void* HL2Bridge::GetLocalPlayerEntity()
	{
		if (!m_entityList || !m_engineClient) return nullptr;
		void **evt = *(void***)m_engineClient;
		typedef int (__thiscall *GetLocalPlayerFn)(void*);
		int localPlayer = ((GetLocalPlayerFn)evt[12])(m_engineClient);
		if (localPlayer <= 0) return nullptr;
		void **lvt = *(void***)m_entityList;
		typedef void* (__thiscall *GetEntityFn)(void*, int);
		return ((GetEntityFn)lvt[3])(m_entityList, localPlayer);
	}

	bool HL2Bridge::GetPlayerPos(Vector &pos)
	{
		if (!m_entityList || !IsInGame()) return false;
		void *player = GetLocalPlayerEntity();
		if (!player || IsBadReadPtr(player, sizeof(void*))) return false;

		void *renderable = (char*)player + 4;
		if (IsBadReadPtr(renderable, sizeof(void*))) return false;
		void **rendVt = *(void***)renderable;
		if (IsBadReadPtr(rendVt, sizeof(void*) * 2)) return false;

		typedef const float* (__thiscall *GetRenderOriginFn)(void*);
		const float *org = ((GetRenderOriginFn)rendVt[1])(renderable);
		if (org && !IsBadReadPtr(org, sizeof(float) * 3)) {
			// Source maps live inside +-16384 (+-32768 with margins): anything else isn't a position
			for (int i = 0; i < 3; ++i) {
				if (!std::isfinite(org[i]) || std::fabs(org[i]) > 32768.0f) return false;
			}
			pos.x = org[0]; pos.y = org[1]; pos.z = org[2];
			return true;
		}
		return false;
	}

	bool HL2Bridge::GetWorldToScreenMatrix(float outMat[16])
	{
		// Left4Craft: L4D2's WorldToScreenMatrix ([36] in engine.dll) is not the main view's matrix when this runs
		// (split-screen era engine): the renderer's diagnostics showed ndc ~1e10 and w ~1e24 for points in front of
		// the player. The renderer's own camera (player pose, angles, hs->fov) is used instead.
		(void)outMat;
		return false;
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		if (!vt || IsBadReadPtr(vt, sizeof(void*) * 38)) return false;
		void *fnPtr = vt[36];
		if (!fnPtr || IsBadCodePtr((FARPROC)fnPtr)) return false;

		typedef const void* (__thiscall *GetW2SFn)(void*);
		const void *pMat = ((GetW2SFn)fnPtr)(m_engineClient);
		if (!pMat || IsBadReadPtr(pMat, sizeof(float) * 16)) return false;

		std::memcpy(outMat, pMat, sizeof(float) * 16);
		return true;
	}

	void HL2Bridge::SetGuiOpen(bool open)
	{
		if (open != m_guiOpen) {
			m_guiOpen = open;
			if (m_guiOpen) {
				POINT pt;
				if (GetCursorPos(&pt) && m_hValveWnd) {
					ScreenToClient(m_hValveWnd, &pt);
					m_cursorX = std::clamp((int)pt.x, 0, m_viewW - 1);
					m_cursorY = std::clamp((int)pt.y, 0, m_viewH - 1);
				} else {
					m_cursorX = m_viewW / 2;
					m_cursorY = m_viewH / 2;
				}
				PushInput(proto::kInCursor, 0, m_cursorX, m_cursorY);
				ExecCmd("cl_mouseenable 0; -forward; -back; -moveleft; -moveright; -jump; -duck; -speed; -attack; -attack2;");
				ShowCursor(TRUE);
			} else {
				ExecCmd("cl_mouseenable 1;");
				ShowCursor(FALSE);
				PushInput(proto::kInReleaseAll, 0, 0, 0);
			}
		}
	}

	void HL2Bridge::Tick()
	{
		if (!m_initialized) {
			Init();
			if (!m_initialized) return;
		}

		proto::Header *hdr = (proto::Header*)m_shmView;
		if (hdr) {
			hdr->hostHeartbeatMs = (uint64_t)GetTickCount();
		}

		char currentMap[64] = {0};
		if (m_engineClient) {
			void **evt = *(void***)m_engineClient;
			typedef const char* (__thiscall *GetLevelNameFn)(void*);
			const char *lvl = ((GetLevelNameFn)evt[l4d2::kGetLevelName])(m_engineClient);
			if (lvl) {
				const char *slash = strrchr(lvl, '/');
				if (!slash) slash = strrchr(lvl, '\\');
				const char *name = slash ? slash + 1 : lvl;
				strncpy(currentMap, name, sizeof(currentMap) - 1);
			}
		}

		bool inPlayable = IsInPlayableWorld();
		if (!inPlayable) {
			if (m_wasInWorld) {
				m_wasInWorld = false;
				m_lastPosValid = false;
			}
			return;
		}

		// Level change, quicksave load, or checkpoint reload detection
		bool isLoading = IsDrawingLoadingImage();
		bool levelChanged = (currentMap[0] != '\0' && strcmp(currentMap, m_lastMapName) != 0);
		bool saveReloaded = (m_wasLoading && !isLoading);

		float curGameTime = 0.0f;
		if (m_engineClient) {
			void **vt = *(void***)m_engineClient;
			typedef float (__thiscall *TimeFn)(void*);
			curGameTime = ((TimeFn)vt[14])(m_engineClient);
		}
		bool timeJumpedBack = (m_lastGameTime > 3.0f && curGameTime < m_lastGameTime - 2.0f);
		m_lastGameTime = curGameTime;
		m_wasLoading = isLoading;

		if (levelChanged || saveReloaded || timeJumpedBack) {
			if (levelChanged) {
				strncpy(m_lastMapName, currentMap, sizeof(m_lastMapName) - 1);
			}
			m_collisionEpoch++;
			m_teleportSeq++;
			m_hl2OverrideUntil = GetTickCount() + 1000;
			m_hasLastSentMcPos = false;
			m_mcPhysicsActive = false;
			m_placedBlocks.clear();
			m_placedLights.clear();
			m_knockbacks.clear();
			D3D9WorldRender::Get().ClearSections();
			ExecCmd("sv_cheats 1; script_execute left4craft");
			if (m_shmView) {
				uint64_t head = *(uint64_t*)(m_shmView + proto::kOffCollisionRing + proto::kColRingHeadOff);
				uint64_t pos = head % proto::kColRingDataBytes;
				uint8_t *slot = m_shmView + proto::kOffCollisionRing + proto::kColRingDataOff + pos;
				*(uint32_t*)slot = proto::kColClear;
				*(uint32_t*)(slot + 4) = 4;
				*(uint32_t*)(slot + 8) = m_collisionEpoch;
				*(uint64_t*)(m_shmView + proto::kOffCollisionRing + proto::kColRingHeadOff) = head + 16;
			}
		}

		Vector pos{0, 0, 0};
		QAngle angles{0, 0, 0};
		if (!GetPlayerPos(pos) || !GetAngles(angles)) {
			return;
		}

		if (!m_wasInWorld) {
			// Left4Craft diagnostics: where the local player's networkable / renderable virtuals really point
			// (checked offline for their argument counts against what this add-on's typedefs assume)
			if (void *pl = GetLocalPlayerEntity()) {
				HMODULE hClientDll = GetModuleHandleA("client.dll");
				void **netVtD = *(void***)((char*)pl + 8);
				void **renVtD = *(void***)((char*)pl + 4);
				void **unkVtD = *(void***)pl;
				for (int k = 0; k < 12; ++k)
					DiagLog("player networkable[%d] = client.dll+0x%X", k, (unsigned)((char*)netVtD[k] - (char*)hClientDll));
				for (int k = 0; k < 4; ++k)
					DiagLog("player renderable[%d] = client.dll+0x%X", k, (unsigned)((char*)renVtD[k] - (char*)hClientDll));
				for (int k = 0; k < 9; ++k)
					DiagLog("player unknown[%d] = client.dll+0x%X", k, (unsigned)((char*)unkVtD[k] - (char*)hClientDll));
			}
			m_wasInWorld = true;
			m_teleportSeq++;
			m_hl2OverrideUntil = GetTickCount() + 1000;
			m_hasLastSentMcPos = false;
			m_mcPhysicsActive = false;
			HookWindow();
			PatchMinecraftJumpHeight();
			ExecCmd("sv_cheats 1; mat_queue_mode 0; engine_no_focus_sleep 0; cl_drawhud 0; r_drawviewmodel 0; echo [left4craft] script_execute next; script_execute left4craft"); // Minecraft's HUD and hand replace L4D2's (both cheat cvars: not saved to config.cfg)
			ScanCollision(pos, 10);
		}
		m_lastPosX = pos.x;
		m_lastPosY = pos.y;
		m_lastPosZ = pos.z;
		m_lastPosValid = true;

		UpdateHostState(pos, angles);
		{
			static DWORD s_lastDiag = 0;
			DWORD nowDiag = GetTickCount();
			if (nowDiag - s_lastDiag > 2000) {
				s_lastDiag = nowDiag;
				const proto::McState *ms = (const proto::McState*)(m_shmView + proto::kOffMcState);
				const proto::HostState *hs = (const proto::HostState*)(m_shmView + proto::kOffHostState);
				DiagLog("map %s pos %.1f %.1f %.1f ang %.1f %.1f | mc %.3f %.3f %.3f seq %u flags 0x%x | host %.3f %.3f %.3f flags 0x%x tel %u | mcPhys %d",
					m_lastMapName, pos.x, pos.y, pos.z, angles.pitch, angles.yaw, ms->x, ms->y, ms->z, ms->seq, ms->flags,
					hs->posX, hs->posY, hs->posZ, hs->flags, hs->teleportSeq, m_mcPhysicsActive ? 1 : 0);
			}
		}
		ProcessInput();
		ProcessEvents();
		UpdateNpcKnockbacks();

		DWORD now = GetTickCount();
		if (now - m_lastStripTime > 400) {
			m_lastStripTime = now;
			static uint32_t s_stripCycle = 0;
			if ((++s_stripCycle & 7) == 1) {
				ExecCmd("firstperson; mat_queue_mode 0; engine_no_focus_sleep 0; cl_drawhud 0; r_drawviewmodel 0;");
			} else {
				// L4D2 keeps its weapons and crosshair (hotbar slots 1-2 are its guns)
			}
		}

		float dFastSq = (pos.x - m_lastFastScanPos.x)*(pos.x - m_lastFastScanPos.x) +
		                (pos.y - m_lastFastScanPos.y)*(pos.y - m_lastFastScanPos.y) +
		                (pos.z - m_lastFastScanPos.z)*(pos.z - m_lastFastScanPos.z);
		if (dFastSq > 400.0f || (now - m_lastFastScanTime > 300)) { // L4D2 maps: a radius-8 scan costs 30-47 ms
			m_lastFastScanTime = now;
			m_lastFastScanPos = pos;
			ScanCollision(pos, 8);
		}

		float dMovedSq = (pos.x - m_lastScanPos.x)*(pos.x - m_lastScanPos.x) +
		                 (pos.y - m_lastScanPos.y)*(pos.y - m_lastScanPos.y) +
		                 (pos.z - m_lastScanPos.z)*(pos.z - m_lastScanPos.z);
		if (dMovedSq > 14400.0f || (now - m_lastColScanTime > 2000)) {
			m_lastColScanTime = now;
			m_lastScanPos = pos;
			ScanCollision(pos, 26);
		}
		if (now - m_lastVScriptPoll > 100) {
			m_lastVScriptPoll = now;
			PollVScript();
		}
		// A round restart (all survivors dead, "mission lost") respawns every entity on the same map, so the level
		// change check above never fires: left4craft.nut's thinker is gone and the survivor gets its weapons and
		// L4D2's HUD back. Re-include the script whenever its table or its thinker is missing.
		if (now - m_lastVScriptCheck > 2000) {
			m_lastVScriptCheck = now;
			ExecCmd("script if (!(\"L4C\" in getroottable()) || Entities.FindByName(null, \"l4c_thinker\") == null) IncludeScript(\"left4craft\")");
		}
		if (now - m_lastActorScanTime > 50) {
			m_lastActorScanTime = now;
			ScanActors();
		}
	}

	static bool IsNpcClass(const char *className)
	{
		if (!className || !className[0]) return false;
		if (strstr(className, "Player") != nullptr) return false;
		// L4D2: common infected and the Witch (special infected are CTerrorPlayer: not mirrored yet)
		if (std::strcmp(className, "Infected") == 0 || std::strcmp(className, "Witch") == 0) return true;
		if (strstr(className, "NPC") != nullptr) return true;
		if (strstr(className, "Zombie") != nullptr) return true;
		if (strstr(className, "Headcrab") != nullptr) return true;
		if (strstr(className, "Antlion") != nullptr) return true;
		if (strstr(className, "Vortigaunt") != nullptr) return true;
		if (strstr(className, "Combine") != nullptr) return true;
		if (strstr(className, "Barnacle") != nullptr) return true;
		if (strstr(className, "Manhack") != nullptr) return true;
		if (strstr(className, "Scanner") != nullptr) return true;
		if (strstr(className, "Strider") != nullptr) return true;
		if (strstr(className, "Gunship") != nullptr) return true;
		if (strstr(className, "Helicopter") != nullptr) return true;
		if (strstr(className, "Rollermine") != nullptr) return true;
		if (strstr(className, "Stalker") != nullptr) return true;
		return false;
	}

	static bool IsPropClass(const char *className)
	{
		if (!className || !className[0]) return false;
		if (strstr(className, "Prop") != nullptr) return true;
		if (strstr(className, "Physics") != nullptr) return true;
		if (strstr(className, "Breakable") != nullptr) return true;
		return false;
	}

	static bool IsVehicleClass(const char *className)
	{
		if (!className || !className[0]) return false;
		if (strstr(className, "Vehicle") != nullptr) return true;
		if (strstr(className, "Airboat") != nullptr) return true;
		if (strstr(className, "Jeep") != nullptr) return true;
		if (strstr(className, "Crane") != nullptr) return true;
		if (strstr(className, "PrisonerPod") != nullptr) return true;
		return false;
	}

	class CTraceFilterEnvironment : public ITraceFilter {
	public:
		void *pPlayer = nullptr;
		const std::set<std::tuple<int, int, int>> *pPlacedBlocks = nullptr;
		virtual bool ShouldHitEntity(void *pEntity, int contentsMask) {
			if (!pEntity || pEntity == pPlayer) return false;
			if (!HL2Bridge::Get().IsHandleEntityClient(pEntity)) return true; // static prop / world: solid, not ours
			void *networkable = (char*)pEntity + 8;
			const char *className = "";
			if (!IsBadReadPtr(networkable, sizeof(void*))) {
				void **netVt = *(void***)networkable;
				if (!IsBadReadPtr(netVt, sizeof(void*) * 3)) {
					typedef void* (__thiscall *GetClientClassFn)(void*);
					void *clientClass = ((GetClientClassFn)netVt[l4d2::kNetGetClientClass])(networkable);
					if (clientClass && !IsBadReadPtr(clientClass, sizeof(void*) * 3)) {
						className = SafeClassName(clientClass);
					}
				}
			}
			if (IsNpcClass(className) || IsVehicleClass(className)) return false;
			if (className && (strstr(className, "Ragdoll") != nullptr || strstr(className, "Weapon") != nullptr || strstr(className, "Item") != nullptr)) return false;
			if (pPlacedBlocks && !pPlacedBlocks->empty() && IsPropClass(className)) {
				void *renderable = (char*)pEntity + 4;
				if (!IsBadReadPtr(renderable, sizeof(void*))) {
					void **rendVt = *(void***)renderable;
					if (!IsBadReadPtr(rendVt, sizeof(void*) * 2)) {
						typedef const float* (__thiscall *GetOriginFn)(void*);
						const float *org = ((GetOriginFn)rendVt[1])(renderable);
						if (org && !IsBadReadPtr(org, sizeof(float) * 3)) {
							int bx = (int)std::floor(org[0] * proto::kMetersPerUnit);
							int by = (int)std::floor(org[2] * proto::kMetersPerUnit + 64.0);
							int bz = (int)std::floor(-org[1] * proto::kMetersPerUnit);
							for (int dy = -1; dy <= 1; ++dy) {
								for (int dx = -1; dx <= 1; ++dx) {
									for (int dz = -1; dz <= 1; ++dz) {
										if (pPlacedBlocks->find({bx + dx, by + dy, bz + dz}) != pPlacedBlocks->end()) {
											float cbx = (bx + dx + 0.5f) * (float)proto::kUnitsPerBlock;
											float cby = -(bz + dz + 0.5f) * (float)proto::kUnitsPerBlock;
											float cbz = (by + dy + 0.5f - 64.0f) * (float)proto::kUnitsPerBlock;
							if (std::abs(org[0] - cbx) < 22.0f &&
							    std::abs(org[1] - cby) < 22.0f &&
							    std::abs(org[2] - cbz) < 22.0f) {
								return false; // Ignore our own placed block props (Minecraft already has their BlockState)
							}
										}
									}
								}
							}
						}
					}
				}
			}
			return true;
		}
		virtual int GetTraceType() const { return 0; } // TRACE_EVERYTHING
	};

	class CTraceFilterCombat : public ITraceFilter {
	public:
		void *pPlayer = nullptr;
		const std::set<std::tuple<int, int, int>> *pPlacedBlocks = nullptr;
		virtual bool ShouldHitEntity(void *pEntity, int contentsMask) {
			if (!pEntity || pEntity == pPlayer) return false;
			if (pPlacedBlocks && !pPlacedBlocks->empty()) {
				if (!HL2Bridge::Get().IsHandleEntityClient(pEntity)) return true; // static prop / world: solid, not ours
			void *networkable = (char*)pEntity + 8;
				const char *className = "";
				if (!IsBadReadPtr(networkable, sizeof(void*))) {
					void **netVt = *(void***)networkable;
					if (!IsBadReadPtr(netVt, sizeof(void*) * 3)) {
						typedef void* (__thiscall *GetClientClassFn)(void*);
						void *clientClass = ((GetClientClassFn)netVt[l4d2::kNetGetClientClass])(networkable);
						if (clientClass && !IsBadReadPtr(clientClass, sizeof(void*) * 3)) {
							className = SafeClassName(clientClass);
						}
					}
				}
				if (IsPropClass(className)) {
					void *renderable = (char*)pEntity + 4;
					if (!IsBadReadPtr(renderable, sizeof(void*))) {
						void **rendVt = *(void***)renderable;
						if (!IsBadReadPtr(rendVt, sizeof(void*) * 2)) {
							typedef const float* (__thiscall *GetOriginFn)(void*);
							const float *org = ((GetOriginFn)rendVt[1])(renderable);
							if (org && !IsBadReadPtr(org, sizeof(float) * 3)) {
								int bx = (int)std::floor(org[0] * proto::kMetersPerUnit);
								int by = (int)std::floor(org[2] * proto::kMetersPerUnit + 64.0);
								int bz = (int)std::floor(-org[1] * proto::kMetersPerUnit);
								for (int dy = -1; dy <= 1; ++dy) {
									for (int dx = -1; dx <= 1; ++dx) {
										for (int dz = -1; dz <= 1; ++dz) {
											if (pPlacedBlocks->find({bx + dx, by + dy, bz + dz}) != pPlacedBlocks->end()) {
												float cbx = (bx + dx + 0.5f) * (float)proto::kUnitsPerBlock;
												float cby = -(bz + dz + 0.5f) * (float)proto::kUnitsPerBlock;
												float cbz = (by + dy + 0.5f - 64.0f) * (float)proto::kUnitsPerBlock;
												if (std::abs(org[0] - cbx) < 22.0f &&
												    std::abs(org[1] - cby) < 22.0f &&
												    std::abs(org[2] - cbz) < 22.0f) {
													return false; // Ignore our invisible placed block collision props!
												}
											}
										}
									}
								}
							}
						}
					}
				}
			}
			return true;
		}
		virtual int GetTraceType() const { return 0; } // TRACE_EVERYTHING
	};

	void HL2Bridge::UpdateHostState(const Vector &pos, const QAngle &angles)
	{
		if (!m_shmView) return;

		DWORD nowMs = GetTickCount();
		if (m_hasLastSentMcPos && !m_unstickActive && nowMs >= m_hl2OverrideUntil) {
			float dx = pos.x - m_lastSentMcPos.x;
			float dy = pos.y - m_lastSentMcPos.y;
			float dz = pos.z - m_lastSentMcPos.z;
			// Detect HL2 trigger_teleport or map transition moving the player far from last setpos
			if (dx * dx + dy * dy + dz * dz > 140.0f * 140.0f) {
				m_teleportSeq++;
				m_hl2OverrideUntil = nowMs + 800;
				m_hasLastSentMcPos = false;
				m_mcPhysicsActive = false;
				ScanCollision(pos, 10);
			}
		}

		// Detect HL2 ladders and vehicles around the player
		bool onLadder = false;
		bool inOrNearVehicle = false;

		if (m_engineTrace) {
			void **tvt = *(void***)m_engineTrace;
			if (tvt && tvt[l4d2::kTraceRay] && tvt[0]) {
				typedef void (__thiscall *TraceRayFn)(void*, const Ray_t&, unsigned int, ITraceFilter*, trace_t*);
				typedef int (__thiscall *GetPointContentsFn)(void*, const Vector&, int, void**);
				TraceRayFn traceRay = (TraceRayFn)tvt[l4d2::kTraceRay];
				GetPointContentsFn getPointContents = (GetPointContentsFn)tvt[0];

				constexpr unsigned int kContentsLadder = 0x20000000;
				if ((getPointContents(m_engineTrace, Vector{pos.x, pos.y, pos.z + 18.0f}, l4d2::kMaskAll, nullptr) & kContentsLadder) != 0 ||
				    (getPointContents(m_engineTrace, Vector{pos.x, pos.y, pos.z + 48.0f}, l4d2::kMaskAll, nullptr) & kContentsLadder) != 0) {
					onLadder = true;
				}

				if (!onLadder) {
					CTraceFilterWorldOnly worldFilter;
					static const float kDirs[8][2] = {
						{ 28.0f, 0.0f }, { -28.0f, 0.0f }, { 0.0f, 28.0f }, { 0.0f, -28.0f },
						{ 20.0f, 20.0f }, { -20.0f, 20.0f }, { 20.0f, -20.0f }, { -20.0f, -20.0f }
					};
					for (int d = 0; d < 8 && !onLadder; ++d) {
						Vector start{ pos.x, pos.y, pos.z + 24.0f };
						Vector end{ pos.x + kDirs[d][0], pos.y + kDirs[d][1], pos.z + 24.0f };
						Ray_t ray;
						ray.Init(start, end);
						trace_t tr;
						std::memset(&tr, 0, sizeof(tr));
						traceRay(m_engineTrace, ray, kContentsLadder | 0x200400B, &worldFilter, &tr);
						if (tr.fraction < 1.0f || tr.startsolid) {
							if ((tr.contents & kContentsLadder) != 0) {
								onLadder = true;
							} else if (tr.surface.name && !IsBadReadPtr(tr.surface.name, 4)) {
								if (strstr(tr.surface.name, "ladder") != nullptr || strstr(tr.surface.name, "LADDER") != nullptr) {
									onLadder = true;
								}
							}
						}
					}
				}
			}
		}

		if (m_entityList) {
			void **lvt = *(void***)m_entityList;
			typedef void* (__thiscall *GetEntityFn)(void*, int);
			typedef int (__thiscall *GetHighestFn)(void*);
			GetEntityFn getEntity = (GetEntityFn)lvt[3];
			GetHighestFn getHighest = (GetHighestFn)lvt[6];
			int maxEnt = getHighest ? getHighest(m_entityList) : 256;
			maxEnt = std::clamp(maxEnt, 1, 512);
			void *pPlayer = GetLocalPlayerEntity();

			for (int i = 2; i <= maxEnt; ++i) {
				void *ent = getEntity(m_entityList, i);
				if (!IsNetworkableAt(i, ent)) continue;
				if (!ent || ent == pPlayer || IsBadReadPtr(ent, sizeof(void*))) continue;

				void *networkable = (char*)ent + 8;
				if (IsBadReadPtr(networkable, sizeof(void*) * 9)) continue;
				void **netVt = *(void***)networkable;
				if (IsBadReadPtr(netVt, sizeof(void*) * 9)) continue;

				typedef void* (__thiscall *GetClientClassFn)(void*);
				typedef bool (__thiscall *IsDormantFn)(void*);
				void *clientClass = ((GetClientClassFn)netVt[l4d2::kNetGetClientClass])(networkable);
				if (!clientClass || IsBadReadPtr(clientClass, sizeof(void*) * 3)) continue;
				const char *className = SafeClassName(clientClass);
				if (!className) continue;

				bool isVehicle = IsVehicleClass(className);
				bool isLadderEnt = (strstr(className, "Ladder") != nullptr);
				if (!isVehicle && !isLadderEnt) continue;
				if (((IsDormantFn)netVt[l4d2::kNetIsDormant])(networkable)) continue;

				void *renderable = (char*)ent + 4;
				if (IsBadReadPtr(renderable, sizeof(void*))) continue;
				void **rendVt = *(void***)renderable;
				if (IsBadReadPtr(rendVt, sizeof(void*) * 2)) continue;
				typedef const float* (__thiscall *GetOriginFn)(void*);
				const float *org = ((GetOriginFn)rendVt[1])(renderable);
				if (!org || IsBadReadPtr(org, sizeof(float) * 3)) continue;

				float dx = org[0] - pos.x;
				float dy = org[1] - pos.y;
				float dz = org[2] - pos.z;
				if (isVehicle) {
					if (dx * dx + dy * dy + dz * dz < 145.0f * 145.0f) {
						inOrNearVehicle = true;
					}
				} else if (isLadderEnt && !onLadder) {
					if (dx * dx + dy * dy < 44.0f * 44.0f && std::abs(dz) < 260.0f) {
						onLadder = true;
					}
				}
			}
		}

		proto::McState *ms = (proto::McState*)(m_shmView + proto::kOffMcState);
		if ((ms->flags & proto::kMcOnGround) == 0 && (ms->flags & proto::kMcFlying) == 0 && ms->y < m_lastMcY - 0.002) {
			m_lastFallingUntilMs = nowMs + 160;
		}
		m_lastMcY = ms->y;
		constexpr double kYOffset = 64.0;
		bool isDucking = !m_mcPhysicsActive && ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
		float eyeHeightUnits = isDucking ? l4d2::kEyeDuck : l4d2::kEyeStand;

		// Convert Source coords -> MC coords
		double feet_x = pos.x * proto::kMetersPerUnit;
		double feet_y = (pos.z * proto::kMetersPerUnit) + kYOffset;
		double feet_z = -pos.y * proto::kMetersPerUnit;

		double cam_x = feet_x;
		double cam_y = ((pos.z + eyeHeightUnits) * proto::kMetersPerUnit) + kYOffset;
		double cam_z = feet_z;
		{
			static DWORD s_lastPoseDiag = 0;
			if (GetTickCount() - s_lastPoseDiag > 2000) {
				s_lastPoseDiag = GetTickCount();
				DiagLog("pose calc: pos %.2f %.2f %.2f eye %.1f -> feet %.4f %.4f %.4f cam %.4f %.4f %.4f",
					pos.x, pos.y, pos.z, eyeHeightUnits, feet_x, feet_y, feet_z, cam_x, cam_y, cam_z);
			}
		}

		bool mcPosValid = (ms->seq > 0) && (std::abs(ms->x) > 0.01 || std::abs(ms->z) > 0.01 || std::abs(ms->y - 64.0) > 0.01);
		double dMcSq = (ms->x - feet_x) * (ms->x - feet_x) +
		               (ms->y - feet_y) * (ms->y - feet_y) +
		               (ms->z - feet_z) * (ms->z - feet_z);

		bool overrideMcPhysics = inOrNearVehicle || (nowMs < m_hl2OverrideUntil) || !mcPosValid;
		if (!overrideMcPhysics && !m_mcPhysicsActive && dMcSq > 9.0) {
			// Wait until Minecraft snaps to HL2's spawn/teleport position before switching to MC physics
			overrideMcPhysics = true;
			m_teleportSeq++;
		}

		// Convert Source angles -> MC angles
		float mc_yaw = -90.0f - angles.yaw;
		while (mc_yaw > 180.0f) mc_yaw -= 360.0f;
		while (mc_yaw < -180.0f) mc_yaw += 360.0f;
		float mc_pitch = angles.pitch;

		proto::HostState *hs = (proto::HostState*)(m_shmView + proto::kOffHostState);
		uint32_t seq = hs->seq;
		hs->seq = seq + 1;
		MemoryBarrier();

		uint32_t flags = proto::kHostInGame;
		if (IsPaused()) flags |= proto::kHostPaused;
		if (onLadder) flags |= proto::kHostOnLadder;
		if (overrideMcPhysics) flags |= proto::kHostVehicleOrOverride;

		Vector down = pos;
		down.z -= 6.0f;
		trace_t tr;
		if (TraceWorldRay(pos, down, tr) && tr.fraction < 1.0f) {
			flags |= proto::kHostOnGround;
		}

		hs->flags = flags;
		hs->mapId = 1;
		hs->collisionEpoch = m_collisionEpoch;
		hs->teleportSeq = m_teleportSeq;
		if (overrideMcPhysics) {
			hs->posX = cam_x;
			hs->posY = cam_y;
			hs->posZ = cam_z;
		} else {
			float mcEyeH = (ms->eyeHeight > 0.1f) ? ms->eyeHeight : 1.62f;
			hs->posX = ms->x;
			hs->posY = ms->y + mcEyeH;
			hs->posZ = ms->z;
		}
		{
			static DWORD s_lastHsDiag = 0;
			if (GetTickCount() - s_lastHsDiag > 2000) {
				s_lastHsDiag = GetTickCount();
				DiagLog("host write: override %d -> %.4f %.4f %.4f (cam %.4f %.4f %.4f)", overrideMcPhysics ? 1 : 0, hs->posX, hs->posY, hs->posZ, cam_x, cam_y, cam_z);
			}
		}
		hs->yaw = mc_yaw;
		hs->pitch = mc_pitch;

		HWND hwnd = FindWindowA("Valve001", nullptr);
		RECT rc;
		if (hwnd && GetClientRect(hwnd, &rc)) {
			hs->viewportW = rc.right - rc.left;
			hs->viewportH = rc.bottom - rc.top;
		} else {
			hs->viewportW = 1920;
			hs->viewportH = 1080;
		}
		m_viewW = hs->viewportW;
		m_viewH = hs->viewportH;

		float hl2Fov = l4d2::kHorizontalFov;
		float radFov = hl2Fov * 0.5f * 0.0174532925f;
		float vertFov = 2.0f * std::atan(std::tan(radFov) * 0.75f) * 57.2957795f;
		hs->fov = vertFov;
		hs->gameMode = m_creative ? 1 : 0;

		MemoryBarrier();
		hs->seq = seq + 2;

		// Check McState to see if Minecraft screen is open
		bool mcScreenOpen = (ms->flags & proto::kMcScreenOpen) != 0;
		SetGuiOpen(mcScreenOpen);

		// Synchronize player position: Minecraft physics -> HL2 (unless in vehicle or override window)
		if (overrideMcPhysics) {
			if (m_mcPhysicsActive || m_wasFlying) {
				m_mcPhysicsActive = false;
				m_wasFlying = false;
				m_hasLastSentMcPos = false;
				ExecCmd("script L4C.SetPuppet(false)"); // L4D2 walks the survivor again (ladder, vehicle, pinned...)
			}
		} else {
			if (!m_mcPhysicsActive) {
				m_mcPhysicsActive = true;
				// Left4Craft: the survivor floats as a puppet (near-zero gravity, setpos each frame) while Minecraft moves
				// Steve; it stays a walking player so the infected still chase and hit it (noclip made them do no
				// damage). Keys are swallowed in SubclassWndProc.
				ExecCmd("-forward; -back; -moveleft; -moveright; -jump; -duck; script L4C.SetPuppet(true)");
				m_lastPuppetMs = GetTickCount();
			}
			if (GetTickCount() - m_lastPuppetMs > 2000) {
				m_lastPuppetMs = GetTickCount(); // again now and then: a map load restarts the VScript
				ExecCmd("script L4C.SetPuppet(true)");
			}
			float mcEyeH = (ms->eyeHeight > 0.1f) ? ms->eyeHeight : 1.62f;
			float eyeOffsetBlocks = mcEyeH - 1.62f;
			float sx = (float)ms->x * (float)proto::kUnitsPerBlock;
			float sy = -(float)ms->z * (float)proto::kUnitsPerBlock;
			float sz = ((float)ms->y + eyeOffsetBlocks - 64.0f) * (float)proto::kUnitsPerBlock;

			// Real-time Source Engine 3D inner-core hull wall collision & push-out (prevents flying through HL2 walls at high speed/altitude without blocking narrow doorways)
			if (m_engineTrace && m_hasLastSentMcPos) {
				void **tvt = *(void***)m_engineTrace;
				if (tvt && tvt[l4d2::kTraceRay]) {
					typedef void (__thiscall *TraceRayFn)(void*, const Ray_t&, unsigned int, ITraceFilter*, trace_t*);
					TraceRayFn traceRay = (TraceRayFn)tvt[l4d2::kTraceRay];
					CTraceFilterWorldOnly worldFilter;

					constexpr unsigned int kWorldSolidMask = 0x201000B; // World brushes + playerclip + grates + windows (ignores moveable doors/props)
					Vector hullMins{ -5.0f, -5.0f, 24.0f };
					Vector hullMaxs{  5.0f,  5.0f, 46.0f };
					Vector targetPos{ sx, sy, sz };

					Ray_t hullRay;
					hullRay.InitHull(m_lastSentMcPos, targetPos, hullMins, hullMaxs);
					trace_t trHull;
					std::memset(&trHull, 0, sizeof(trHull));
					traceRay(m_engineTrace, hullRay, kWorldSolidMask, &worldFilter, &trHull);

					bool pushedOut = false;
					if (trHull.startsolid) {
						// Inner core is inside an HL2 world wall -> push radially outward to nearest free space
						for (int rStep = 1; rStep <= 8 && !pushedOut; ++rStep) {
							float r = rStep * 4.0f;
							for (int d = 0; d < 8; ++d) {
								float ang = d * (3.14159265f * 0.25f);
								Vector cand{ sx + std::cos(ang) * r, sy + std::sin(ang) * r, sz };
								Ray_t testRay;
								testRay.InitHull(cand, cand, hullMins, hullMaxs);
								trace_t trTest;
								std::memset(&trTest, 0, sizeof(trTest));
								traceRay(m_engineTrace, testRay, kWorldSolidMask, &worldFilter, &trTest);
								if (!trTest.startsolid) {
									sx = cand.x;
									sy = cand.y;
									pushedOut = true;
									break;
								}
							}
						}
					}
					// (HL2Craft's wall slide for fast flight is gone: Minecraft's own collision stops Steve, and a slide
					// applied only on the L4D2 side made the two positions drift apart)

					if (pushedOut) {
						float pdx = sx - targetPos.x;
						float pdy = sy - targetPos.y;
						// Every correction goes back to Minecraft (HL2Craft only sent ones over 64 units: smaller
						// ones left L4D2's survivor and Steve in different places)
						if (pdx * pdx + pdy * pdy > 0.25f) {
							DiagLog("push-out from world solid: %.1f units", std::sqrt(pdx * pdx + pdy * pdy));
							double corrMcX = sx * proto::kMetersPerUnit;
							double corrMcZ = -sy * proto::kMetersPerUnit;
							double corrMcFeetY = (sz * proto::kMetersPerUnit) + kYOffset - eyeOffsetBlocks;
							hs->seq = seq + 3;
							MemoryBarrier();
							hs->posX = corrMcX;
							hs->posY = corrMcFeetY + mcEyeH;
							hs->posZ = corrMcZ;
							m_teleportSeq++;
							hs->teleportSeq = m_teleportSeq;
							MemoryBarrier();
							hs->seq = seq + 4;
						}
					}
				}
			}

			m_lastSentMcPos = Vector{ sx, sy, sz };
			m_hasLastSentMcPos = true;

			char syncCmd[256];
			snprintf(syncCmd, sizeof(syncCmd),
				"ent_fire !player addoutput \"basevelocity 0 0 0\"; "
				"ent_fire !player addoutput \"velocity 0 0 0\"; "
				"setpos %.2f %.2f %.2f",
				sx, sy, sz);
			ExecCmd(syncCmd);

			// Real-time dynamic point lighting from held torches, lanterns, campfires, lava & placed lights
			static void *s_effects = nullptr;
			if (!s_effects) {
				HMODULE hEngine = GetModuleHandleA("engine.dll");
				if (hEngine) {
					CreateInterfaceFn engineFactory = (CreateInterfaceFn)GetProcAddress(hEngine, "CreateInterface");
					if (engineFactory) s_effects = engineFactory("VEngineEffects001", nullptr);
				}
			}
			if (s_effects && !IsBadReadPtr(s_effects, sizeof(void*))) {
				void **evt = *(void***)s_effects;
				if (evt && !IsBadReadPtr(evt, sizeof(void*) * 5)) {
					typedef dlight_t* (__thiscall *AllocDlightFn)(void*, int key);
					AllocDlightFn allocDlight = (AllocDlightFn)evt[4];

					// 1. Held item dynamic light
					if (ms->heldLight > 0) {
						dlight_t *dl = allocDlight(s_effects, 1001);
						if (dl) {
							dl->flags = 0;
							dl->origin = Vector{ sx, sy, sz + 42.0f };
							dl->radius = 170.0f * ((float)ms->heldLight / 15.0f);
							if (ms->heldLightType == 2) {
								dl->color.r = 60; dl->color.g = 200; dl->color.b = 255; dl->color.exponent = 0;
							} else {
								dl->color.r = 255; dl->color.g = 180; dl->color.b = 75; dl->color.exponent = 0;
							}
							dl->die = m_lastGameTime + 0.25f;
							dl->decay = 0.0f;
							dl->minlight = 4.0f;
						}
					}

					// 2. Placed torches, lanterns, campfires, and lava
					if (!m_placedLights.empty()) {
						int activeCount = 0;
						for (const auto &pl : m_placedLights) {
							if (activeCount >= 24) break;
							float lx = (pl.bx + 0.5f) * (float)proto::kUnitsPerBlock;
							float ly = -(pl.bz + 0.5f) * (float)proto::kUnitsPerBlock;
							float lz = (pl.by + 0.5f - 64.0f) * (float)proto::kUnitsPerBlock;
							float dx = lx - sx, dy = ly - sy, dz = lz - sz;
							if (dx * dx + dy * dy + dz * dz > 750.0f * 750.0f) continue;
							activeCount++;

							int key = 2000 + (int)(((pl.bx * 73856093) ^ (pl.by * 19349663) ^ (pl.bz * 83492791)) & 0x0FFF);
							dlight_t *dl = allocDlight(s_effects, key);
							if (dl) {
								dl->flags = 0;
								dl->origin = Vector{ lx, ly, lz };
								dl->radius = 150.0f * ((float)pl.lightLevel / 15.0f);
								if (pl.lightType == 2) {
									dl->color.r = 60; dl->color.g = 200; dl->color.b = 255; dl->color.exponent = 0;
								} else {
									dl->color.r = 255; dl->color.g = 175; dl->color.b = 70; dl->color.exponent = 0;
								}
								dl->die = m_lastGameTime + 0.25f;
								dl->decay = 0.0f;
								dl->minlight = 4.0f;
							}
						}
					}
				}
			}
		}

		// Synchronize damage taken in HL2 -> Minecraft
		bool mcBlocking = (ms->flags & proto::kMcBlocking) != 0;
		if (m_entityList && m_engineClient && !m_creative) {
			void *pPlayer = GetLocalPlayerEntity();
			int *pHealth = pPlayer ? l4d2::Netvar<int>(pPlayer, "m_iHealth") : nullptr;
			static bool s_noHealthLogged = false;
			if (pPlayer && !pHealth && !s_noHealthLogged) {
				s_noHealthLogged = true;
				DiagLog("damage: no m_iHealth netvar on the local player (class %s): L4D2 hits can't reach Minecraft", l4d2::ClassName(pPlayer));
			}
			if (pHealth && !IsBadReadPtr(pHealth, 4)) {
				int curHp = *pHealth;
				if (m_lastPlayerHl2Hp > 0 && curHp < m_lastPlayerHl2Hp) {
					int dmg = m_lastPlayerHl2Hp - curHp;
					DiagLog("damage: L4D2 health %d -> %d (%d), mc health %.1f, blocking %d, self-hit window %d",
						m_lastPlayerHl2Hp, curHp, dmg, ms->health, mcBlocking ? 1 : 0, GetTickCount() < m_ignoreSelfDamageUntil ? 1 : 0);
					bool blocked = false;
					if (mcBlocking) {
						constexpr float kDeg2Rad = 0.0174532925f;
						float radYaw = angles.yaw * kDeg2Rad;
						float radPitch = angles.pitch * kDeg2Rad;
						Vector fwd{
							std::cos(radPitch) * std::cos(radYaw),
							std::cos(radPitch) * std::sin(radYaw),
							-std::sin(radPitch)
						};

						// Find closest hostile/active NPC within 2500 units
						float bestDistSq = 2500.0f * 2500.0f;
						Vector bestAttackerPos{0, 0, 0};
						bool foundAttacker = false;

						void **lvt = *(void***)m_entityList;
						typedef void* (__thiscall *GetEntityFn)(void*, int);
						typedef int (__thiscall *GetHighestFn)(void*);
						GetEntityFn getEntity = (GetEntityFn)lvt[3];
						GetHighestFn getHighest = (GetHighestFn)lvt[6];
						int maxEnt = getHighest ? getHighest(m_entityList) : 256;
						maxEnt = std::clamp(maxEnt, 1, 512);

						for (int i = 2; i <= maxEnt; ++i) {
							void *ent = getEntity(m_entityList, i);
				if (!IsNetworkableAt(i, ent)) continue;
							if (!ent || ent == pPlayer) continue;

							void *networkable = (char*)ent + 8;
							if (IsBadReadPtr(networkable, sizeof(void*) * 9)) continue;
							void **netVt = *(void***)networkable;
							if (IsBadReadPtr(netVt, sizeof(void*) * 9)) continue;

							typedef void* (__thiscall *GetClientClassFn)(void*);
							typedef bool (__thiscall *IsDormantFn)(void*);
							void *clientClass = ((GetClientClassFn)netVt[l4d2::kNetGetClientClass])(networkable);
							if (!clientClass || IsBadReadPtr(clientClass, sizeof(void*) * 3)) continue;
							const char *className = SafeClassName(clientClass);
							if (!className || !IsNpcClass(className)) continue;
							if (((IsDormantFn)netVt[l4d2::kNetIsDormant])(networkable)) continue;

							void *renderable = (char*)ent + 4;
							if (IsBadReadPtr(renderable, sizeof(void*))) continue;
							void **rendVt = *(void***)renderable;
							if (IsBadReadPtr(rendVt, sizeof(void*) * 2)) continue;
							typedef const float* (__thiscall *GetOriginFn)(void*);
							const float *org = ((GetOriginFn)rendVt[1])(renderable);
							if (!org || IsBadReadPtr(org, sizeof(float) * 3)) continue;

							float dx = org[0] - pos.x;
							float dy = org[1] - pos.y;
							float dz = org[2] - pos.z;
							float dSq = dx * dx + dy * dy + dz * dz;
							if (dSq < bestDistSq) {
								bestDistSq = dSq;
								bestAttackerPos = { org[0], org[1], org[2] };
								foundAttacker = true;
							}
						}

						if (foundAttacker) {
							float dirX = bestAttackerPos.x - pos.x;
							float dirY = bestAttackerPos.y - pos.y;
							float dirLen = std::sqrt(dirX * dirX + dirY * dirY);
							if (dirLen > 1.0f) {
								dirX /= dirLen;
								dirY /= dirLen;
								float dot = fwd.x * dirX + fwd.y * dirY;
								// Shield in MC blocks in 180° frontal arc
								if (dot > -0.1f) {
									blocked = true;
								}
							} else {
								blocked = true;
							}
						}
					}

					if (blocked || GetTickCount() < m_ignoreSelfDamageUntil) {
						// Shield absorbs the damage or self-damage immunity window active!
						*pHealth = m_lastPlayerHl2Hp;
						curHp = m_lastPlayerHl2Hp;
						if (blocked) {
							ExecCmd("play weapons/fx/rics/ric1.wav");
						}
					} else {
						if (curHp <= 0) {
							// Player died in HL2 -> clear blocks and kill props
							m_collisionEpoch++;
							m_placedBlocks.clear();
							D3D9WorldRender::Get().ClearSections();
							ExecCmd("script L4C.ClearAll()");
							uint64_t head = *(uint64_t*)(m_shmView + proto::kOffCollisionRing + proto::kColRingHeadOff);
							uint64_t ringPos = head % proto::kColRingDataBytes;
							uint8_t *slot = m_shmView + proto::kOffCollisionRing + proto::kColRingDataOff + ringPos;
							*(uint32_t*)slot = proto::kColClear;
							*(uint32_t*)(slot + 4) = 4;
							*(uint32_t*)(slot + 8) = m_collisionEpoch;
							MemoryBarrier();
							*(uint64_t*)(m_shmView + proto::kOffCollisionRing + proto::kColRingHeadOff) = head + 16;
						}
						PushInput(proto::kInHurt, 0, dmg * 4, 0); // L4D2 hp mirrors MC hp x5
						DiagLog("damage: sent %d L4D2 hp to Minecraft (%.2f hearts of damage)", dmg, dmg / 5.0f / 2.0f);
					}
				}
				m_lastPlayerHl2Hp = curHp;
			}
		}

		// Synchronize death from Minecraft
		bool mcDead = (ms->flags & proto::kMcDead) != 0;
		if (mcDead && !m_mcWasDead) {
			m_mcWasDead = true;
			m_placedBlocks.clear();
			D3D9WorldRender::Get().ClearSections();
			ExecCmd("script L4C.ClearAll(); ent_fire !player sethealth 0");
		} else if (!mcDead && m_mcWasDead) {
			m_mcWasDead = false;
			m_lastSyncedHp = 100;
			m_lastPlayerHl2Hp = 100;
			ExecCmd("ent_fire !player sethealth 100");
		}

		// Synchronize health from Minecraft (if alive)
		if (!mcDead) {
			int targetHp = (int)std::ceil(ms->health * 5.0f);
			if (targetHp < 1) targetHp = 1;
			if (targetHp > 100) targetHp = 100;
			if (targetHp != m_lastSyncedHp) {
				m_lastSyncedHp = targetHp;
				m_lastPlayerHl2Hp = targetHp;
				char cmd[64];
				snprintf(cmd, sizeof(cmd), "ent_fire !player sethealth %d", targetHp);
				DiagLog("health mirror: Minecraft %.1f -> L4D2 sethealth %d", ms->health, targetHp);
				ExecCmd(cmd);
			}
		}

		m_creative = (ms->gameMode == 1);
	}

	typedef BOOL (WINAPI *SetCursorPosFn)(int, int);
	static SetCursorPosFn g_realSetCursorPos = nullptr;
	static uint8_t g_savedCursorBytes[5] = {0};

	static BOOL WINAPI Hook_SetCursorPos(int x, int y) {
		if (HL2Bridge::Get().IsGuiOpen()) {
			return TRUE; // Do not center or move cursor while inventory or chat is open
		}
		DWORD oldProt;
		VirtualProtect((void*)g_realSetCursorPos, 5, PAGE_EXECUTE_READWRITE, &oldProt);
		std::memcpy((void*)g_realSetCursorPos, g_savedCursorBytes, 5);
		BOOL res = g_realSetCursorPos(x, y);
		uint8_t jmp[5] = { 0xE9, 0, 0, 0, 0 };
		*(int32_t*)(jmp + 1) = (int32_t)((char*)Hook_SetCursorPos - (char*)g_realSetCursorPos - 5);
		std::memcpy((void*)g_realSetCursorPos, jmp, 5);
		VirtualProtect((void*)g_realSetCursorPos, 5, oldProt, &oldProt);
		return res;
	}

	void HL2Bridge::ScanCollision(const Vector &playerPos, int radius)
	{
		if (!m_engineTrace || !m_shmView || !IsInPlayableWorld()) return;
		void **vt = *(void***)m_engineTrace;
		if (!vt || !vt[l4d2::kTraceRay] || !vt[0]) return;
		const DWORD scanStart = GetTickCount();
		typedef void (__thiscall *TraceRayFn)(void*, const Ray_t&, unsigned int, ITraceFilter*, trace_t*);
		TraceRayFn traceRay = (TraceRayFn)vt[l4d2::kTraceRay];
		typedef int (__thiscall *GetPointContentsFn)(void*, const Vector&, int, void**);
		GetPointContentsFn getPointContents = (GetPointContentsFn)vt[0];

		CTraceFilterEnvironment filter;
		filter.pPlacedBlocks = &m_placedBlocks;
		if (m_entityList && m_engineClient) {
			void **evt = *(void***)m_engineClient;
			typedef int (__thiscall *GetLocalPlayerFn)(void*);
			int localPlayer = ((GetLocalPlayerFn)evt[12])(m_engineClient);
			if (localPlayer > 0) {
				void **lvt = *(void***)m_entityList;
				typedef void* (__thiscall *GetEntityFn)(void*, int);
				filter.pPlayer = ((GetEntityFn)lvt[3])(m_entityList, localPlayer);
			}
		}

		constexpr int kMaxRadius = 26;
		constexpr int kMaxGridSize = kMaxRadius * 2 + 1;
		int r = std::clamp(radius, 4, kMaxRadius);
		int gridSize = r * 2 + 1;
		constexpr float kBlockUnits = (float)proto::kUnitsPerBlock;

		constexpr double kYOffset = 64.0;
		constexpr unsigned int kStaticWorldMask = 0x201400B; // MASK_PLAYERSOLID (brushes, playerclip, moveable doors, static props, grates, windows)
		constexpr unsigned int kSolidContentsMask = 0x1400B;

		g_traceThread = GetCurrentThreadId();
		g_inTrace = true;
		if (setjmp(g_traceJmp) != 0) {
			g_inTrace = false;
			return; // Safely handled any engine trace exception
		}

		// Anchor floor scan to the actual ground below the player's legs (never above ceiling when jumping/flying)
		float baseFloorZ = playerPos.z;
		{
			Ray_t downRay;
			downRay.Init(Vector{playerPos.x, playerPos.y, playerPos.z + 18.0f}, Vector{playerPos.x, playerPos.y, playerPos.z - 2000.0f});
			trace_t downTr;
			std::memset(&downTr, 0, sizeof(downTr));
			traceRay(m_engineTrace, downRay, kStaticWorldMask, &filter, &downTr);
			if (downTr.fraction < 1.0f && !downTr.startsolid) {
				baseFloorZ = downTr.endpos.z;
			}
		}

		int centerBx = (int)std::floor(playerPos.x * proto::kMetersPerUnit);
		int centerBz = (int)std::floor(-playerPos.y * proto::kMetersPerUnit);
		int centerBy = (int)std::floor((baseFloorZ * proto::kMetersPerUnit) + kYOffset);
		int playerBy = (int)std::floor((playerPos.z * proto::kMetersPerUnit) + kYOffset);

		int minX = centerBx - r, maxX = centerBx + r;
		int minZ = centerBz - r, maxZ = centerBz + r;
		int minY = std::min(centerBy, playerBy) - 10;
		int maxY = std::max(centerBy, playerBy) + 10;

		std::vector<proto::ColBlock> blocks;
		blocks.reserve(gridSize * gridSize * 6);

		int floorGrid[kMaxGridSize][kMaxGridSize];
		float floorZGrid[kMaxGridSize][kMaxGridSize];
		float floorNzGrid[kMaxGridSize][kMaxGridSize];
		int floorSubGrid[kMaxGridSize][kMaxGridSize];
		for (int x = 0; x < gridSize; x++) {
			for (int z = 0; z < gridSize; z++) {
				floorGrid[x][z] = -9999;
				floorZGrid[x][z] = 0.0f;
				floorNzGrid[x][z] = 1.0f;
				floorSubGrid[x][z] = 7;
			}
		}

		// Pass 1: Floor & slope detection (only step ray origin up when lower point is a walkable step with open headroom, never inside a wall/doorframe)
		for (int ix = 0; ix < gridSize; ix++) {
			int bx = minX + ix;
			for (int iz = 0; iz < gridSize; iz++) {
				int bz = minZ + iz;

				float hx = (bx + 0.5f) * kBlockUnits;
				float hy = -(bz + 0.5f) * kBlockUnits;
				float hzStart = baseFloorZ + 18.0f;
				float hzEnd = baseFloorZ - 420.0f;

				// Only raise hzStart if lower point is solid AND headroom above (+64 and +82) is open air (stairs/slopes/crates, never a wall/doorframe)
				if (getPointContents && (getPointContents(m_engineTrace, Vector{hx, hy, hzStart}, l4d2::kMaskAll, nullptr) & kSolidContentsMask) != 0) {
					bool headroomOpen = ((getPointContents(m_engineTrace, Vector{hx, hy, baseFloorZ + 64.0f}, l4d2::kMaskAll, nullptr) & kSolidContentsMask) == 0) &&
					                    ((getPointContents(m_engineTrace, Vector{hx, hy, baseFloorZ + 82.0f}, l4d2::kMaskAll, nullptr) & kSolidContentsMask) == 0);
					if (headroomOpen) {
						if ((getPointContents(m_engineTrace, Vector{hx, hy, baseFloorZ + 34.0f}, l4d2::kMaskAll, nullptr) & kSolidContentsMask) == 0) {
							hzStart = baseFloorZ + 34.0f;
						} else if ((getPointContents(m_engineTrace, Vector{hx, hy, baseFloorZ + 48.0f}, l4d2::kMaskAll, nullptr) & kSolidContentsMask) == 0) {
							hzStart = baseFloorZ + 48.0f;
						}
					}
				}

				Ray_t ray;
				ray.Init(Vector{hx, hy, hzStart}, Vector{hx, hy, hzEnd});
				trace_t tr;
				std::memset(&tr, 0, sizeof(tr));
				traceRay(m_engineTrace, ray, kStaticWorldMask, &filter, &tr);

				// If block center is inside a doorframe/wall pillar, sample the 4 quadrant centers so narrow passages keep their floor
				if (tr.startsolid || tr.fraction >= 1.0f) {
					for (int qx = 0; qx < 2 && (tr.startsolid || tr.fraction >= 1.0f); ++qx) {
						for (int qz = 0; qz < 2; ++qz) {
							float qhx = (bx + 0.25f + qx * 0.5f) * kBlockUnits;
							float qhy = -(bz + 0.25f + qz * 0.5f) * kBlockUnits;
							Ray_t qRay;
							qRay.Init(Vector{qhx, qhy, baseFloorZ + 18.0f}, Vector{qhx, qhy, hzEnd});
							trace_t qTr;
							std::memset(&qTr, 0, sizeof(qTr));
							traceRay(m_engineTrace, qRay, kStaticWorldMask, &filter, &qTr);
							if (!qTr.startsolid && qTr.fraction < 1.0f) {
								tr = qTr;
								break;
							}
						}
					}
				}

				if (tr.fraction < 1.0f && !tr.startsolid) {
					float floorZ = tr.endpos.z;
					float floorYMc = (floorZ * (float)proto::kMetersPerUnit) + (float)kYOffset;
					int by = (int)std::floor(floorYMc - 0.15f);

					if (by >= minY && by <= maxY) {
						int sub = std::clamp((int)((floorYMc - by) * 8.0f), 0, 7);
						floorGrid[ix][iz] = by;
						floorZGrid[ix][iz] = floorZ;
						floorNzGrid[ix][iz] = tr.plane.normal.z;
						floorSubGrid[ix][iz] = sub;

						float nx = tr.plane.normal.x;
						float ny = tr.plane.normal.y;
						float nz = tr.plane.normal.z;

						proto::ColBlock topBlock;
						topBlock.x = bx;
						topBlock.y = by;
						topBlock.z = bz;
						topBlock.pad = 0;
						for (int l = 0; l < 8; l++) topBlock.bits[l] = 0ULL;

						proto::ColBlock upperSlopeBlock;
						upperSlopeBlock.x = bx;
						upperSlopeBlock.y = by + 1;
						upperSlopeBlock.z = bz;
						upperSlopeBlock.pad = 0;
						for (int l = 0; l < 8; l++) upperSlopeBlock.bits[l] = 0ULL;
						bool hasUpperSlope = false;

						if (nz >= 0.50f && nz < 0.97f) {
							// Sloped surface: compute 8x8 micro-staircase heights across the block
							for (int sz = 0; sz < 8; ++sz) {
								float dzMc = (sz + 0.5f) * 0.125f - 0.5f;
								for (int sx = 0; sx < 8; ++sx) {
									float dxMc = (sx + 0.5f) * 0.125f - 0.5f;
									float dyMc = (-nx * dxMc + ny * dzMc) / nz;
									float colYMc = floorYMc + dyMc;
									int totalSub = std::clamp((int)std::floor((colYMc - by) * 8.0f), 0, 13);
									uint64_t bitMask = 1ULL << (sx | (sz << 3));
									int topSub = std::min(totalSub, 7);
									for (int l = 0; l <= topSub; ++l) {
										topBlock.bits[l] |= bitMask;
									}
									if (totalSub >= 8) {
										hasUpperSlope = true;
										int upSub = totalSub - 8;
										for (int l = 0; l <= upSub; ++l) {
											upperSlopeBlock.bits[l] |= bitMask;
										}
									}
								}
							}
						} else {
							for (int l = 0; l <= sub; l++) {
								topBlock.bits[l] = 0xFFFFFFFFFFFFFFFFULL;
							}
						}
						blocks.push_back(topBlock);
						if (hasUpperSlope && by + 1 <= maxY) {
							blocks.push_back(upperSlopeBlock);
						}

						// Solid support column directly below floor surface so steps/ledges/crates have solid side walls
						for (int depth = 1; depth <= 3; ++depth) {
							proto::ColBlock subBlock;
							subBlock.x = bx;
							subBlock.y = by - depth;
							subBlock.z = bz;
							subBlock.pad = 0;
							for (int l = 0; l < 8; l++) subBlock.bits[l] = 0xFFFFFFFFFFFFFFFFULL;
							blocks.push_back(subBlock);
						}
					}
				}
			}
		}

		static const uint64_t kQuadMasks[2][2] = {
			{ 0x000000000F0F0F0FULL, 0x0F0F0F0F00000000ULL },
			{ 0x00000000F0F0F0F0ULL, 0xF0F0F0F000000000ULL }
		};

		// Hierarchical 8x8 sub-voxel wall scanner (4.72-unit / 0.125-block resolution near player so narrow passages & doorways are never narrowed)
		auto trace8x8WallMask = [&](int bx, int bz, float testZ, float halfZ) -> uint64_t {
			float bcx = (bx + 0.5f) * kBlockUnits;
			float bcy = -(bz + 0.5f) * kBlockUnits;
			Ray_t bRay;
			bRay.InitHull(Vector{bcx, bcy, testZ}, Vector{bcx, bcy, testZ},
			              Vector{-18.85f, -18.85f, -halfZ}, Vector{18.85f, 18.85f, halfZ});
			trace_t bTr;
			std::memset(&bTr, 0, sizeof(bTr));
			traceRay(m_engineTrace, bRay, kStaticWorldMask, &filter, &bTr);
			if (!bTr.startsolid) {
				return 0ULL;
			}

			bool fineSubVoxels = (std::abs(bx - centerBx) <= 6 && std::abs(bz - centerBz) <= 6);
			uint64_t mask = 0ULL;
			Vector qMins{ -9.4f, -9.4f, -halfZ };
			Vector qMaxs{  9.4f,  9.4f,  halfZ };
			Vector subMins{ -1.8f, -1.8f, -halfZ };
			Vector subMaxs{  1.8f,  1.8f,  halfZ };

			for (int qx = 0; qx < 2; ++qx) {
				for (int qz = 0; qz < 2; ++qz) {
					float qcx = (bx + 0.25f + qx * 0.5f) * kBlockUnits;
					float qcy = -(bz + 0.25f + qz * 0.5f) * kBlockUnits;
					Ray_t qRay;
					qRay.InitHull(Vector{qcx, qcy, testZ}, Vector{qcx, qcy, testZ}, qMins, qMaxs);
					trace_t qTr;
					std::memset(&qTr, 0, sizeof(qTr));
					traceRay(m_engineTrace, qRay, kStaticWorldMask, &filter, &qTr);
					if (!qTr.startsolid) continue;

					if (!fineSubVoxels) {
						mask |= kQuadMasks[qx][qz];
					} else {
						for (int sx = qx * 4; sx < qx * 4 + 4; ++sx) {
							float scx = (bx + (sx + 0.5f) * 0.125f) * kBlockUnits;
							for (int sz = qz * 4; sz < qz * 4 + 4; ++sz) {
								float scy = -(bz + (sz + 0.5f) * 0.125f) * kBlockUnits;
								Ray_t sRay;
								sRay.InitHull(Vector{scx, scy, testZ}, Vector{scx, scy, testZ}, subMins, subMaxs);
								trace_t sTr;
								std::memset(&sTr, 0, sizeof(sTr));
								traceRay(m_engineTrace, sRay, kStaticWorldMask, &filter, &sTr);
								if (sTr.startsolid) {
									mask |= (1ULL << (sx | (sz << 3)));
								}
							}
						}
					}
				}
			}
			return mask;
		};

		// Pass 2: 8x8 Sub-Voxel walls, fences, closed doors, crouch-vents, ceilings, and flight-altitude walls
		for (int ix = 0; ix < gridSize; ix++) {
			int bx = minX + ix;
			for (int iz = 0; iz < gridSize; iz++) {
				int bz = minZ + iz;
				int by = floorGrid[ix][iz];
				float cx = (bx + 0.5f) * kBlockUnits;
				float cy = -(bz + 0.5f) * kBlockUnits;

				if (by == -9999) {
					uint64_t solidMask = trace8x8WallMask(bx, bz, playerPos.z + 32.0f, 6.0f);
					if (solidMask != 0ULL) {
						for (int wy = std::min(centerBy, playerBy); wy <= std::max(centerBy, playerBy) + 2; wy++) {
							proto::ColBlock wb;
							wb.x = bx; wb.y = wy; wb.z = bz; wb.pad = 0;
							for (int l = 0; l < 8; l++) wb.bits[l] = solidMask;
							blocks.push_back(wb);
						}
					}
					continue;
				}

				float floorZ = floorZGrid[ix][iz];
				float floorNz = floorNzGrid[ix][iz];
				int sub = floorSubGrid[ix][iz];
				// Test lower wall at knee/waist height (28 units on flat floors, 38 on slopes so low stairwell ceilings & vents aren't hit)
				float zLowerWall = (floorNz > 0.96f) ? (floorZ + 28.0f) : (floorZ + 38.0f);
				uint64_t lowerWallMask = trace8x8WallMask(bx, bz, zLowerWall, 4.0f);

				if (lowerWallMask != 0ULL) {
					for (int wy = by + 1; wy <= std::min(by + 2, maxY); ++wy) {
						proto::ColBlock wb;
						wb.x = bx; wb.y = wy; wb.z = bz; wb.pad = 0;
						for (int l = 0; l < 8; l++) wb.bits[l] = lowerWallMask;
						blocks.push_back(wb);
					}
					if (by + 3 <= maxY) {
						uint64_t upperMask = trace8x8WallMask(bx, bz, floorZ + 68.0f, 6.0f);
						if (upperMask != 0ULL) {
							proto::ColBlock wb3;
							wb3.x = bx; wb3.y = by + 3; wb3.z = bz; wb3.pad = 0;
							for (int l = 0; l < 8; l++) wb3.bits[l] = upperMask;
							blocks.push_back(wb3);
						}
					}
				} else {
					// Check for crouch-vents / low overhangs (clear at knee height, solid at floorZ + 52)
					float zVent = floorZ + 52.0f;
					bool blockedVent = getPointContents && ((getPointContents(m_engineTrace, Vector{cx, cy, zVent}, l4d2::kMaskAll, nullptr) & kSolidContentsMask) != 0);
					if (blockedVent && by + 2 <= maxY) {
						int ceilStartLayer = (sub + 1 + 13) - 16; // in block by + 2
						if (ceilStartLayer < 0) ceilStartLayer = 0;
						proto::ColBlock ventCeil;
						ventCeil.x = bx; ventCeil.y = by + 2; ventCeil.z = bz; ventCeil.pad = 0;
						for (int l = 0; l < 8; l++) {
							ventCeil.bits[l] = (l >= ceilStartLayer) ? 0xFFFFFFFFFFFFFFFFULL : 0ULL;
						}
						blocks.push_back(ventCeil);
					}
				}

				// Check higher ceilings and walls at the player's current altitude (when jumping, pillaring upwards, or flying with Elytra)
				int highStart = by + 3;
				int highEnd = std::min(std::max(by + 4, playerBy + 2), maxY);
				for (int wy = highStart; wy <= highEnd; ++wy) {
					float cz = (wy + 0.5f - (float)kYOffset) * kBlockUnits;
					if (wy >= playerBy - 1 && wy <= playerBy + 2 && playerBy > by + 2) {
						uint64_t highMask = trace8x8WallMask(bx, bz, cz, 6.0f);
						if (highMask != 0ULL) {
							proto::ColBlock cb;
							cb.x = bx; cb.y = wy; cb.z = bz; cb.pad = 0;
							for (int l = 0; l < 8; l++) cb.bits[l] = highMask;
							blocks.push_back(cb);
						}
					} else if (getPointContents && ((getPointContents(m_engineTrace, Vector{cx, cy, cz}, l4d2::kMaskAll, nullptr) & kSolidContentsMask) != 0)) {
						proto::ColBlock cb;
						cb.x = bx; cb.y = wy; cb.z = bz; cb.pad = 0;
						for (int l = 0; l < 8; l++) cb.bits[l] = 0xFFFFFFFFFFFFFFFFULL;
						blocks.push_back(cb);
					}
				}
			}
		}

		g_inTrace = false;

		{
			DWORD took = GetTickCount() - scanStart;
			if (took > 30) DiagLog("ScanCollision radius %d took %lu ms (%zu blocks)", radius, (unsigned long)took, blocks.size());
		}
		// an empty region is sent too: it clears geometry that went away (an opened door)

		// Write to CollisionRing
		uint64_t head = *(uint64_t*)(m_shmView + proto::kOffCollisionRing + proto::kColRingHeadOff);
		uint32_t payloadBytes = sizeof(proto::ColRegion) + blocks.size() * sizeof(proto::ColBlock);
		uint32_t totalMsgBytes = (8 + payloadBytes + 7) & ~7;

		uint64_t pos = head % proto::kColRingDataBytes;
		if (pos + totalMsgBytes > proto::kColRingDataBytes) {
			// Wrap pad
			uint8_t *padSlot = m_shmView + proto::kOffCollisionRing + proto::kColRingDataOff + pos;
			*(uint32_t*)padSlot = proto::kColPad;
			*(uint32_t*)(padSlot + 4) = 0;
			head += (proto::kColRingDataBytes - pos);
			pos = 0;
		}

		uint8_t *slot = m_shmView + proto::kOffCollisionRing + proto::kColRingDataOff + pos;
		*(uint32_t*)slot = proto::kColRegion;
		*(uint32_t*)(slot + 4) = payloadBytes;

		proto::ColRegion *reg = (proto::ColRegion*)(slot + 8);
		reg->minX = minX; reg->minY = minY; reg->minZ = minZ;
		reg->maxX = maxX; reg->maxY = maxY; reg->maxZ = maxZ;
		reg->epoch = m_collisionEpoch;
		reg->count = (uint32_t)blocks.size();

		std::memcpy(slot + 8 + sizeof(proto::ColRegion), blocks.data(), blocks.size() * sizeof(proto::ColBlock));
		MemoryBarrier();
		*(uint64_t*)(m_shmView + proto::kOffCollisionRing + proto::kColRingHeadOff) = head + totalMsgBytes;
	}

	void HL2Bridge::ScanActors()
	{
		if (!m_entityList || !m_shmView || !IsInPlayableWorld()) return;
		void **vt = *(void***)m_entityList;
		typedef void* (__thiscall *GetEntityFn)(void*, int);
		typedef int (__thiscall *GetHighestFn)(void*);
		GetEntityFn getEntity = (GetEntityFn)vt[3];
		GetHighestFn getHighest = (GetHighestFn)vt[6];

		int localPlayer = 1;
		if (m_engineClient) {
			void **evt = *(void***)m_engineClient;
			typedef int (__thiscall *GetLocalPlayerFn)(void*);
			localPlayer = ((GetLocalPlayerFn)evt[12])(m_engineClient);
		}
		void *pPlayerEnt = GetLocalPlayerEntity();
		Vector playerPos{0, 0, 0};
		bool hasPlayerPos = GetPlayerPos(playerPos);

		int maxEnts = getHighest ? getHighest(m_entityList) : 64;
		if (maxEnts > 2048) maxEnts = 2048; // L4D2: common infected often sit above index 256

		proto::ActorTable *tbl = (proto::ActorTable*)(m_shmView + proto::kOffActorTable);
		uint32_t seq = tbl->seq;
		tbl->seq = seq + 1;
		MemoryBarrier();

		constexpr double kYOffset = 64.0;
		uint32_t count = 0;
		for (int i = 2; i <= maxEnts && count < proto::kMaxActors; i++) {
			if (i == localPlayer) continue;

			void *ent = getEntity(m_entityList, i);
				if (!IsNetworkableAt(i, ent)) continue;
			if (!ent || ent == pPlayerEnt || IsBadReadPtr(ent, sizeof(void*))) continue;

			void *networkable = (char*)ent + 8;
			if (IsBadReadPtr(networkable, sizeof(void*))) continue;
			void **netVt = *(void***)networkable;
			if (IsBadReadPtr(netVt, sizeof(void*) * 9)) continue;

			typedef void* (__thiscall *GetClientClassFn)(void*);
			typedef bool (__thiscall *IsDormantFn)(void*);
			void *clientClass = ((GetClientClassFn)netVt[l4d2::kNetGetClientClass])(networkable);
			if (!clientClass || IsBadReadPtr(clientClass, sizeof(void*) * 3)) continue;
			const char *className = SafeClassName(clientClass);
			if (!className) continue;

			// Only mirror living NPCs as HL2ActorEntity proxies in Minecraft (never props or placed blocks!)
			bool isNpc = IsNpcClass(className);
			if (!isNpc) continue;

			if (((IsDormantFn)netVt[l4d2::kNetIsDormant])(networkable)) continue;

			if (uint8_t *pLife = l4d2::Netvar<uint8_t>(ent, "m_lifeState")) {
				uint8_t lifeState = *pLife;
				if (lifeState != 0) continue;
			}

			void *renderable = (char*)ent + 4;
			if (IsBadReadPtr(renderable, sizeof(void*))) continue;
			void **rendVt = *(void***)renderable;
			if (IsBadReadPtr(rendVt, sizeof(void*) * 2)) continue;

			typedef const float* (__thiscall *GetOriginFn)(void*);
			const float *org = ((GetOriginFn)rendVt[1])(renderable);
			if (!org || IsBadReadPtr(org, sizeof(float) * 3)) continue;

			// Never create a proxy hitbox overlapping the local player's own position
			if (hasPlayerPos) {
				float dx = org[0] - playerPos.x;
				float dy = org[1] - playerPos.y;
				float dz = org[2] - playerPos.z;
				if (dx * dx + dy * dy < 24.0f * 24.0f && std::abs(dz) < 72.0f) {
					continue;
				}
			}

			// Ignite and burn any NPC standing in placed lava
			for (const auto &pl : m_placedLights) {
				if (!pl.isLava) continue;
				float lx = (pl.bx + 0.5f) * (float)proto::kUnitsPerBlock;
				float ly = -(pl.bz + 0.5f) * (float)proto::kUnitsPerBlock;
				float lz = (pl.by + 0.5f - 64.0f) * (float)proto::kUnitsPerBlock;
				float ldx = org[0] - lx;
				float ldy = org[1] - ly;
				float ldz = org[2] - lz;
				if (ldx * ldx + ldy * ldy < 32.0f * 32.0f && std::abs(ldz) < 40.0f) {
					static uint32_t s_lavaBurnCount = 0;
					uint32_t lbId = ++s_lavaBurnCount;
					char lavaBurnCmd[256];
					snprintf(lavaBurnCmd, sizeof(lavaBurnCmd),
						"ent_create point_hurt targetname mc_lv_%u; "
						"ent_fire mc_lv_%u addoutput \"origin %.2f %.2f %.2f\"; "
						"ent_fire mc_lv_%u addoutput \"Damage 12\"; "
						"ent_fire mc_lv_%u addoutput \"DamageRadius 28\"; "
						"ent_fire mc_lv_%u addoutput \"DamageType 8\"; "
						"ent_fire mc_lv_%u Hurt; "
						"ent_fire mc_lv_%u Kill",
						lbId, lbId, org[0], org[1], org[2], lbId, lbId, lbId, lbId, lbId);
					ExecCmd(lavaBurnCmd);
					break;
				}
			}

			bool isFriend = (strstr(className, "Citizen") != nullptr ||
			                 strstr(className, "Barney") != nullptr ||
			                 strstr(className, "Alyx") != nullptr ||
			                 strstr(className, "Vortigaunt") != nullptr ||
			                 strstr(className, "Monk") != nullptr ||
			                 strstr(className, "Eli") != nullptr ||
			                 strstr(className, "Kleiner") != nullptr ||
			                 strstr(className, "Mossman") != nullptr ||
			                 strstr(className, "Dog") != nullptr);

			bool isArmed = false;
			if (strstr(className, "Combine") != nullptr ||
			    strstr(className, "MetroPolice") != nullptr ||
			    strstr(className, "Hunter") != nullptr ||
			    strstr(className, "Turret") != nullptr ||
			    strstr(className, "Strider") != nullptr ||
			    strstr(className, "Gunship") != nullptr ||
			    strstr(className, "Helicopter") != nullptr ||
			    strstr(className, "Sniper") != nullptr) {
				isArmed = true;
			} else if (uint32_t *pWpn = l4d2::Netvar<uint32_t>(ent, "m_hActiveWeapon")) {
				uint32_t hActiveWpn = *pWpn;
				if (hActiveWpn != 0xFFFFFFFF && hActiveWpn != 0) {
					isArmed = true;
				}
			}

			uint32_t actorFlags = isFriend ? proto::kActorFriend : proto::kActorHostile;
			if (isArmed) {
				actorFlags |= proto::kActorArmed;
			}

			proto::ActorRecord &rec = tbl->actors[count];
			rec.entIndex = i;
			rec.flags = actorFlags;
			rec.x = org[0] * (float)proto::kMetersPerUnit;
			rec.y = (org[2] * (float)proto::kMetersPerUnit) + (float)kYOffset;
			rec.z = -org[1] * (float)proto::kMetersPerUnit;
			rec.yaw = 0.0f;

			float w = 0.6f;
			float h = 1.8f;
			if (strstr(className, "Headcrab")) {
				w = 0.4f; h = 0.4f;
			} else if (strstr(className, "Zombie")) {
				w = 0.7f; h = 1.9f;
			} else if (strstr(className, "AntlionGuard")) {
				w = 2.0f; h = 2.5f;
			} else if (strstr(className, "Scanner") || strstr(className, "Manhack")) {
				w = 0.5f; h = 0.5f;
			}
			rec.width = w;
			rec.height = h;
			rec.healthFrac = 1.0f;
			rec.classId = 0;
			strncpy(rec.name, className, sizeof(rec.name) - 1);
			rec.name[sizeof(rec.name) - 1] = '\0';
			count++;
		}

		tbl->count = count;
		MemoryBarrier();
		tbl->seq = seq + 2;
	}

	void HL2Bridge::ProcessEvents()
	{
		if (!m_shmView || !IsInPlayableWorld()) return;
		uint64_t head = *(uint64_t*)(m_shmView + proto::kOffEventRing + proto::kEventRingHeadOff);
		uint64_t tail = *(uint64_t*)(m_shmView + proto::kOffEventRing + proto::kEventRingTailOff);
		if (tail >= head) return;

		static void *s_engineServer = nullptr;
		static void *s_serverEnts = nullptr;
		if (!s_engineServer) {
			HMODULE hEngine = GetModuleHandleA("engine.dll");
			if (hEngine) {
				CreateInterfaceFn engineFactory = (CreateInterfaceFn)GetProcAddress(hEngine, "CreateInterface");
				if (engineFactory) s_engineServer = engineFactory("VEngineServer021", nullptr);
			}
		}
		if (!s_serverEnts) {
			HMODULE hServer = GetModuleHandleA("server.dll");
			if (hServer) {
				CreateInterfaceFn serverFactory = (CreateInterfaceFn)GetProcAddress(hServer, "CreateInterface");
				if (serverFactory) s_serverEnts = serverFactory("ServerGameEnts001", nullptr);
			}
		}

		while (tail < head) {
			uint64_t off = proto::kOffEventRing + proto::kEventRingDataOff + (tail % proto::kEventRingEntries) * sizeof(proto::McEvent);
			proto::McEvent *ev = (proto::McEvent*)(m_shmView + off);
			if (ev->type == proto::kEvHitActor) {
				int targetEnt = ev->entIndex;
				bool isCrit = ((ev->flags & 1) != 0) || (GetTickCount() < m_lastFallingUntilMs);
				int dmg = (int)std::max(ev->a * 3.5f, 25.0f);
				if (isCrit && (ev->flags & 1) == 0) {
					dmg = (int)std::round(dmg * 1.5f);
				}

				Vector entPos{0, 0, 0};
				Vector origEntPos{0, 0, 0};
				bool foundPos = false;
				void *pPlayerEnt = GetLocalPlayerEntity();
				if (targetEnt > 1 && m_entityList) {
					void **evt = *(void***)m_entityList;
					typedef void* (__thiscall *GetClientEntityFn)(void*, int);
					GetClientEntityFn getEntity = (GetClientEntityFn)evt[3];
					void *ent = getEntity(m_entityList, targetEnt);
					if (ent && ent != pPlayerEnt && !IsBadReadPtr(ent, sizeof(void*))) {
						void *renderable = (char*)ent + 4;
						if (!IsBadReadPtr(renderable, sizeof(void*))) {
							void **rendVt = *(void***)renderable;
							if (!IsBadReadPtr(rendVt, sizeof(void*) * 2)) {
								typedef const float* (__thiscall *GetOriginFn)(void*);
								const float *org = ((GetOriginFn)rendVt[1])(renderable);
								if (org && !IsBadReadPtr(org, sizeof(float) * 3)) {
									entPos = Vector{ org[0], org[1], org[2] };
									origEntPos = entPos;
									foundPos = true;
								}
							}
						}
					}
				}

				Vector playerPos{0, 0, 0};
				bool hasPlayerPos = GetPlayerPos(playerPos);
				Vector kbDir{ ev->b, -ev->c, 0.0f };
				float kbLen = std::sqrt(kbDir.x * kbDir.x + kbDir.y * kbDir.y);
				if (kbLen > 1e-4f) {
					kbDir.x /= kbLen;
					kbDir.y /= kbLen;
				} else if (foundPos && hasPlayerPos) {
					float dx = origEntPos.x - playerPos.x;
					float dy = origEntPos.y - playerPos.y;
					float dLen = std::sqrt(dx * dx + dy * dy);
					if (dLen > 1e-3f) {
						kbDir.x = dx / dLen;
						kbDir.y = dy / dLen;
					}
				}

				if (foundPos && hasPlayerPos) {
					float dx = entPos.x - playerPos.x;
					float dy = entPos.y - playerPos.y;
					float dist = std::sqrt(dx * dx + dy * dy);
					if (dist < 24.0f) {
						foundPos = false;
					} else if (dist < 60.0f) {
						// Shift point_hurt away from player so splash radius never touches !player
						entPos.x += (dx / dist) * 20.0f;
						entPos.y += (dy / dist) * 20.0f;
					}
				}

				DWORD nowHitMs = GetTickCount();
				int applyDmg = dmg;
				if (targetEnt > 1 && targetEnt == m_lastHitEntIndex && (nowHitMs - m_lastHitTimeMs) < 160) {
					applyDmg = std::max(0, dmg - m_lastHitAppliedDmg);
					m_lastHitAppliedDmg += applyDmg;
				} else if (targetEnt > 1) {
					m_lastHitEntIndex = targetEnt;
					m_lastHitTimeMs = nowHitMs;
					m_lastHitAppliedDmg = applyDmg;
				}

				if (targetEnt > 1 && foundPos) {
					float kbStrength = (ev->d > 0.05f) ? ev->d : 0.55f;
					StartNpcKnockback(targetEnt, origEntPos, kbDir, kbStrength, isCrit, applyDmg);
					if (isCrit && (ev->flags & 1) == 0) {
						int32_t mcX100 = (int32_t)std::round(origEntPos.x * proto::kMetersPerUnit * 100.0);
						int32_t mcY100 = (int32_t)std::round(((origEntPos.z + 40.0f) * proto::kMetersPerUnit + 64.0) * 100.0);
						int32_t mcZ100 = (int32_t)std::round(-origEntPos.y * proto::kMetersPerUnit * 100.0);
						PushInput(99, 1, mcX100, mcY100, mcZ100);
					}
				}

				m_ignoreSelfDamageUntil = nowHitMs + 250;
				if (foundPos) {
					if (applyDmg > 0) {
						static uint32_t s_hurtCounter = 0;
						uint32_t hurtId = ++s_hurtCounter;
						int safeHp = (m_lastSyncedHp > 0) ? m_lastSyncedHp : 100;
						bool isOnFire = (ev->flags & 4) != 0;
						int dmgType = isOnFire ? 8 : (isCrit ? 64 : 128); // 8: DMG_BURN, 64: DMG_BLAST, 128: DMG_CLUB

						char cmd[384];
						snprintf(cmd, sizeof(cmd),
							"ent_create point_hurt targetname mc_hrt_%u; "
							"ent_fire mc_hrt_%u addoutput \"origin %.2f %.2f %.2f\"; "
							"ent_fire mc_hrt_%u addoutput \"Damage %d\"; "
							"ent_fire mc_hrt_%u addoutput \"DamageRadius 28\"; "
							"ent_fire mc_hrt_%u addoutput \"DamageType %d\"; "
							"ent_fire mc_hrt_%u Hurt; "
							"ent_fire mc_hrt_%u Kill; "
							"ent_fire !player sethealth %d",
							hurtId, hurtId, entPos.x, entPos.y, entPos.z + 32.0f,
							hurtId, applyDmg, hurtId, hurtId, dmgType, hurtId, hurtId, safeHp);
						ExecCmd(cmd);
					}
				} else {
					HitCurrentTarget(dmg);
				}
			} else if (ev->type == proto::kEvExplosion) {
				float sx = ev->a * (float)proto::kUnitsPerBlock;
				float sz = (ev->b - 64.0f) * (float)proto::kUnitsPerBlock;
				float sy = -ev->c * (float)proto::kUnitsPerBlock;
				int mag = (int)std::max(ev->d * 40.0f, 150.0f);
				float radius = std::max(ev->d * 60.0f, 300.0f);

				// Kill collision props for any placed blocks within explosion radius
				float expR = ev->d + 1.5f;
				float expRSq = expR * expR;
				std::vector<std::tuple<int, int, int>> toRemove;
				for (const auto &bPos : m_placedBlocks) {
					float dx = (std::get<0>(bPos) + 0.5f) - ev->a;
					float dy = (std::get<1>(bPos) + 0.5f) - ev->b;
					float dz = (std::get<2>(bPos) + 0.5f) - ev->c;
					if (dx * dx + dy * dy + dz * dz <= expRSq) {
						toRemove.push_back(bPos);
					}
				}
				for (const auto &bPos : toRemove) {
					int bx = std::get<0>(bPos);
					int by = std::get<1>(bPos);
					int bz = std::get<2>(bPos);
					char delCmd[128];
					snprintf(delCmd, sizeof(delCmd), "script L4C.Clear(%d,%d,%d)", bx, by, bz);
					ExecCmd(delCmd);
					m_placedBlocks.erase(bPos);
				}

				static uint32_t s_expCounter = 0;
				uint32_t expId = ++s_expCounter;
				char cmd[320];
				snprintf(cmd, sizeof(cmd),
					"ent_create env_explosion targetname mc_exp_%u; "
					"ent_fire mc_exp_%u addoutput \"origin %.2f %.2f %.2f\"; "
					"ent_fire mc_exp_%u addoutput \"iMagnitude %d\"; "
					"ent_fire mc_exp_%u addoutput \"iRadiusOverride %.1f\"; "
					"ent_fire mc_exp_%u Explode; "
					"ent_fire mc_exp_%u Kill",
					expId, expId, sx, sy, sz, expId, mag, expId, radius, expId, expId);
				ExecCmd(cmd);
			} else if (ev->type == proto::kEvTeleport) {
				float sx = ev->a * (float)proto::kUnitsPerBlock;
				float sz = (ev->b - 64.0f) * (float)proto::kUnitsPerBlock + 16.0f;
				float sy = -ev->c * (float)proto::kUnitsPerBlock;

				char cmd[128];
				snprintf(cmd, sizeof(cmd), "setpos %.2f %.2f %.2f", sx, sy, sz);
				ExecCmd(cmd);

				m_lastPosX = sx;
				m_lastPosY = sy;
				m_lastScanPos = Vector{ sx, sy, sz };
				m_lastSentMcPos = Vector{ sx, sy, sz };
				m_hasLastSentMcPos = true;
				m_hl2OverrideUntil = GetTickCount() + 400;
				m_teleportSeq++;
				ScanCollision(Vector{ sx, sy, sz }, 10);

				auto *hs = (proto::HostState*)(m_shmView + proto::kOffHostState);
				if (hs) {
					hs->posX = ev->a;
					hs->posY = ev->b + 1.0;
					hs->posZ = ev->c;
					hs->teleportSeq = m_teleportSeq;
				}
			} else if (ev->type == proto::kEvPlayerDied) {
				m_placedBlocks.clear();
				m_placedLights.clear();
				ExecCmd("script L4C.ClearAll(); kill");
			} else if (ev->type == proto::kEvClearBlocks) {
				m_placedBlocks.clear();
				m_placedLights.clear();
				ExecCmd("script L4C.ClearAll()");
			} else if (ev->type == proto::kEvBlockPlaced) {
				float sx = (ev->a + 0.5f) * (float)proto::kUnitsPerBlock;
				float sy = -(ev->c + 0.5f) * (float)proto::kUnitsPerBlock;
				float sz = (ev->b + 0.5f - 64.0f) * (float)proto::kUnitsPerBlock;
				int ibx = (int)std::round(ev->a);
				int iby = (int)std::round(ev->b);
				int ibz = (int)std::round(ev->c);

				int lightLevel = (int)ev->d;
				int lightType = ev->flags;
				bool hasCollision = (ev->weapon != 0);

				if (lightLevel > 0) {
					m_placedLights.push_back({ ibx, iby, ibz, lightLevel, lightType, lightLevel >= 15 && !hasCollision });
				}

				if (hasCollision) {
					m_placedBlocks.insert({ibx, iby, ibz});

					// L4D2's server has no prop_physics_override or modelscale: an exact-size env_physics_blocker from
					// left4craft.nut, with hit points the infected wear down (contract section 5)
					const float u = (float)proto::kUnitsPerBlock;
					char cmd[256];
					snprintf(cmd, sizeof(cmd), "script L4C.Block(%d,%d,%d,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%d)",
						ibx, iby, ibz, ibx * u, -(ibz + 1) * u, (iby - 64) * u, (ibx + 1) * u, -ibz * u, (iby - 63) * u, 300);
					ExecCmd(cmd);
				}
			} else if (ev->type == proto::kEvBlockBroken) {
				int ibx = (int)std::round(ev->a);
				int iby = (int)std::round(ev->b);
				int ibz = (int)std::round(ev->c);
				m_placedBlocks.erase({ibx, iby, ibz});
				for (auto it = m_placedLights.begin(); it != m_placedLights.end(); ) {
					if (it->bx == ibx && it->by == iby && it->bz == ibz) {
						it = m_placedLights.erase(it);
					} else {
						++it;
					}
				}
				char cmd[128];
				snprintf(cmd, sizeof(cmd), "script L4C.Clear(%d,%d,%d)", ibx, iby, ibz);
				ExecCmd(cmd);
			}
			tail++;
		}
		*(uint64_t*)(m_shmView + proto::kOffEventRing + proto::kEventRingTailOff) = tail;
	}

	void HL2Bridge::HitPicker(int extraDmg)
	{
	}

	void HL2Bridge::HitCurrentTarget(int dmg)
	{
		if (!m_engineTrace || !IsInPlayableWorld()) return;
		Vector pos{0, 0, 0};
		QAngle angles{0, 0, 0};
		if (!GetPlayerPos(pos) || !GetAngles(angles)) return;

		float radYaw = angles.yaw * 3.14159265f / 180.0f;
		float radPitch = -angles.pitch * 3.14159265f / 180.0f;
		Vector fwd;
		fwd.x = std::cos(radPitch) * std::cos(radYaw);
		fwd.y = std::cos(radPitch) * std::sin(radYaw);
		fwd.z = std::sin(radPitch);

		bool isDucking = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		Vector start = pos;
		start.z += isDucking ? l4d2::kEyeDuck : l4d2::kEyeStand;
		Vector end;
		end.x = start.x + fwd.x * 120.0f;
		end.y = start.y + fwd.y * 120.0f;
		end.z = start.z + fwd.z * 120.0f;

		Ray_t ray;
		ray.Init(start, end);
		trace_t tr;
		std::memset(&tr, 0, sizeof(tr));
		CTraceFilterCombat filter;
		filter.pPlacedBlocks = &m_placedBlocks;
		if (m_entityList && m_engineClient) {
			void **evt = *(void***)m_engineClient;
			typedef int (__thiscall *GetLocalPlayerFn)(void*);
			int localPlayer = ((GetLocalPlayerFn)evt[12])(m_engineClient);
			if (localPlayer > 0) {
				void **lvt = *(void***)m_entityList;
				typedef void* (__thiscall *GetEntityFn)(void*, int);
				filter.pPlayer = ((GetEntityFn)lvt[3])(m_entityList, localPlayer);
			}
		}

		void **vt = *(void***)m_engineTrace;
		typedef void (__thiscall *TraceRayFn)(void*, const Ray_t&, unsigned int, ITraceFilter*, trace_t*);
		// 0x4600400B = MASK_SHOT | CONTENTS_GRATE (includes CONTENTS_HITBOX and CONTENTS_DEBRIS for NPCs and props)
		((TraceRayFn)vt[l4d2::kTraceRay])(m_engineTrace, ray, 0x4600400B, &filter, &tr);

		if (tr.fraction < 1.0f && tr.m_pEnt && tr.m_pEnt != filter.pPlayer && IsHandleEntityClient(tr.m_pEnt)) {
			void *networkable = (char*)tr.m_pEnt + 8;
			const char *className = "";
			int hitEntIndex = -1;
			if (!IsBadReadPtr(networkable, sizeof(void*))) {
				void **netVt = *(void***)networkable;
				if (!IsBadReadPtr(netVt, sizeof(void*) * 10)) {
					typedef void* (__thiscall *GetClientClassFn)(void*);
					void *clientClass = ((GetClientClassFn)netVt[l4d2::kNetGetClientClass])(networkable);
					if (clientClass && !IsBadReadPtr(clientClass, sizeof(void*) * 3)) {
						className = SafeClassName(clientClass);
					}
					typedef int (__thiscall *EntIndexFn)(void*);
					hitEntIndex = ((EntIndexFn)netVt[l4d2::kNetEntIndex])(networkable);
				}
			}

			bool isNpc = IsNpcClass(className);
			bool isProp = IsPropClass(className);
			if (isNpc || isProp) {
				Vector hitPos = tr.endpos;
				float dx = hitPos.x - pos.x;
				float dy = hitPos.y - pos.y;
				float dist = std::sqrt(dx * dx + dy * dy);
				if (dist < 18.0f) {
					return;
				}
				if (dist < 42.0f) {
					hitPos.x += (dx / dist) * 18.0f;
					hitPos.y += (dy / dist) * 18.0f;
				}

				DWORD nowHitMs = GetTickCount();
				bool isCrit = isNpc && (nowHitMs < m_lastFallingUntilMs);
				int clubDmg = isProp ? std::max(dmg, 45) : (isCrit ? (int)std::round(dmg * 1.5f) : dmg);

				if (isNpc && hitEntIndex > 1) {
					if (hitEntIndex == m_lastHitEntIndex && (nowHitMs - m_lastHitTimeMs) < 160) {
						return;
					}
					m_lastHitEntIndex = hitEntIndex;
					m_lastHitTimeMs = nowHitMs;
					m_lastHitAppliedDmg = clubDmg;

					Vector entOrg = tr.endpos;
					void *renderable = (char*)tr.m_pEnt + 4;
					if (!IsBadReadPtr(renderable, sizeof(void*))) {
						void **rendVt = *(void***)renderable;
						if (!IsBadReadPtr(rendVt, sizeof(void*) * 2)) {
							typedef const float* (__thiscall *GetOriginFn)(void*);
							const float *org = ((GetOriginFn)rendVt[1])(renderable);
							if (org && !IsBadReadPtr(org, sizeof(float) * 3)) {
								entOrg = Vector{ org[0], org[1], org[2] };
							}
						}
					}

					Vector kbDir{ fwd.x, fwd.y, 0.0f };
					float kbLen = std::sqrt(kbDir.x * kbDir.x + kbDir.y * kbDir.y);
					if (kbLen > 1e-4f) {
						kbDir.x /= kbLen;
						kbDir.y /= kbLen;
					} else if (dist > 1e-3f) {
						kbDir.x = dx / dist;
						kbDir.y = dy / dist;
					}
					bool sprinting = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
					float kbStrength = sprinting ? 0.85f : 0.55f;
					StartNpcKnockback(hitEntIndex, entOrg, kbDir, kbStrength, isCrit, clubDmg);

					int32_t mcX100 = (int32_t)std::round(tr.endpos.x * proto::kMetersPerUnit * 100.0);
					int32_t mcY100 = (int32_t)std::round((tr.endpos.z * proto::kMetersPerUnit + 64.0) * 100.0);
					int32_t mcZ100 = (int32_t)std::round(-tr.endpos.y * proto::kMetersPerUnit * 100.0);
					PushInput(99, isCrit ? 1 : 0, mcX100, mcY100, mcZ100);
				}

				m_ignoreSelfDamageUntil = nowHitMs + 250;
				static uint32_t s_meleeCounter = 0;
				uint32_t hurtId = ++s_meleeCounter;
				int safeHp = (m_lastSyncedHp > 0) ? m_lastSyncedHp : 100;
				int dmgType = isCrit ? 64 : 128;
				char cmd[384];
				snprintf(cmd, sizeof(cmd),
					"ent_create point_hurt targetname mc_ml_%u; "
					"ent_fire mc_ml_%u addoutput \"origin %.2f %.2f %.2f\"; "
					"ent_fire mc_ml_%u addoutput \"Damage %d\"; "
					"ent_fire mc_ml_%u addoutput \"DamageRadius 22\"; "
					"ent_fire mc_ml_%u addoutput \"DamageType %d\"; "
					"ent_fire mc_ml_%u Hurt; "
					"ent_fire mc_ml_%u Kill; "
					"ent_fire !player sethealth %d",
					hurtId, hurtId, hitPos.x, hitPos.y, hitPos.z,
					hurtId, clubDmg, hurtId, hurtId, dmgType, hurtId, hurtId, safeHp);
				ExecCmd(cmd);
			}
		}
	}

	// The engine's own networkable for entity i must be this entity's +8 sub-object; otherwise the object isn't a
	// C_BaseEntity laid out as hl2sdk says and none of its "networkable" virtuals may be called (Left4Craft, L4D2).
	bool HL2Bridge::IsNetworkableAt(int index, void *ent)
	{
		if (!m_entityList || !ent || IsBadReadPtr(ent, 16)) return false;
		void **lvt = *(void***)m_entityList;
		typedef void* (__thiscall *GetNetworkableFn)(void*, int);
		void *net = ((GetNetworkableFn)lvt[0])(m_entityList, index);
		return net != nullptr && net == (void*)((char*)ent + 8);
	}

	// What traces hand back is an IHandleEntity: a client entity (handle index < 2048 whose networkable is the
	// engine's) or something else, e.g. a static prop, where none of the entity offsets apply.
	bool HL2Bridge::IsHandleEntityClient(void *handleEntity)
	{
		if (!handleEntity || IsBadReadPtr(handleEntity, 16)) return false;
		void **vt = *(void***)handleEntity;
		if (!vt || IsBadReadPtr(vt, sizeof(void*) * 3)) return false;
		typedef const uint32_t& (__thiscall *GetRefEHandleFn)(void*);
		const uint32_t &h = ((GetRefEHandleFn)vt[2])(handleEntity); // IHandleEntity: dtor, SetRefEHandle, GetRefEHandle
		if (IsBadReadPtr(&h, 4) || h == 0xFFFFFFFFu) return false;
		int index = (int)(h & 0xFFF);
		return index > 0 && index < 2048 && IsNetworkableAt(index, handleEntity);
	}

	bool HL2Bridge::TraceWorldRay(const Vector &start, const Vector &end, trace_t &tr)
	{
		if (!m_engineTrace || !IsInPlayableWorld()) return false;
		void **vt = *(void***)m_engineTrace;
		if (!vt || !vt[l4d2::kTraceRay]) return false;
		typedef void (__thiscall *TraceRayFn)(void*, const Ray_t&, unsigned int, ITraceFilter*, trace_t*);
		TraceRayFn traceRay = (TraceRayFn)vt[l4d2::kTraceRay];

		CTraceFilterWorldOnly filter;
		Ray_t ray;
		ray.Init(start, end);
		std::memset(&tr, 0, sizeof(tr));
		traceRay(m_engineTrace, ray, 0x200400B, &filter, &tr);
		return true;
	}

	void HL2Bridge::ComputeUnstickPos(const Vector &playerPos, int ibx, int iby, int ibz, Vector &outPos)
	{
		float bMinX = (float)ibx * (float)proto::kUnitsPerBlock;
		float bMaxX = ((float)ibx + 1.0f) * (float)proto::kUnitsPerBlock;
		float bMinY = -((float)ibz + 1.0f) * (float)proto::kUnitsPerBlock;
		float bMaxY = -(float)ibz * (float)proto::kUnitsPerBlock;
		float bMinZ = ((float)iby - 64.0f) * (float)proto::kUnitsPerBlock;
		float bMaxZ = ((float)iby + 1.0f - 64.0f) * (float)proto::kUnitsPerBlock;

		// 1. Always try pushing UP first (if the 2 blocks above and HL2 ceiling are clear)
		bool upClear = (m_placedBlocks.find({ibx, iby + 1, ibz}) == m_placedBlocks.end()) &&
		               (m_placedBlocks.find({ibx, iby + 2, ibz}) == m_placedBlocks.end());
		if (upClear) {
			trace_t tr;
			Vector upStart{ playerPos.x, playerPos.y, std::max(playerPos.z + 18.0f, bMaxZ - 8.0f) };
			Vector upEnd{ playerPos.x, playerPos.y, bMaxZ + 73.0f };
			if (TraceWorldRay(upStart, upEnd, tr) && (tr.startsolid || tr.fraction < 0.98f)) {
				upClear = false;
			}
		}

		if (upClear) {
			outPos.x = playerPos.x;
			outPos.y = playerPos.y;
			outPos.z = bMaxZ + 1.5f;
			return;
		}

		// 2. Pushing UP is blocked -> push horizontally to the nearest free side
		struct SideCandidate {
			float x, y;
			float traceEndX, traceEndY;
			int nbx, nbz;
			float dist;
		};
		SideCandidate cands[4] = {
			{ bMinX - 17.5f, playerPos.y, bMinX - 33.0f, playerPos.y, ibx - 1, ibz, std::abs((bMinX - 17.5f) - playerPos.x) },
			{ bMaxX + 17.5f, playerPos.y, bMaxX + 33.0f, playerPos.y, ibx + 1, ibz, std::abs((bMaxX + 17.5f) - playerPos.x) },
			{ playerPos.x, bMinY - 17.5f, playerPos.x, bMinY - 33.0f, ibx, ibz + 1, std::abs((bMinY - 17.5f) - playerPos.y) },
			{ playerPos.x, bMaxY + 17.5f, playerPos.x, bMaxY + 33.0f, ibx, ibz - 1, std::abs((bMaxY + 17.5f) - playerPos.y) }
		};
		std::sort(cands, cands + 4, [](const SideCandidate &a, const SideCandidate &b) {
			return a.dist < b.dist;
		});

		int footBy = (int)std::floor(playerPos.z / (float)proto::kUnitsPerBlock + 64.0f + 0.1f);
		float centerBx = (bMinX + bMaxX) * 0.5f;
		float centerBy = (bMinY + bMaxY) * 0.5f;
		float traceZ = playerPos.z + 36.0f;

		int chosenIdx = 0;
		for (int i = 0; i < 4; ++i) {
			bool sideBlocked = (m_placedBlocks.find({cands[i].nbx, footBy, cands[i].nbz}) != m_placedBlocks.end()) ||
			                   (m_placedBlocks.find({cands[i].nbx, footBy + 1, cands[i].nbz}) != m_placedBlocks.end());
			if (!sideBlocked) {
				trace_t tr;
				Vector sStart{ centerBx, centerBy, traceZ };
				Vector sEnd{ cands[i].traceEndX, cands[i].traceEndY, traceZ };
				if (TraceWorldRay(sStart, sEnd, tr) && (tr.startsolid || tr.fraction < 0.95f)) {
					sideBlocked = true;
				}
			}
			if (!sideBlocked) {
				chosenIdx = i;
				break;
			}
		}

		outPos.x = cands[chosenIdx].x;
		outPos.y = cands[chosenIdx].y;
		outPos.z = playerPos.z;
	}

	void HL2Bridge::StartUnstickLift(const Vector &targetPos, int ibx, int iby, int ibz)
	{
		m_unstickActive = true;
		m_unstickUntil = GetTickCount() + 90;
		m_unstickPos = targetPos;
		m_unstickBx = ibx;
		m_unstickBy = iby;
		m_unstickBz = ibz;

		char unstickCmd[256];
		snprintf(unstickCmd, sizeof(unstickCmd),
			"script L4C.Unsolid(%d,%d,%d,0.15); "
			"ent_fire !player addoutput \"basevelocity 0 0 0\"; "
			"ent_fire !player addoutput \"velocity 0 0 0\"; "
			"setpos %.2f %.2f %.2f",
			ibx, iby, ibz, targetPos.x, targetPos.y, targetPos.z);
		ExecCmd(unstickCmd);
	}

	void HL2Bridge::StartNpcKnockback(int entIndex, const Vector &entPos, const Vector &dir, float strength, bool isCrit, int applyDmg)
	{
		if (entIndex <= 1) return;
		DWORD now = GetTickCount();
		DWORD flashDuration = 320;

		bool updated = false;
		for (auto &kb : m_knockbacks) {
			if (kb.entIndex == entIndex) {
				kb.lastPos = entPos;
				kb.startMs = now;
				kb.flashEndMs = now + flashDuration;
				kb.isCrit = isCrit;
				updated = true;
				break;
			}
		}
		if (!updated) {
			if (m_knockbacks.size() >= 24) {
				m_knockbacks.erase(m_knockbacks.begin());
			}
			m_knockbacks.push_back(ActiveKnockback{ entIndex, entPos, now, now + flashDuration, isCrit });
		}

		float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
		if (len > 1e-4f) {
			Vector nDir{ dir.x / len, dir.y / len, 0.0f };
			static uint32_t s_pushCounter = 0;
			uint32_t pushId = ++s_pushCounter;
			Vector pushPos{
				entPos.x - nDir.x * 24.0f,
				entPos.y - nDir.y * 24.0f,
				entPos.z + 4.0f
			};
			int mag = isCrit ? 260 : 190;
			if (strength > 0.8f) mag = (int)(mag * 1.3f);

			char cmd[320];
			snprintf(cmd, sizeof(cmd),
				"ent_create point_push targetname mc_kbp_%u; "
				"ent_fire mc_kbp_%u addoutput \"origin %.2f %.2f %.2f\"; "
				"ent_fire mc_kbp_%u addoutput \"magnitude %d\"; "
				"ent_fire mc_kbp_%u addoutput \"radius 64\"; "
				"ent_fire mc_kbp_%u addoutput \"spawnflags 28\"; "
				"ent_fire mc_kbp_%u Enable; "
				"ent_fire mc_kbp_%u Kill \"\" 0.08",
				pushId, pushId, pushPos.x, pushPos.y, pushPos.z,
				pushId, mag, pushId, pushId, pushId, pushId);
			ExecCmd(cmd);
		}
	}

	void HL2Bridge::UpdateNpcKnockbacks()
	{
		if (m_knockbacks.empty() || !m_entityList || !IsInPlayableWorld()) {
			if (!IsInPlayableWorld()) m_knockbacks.clear();
			return;
		}

		void **evt = *(void***)m_entityList;
		typedef void* (__thiscall *GetClientEntityFn)(void*, int);
		GetClientEntityFn getClientEnt = (GetClientEntityFn)evt[3];
		void *pPlayerEnt = GetLocalPlayerEntity();

		DWORD now = GetTickCount();
		for (auto it = m_knockbacks.begin(); it != m_knockbacks.end(); ) {
			void *pClientEnt = getClientEnt(m_entityList, it->entIndex);
			bool validClientEnt = (pClientEnt && pClientEnt != pPlayerEnt && !IsBadReadPtr(pClientEnt, 16));

			bool isDead = false;
			if (!validClientEnt) {
				isDead = true;
			} else if (uint8_t *pLife = l4d2::Netvar<uint8_t>(pClientEnt, "m_lifeState")) {
				uint8_t lifeState = *pLife;
				if (lifeState != 0) {
					isDead = true;
				}
			}

			if (isDead || now >= it->flashEndMs) {
				if (validClientEnt) {
					if (uint8_t *clr = l4d2::Netvar<uint8_t>(pClientEnt, "m_clrRender")) {
						clr[0] = 255; clr[1] = 255; clr[2] = 255; clr[3] = 255;
					}
				}
				if (isDead) {
					ExecCmd("ent_fire prop_ragdoll* Color \"255 255 255\"; ent_fire prop_ragdoll* addoutput \"rendercolor 255 255 255\";");
				}
				it = m_knockbacks.erase(it);
				continue;
			}

			if (validClientEnt) {
				if (uint8_t *clr = l4d2::Netvar<uint8_t>(pClientEnt, "m_clrRender")) {
					clr[0] = 255; clr[1] = 255; clr[2] = 255; clr[3] = 255;
				}
			}

			++it;
		}
	}

	void HL2Bridge::ProcessInput()
	{
		HWND fgWnd = GetForegroundWindow();
		DWORD fgPid = 0;
		if (fgWnd) GetWindowThreadProcessId(fgWnd, &fgPid);
		if (fgPid != GetCurrentProcessId()) return;

		if (m_guiOpen) {
			ExecCmd("-forward; -back; -moveleft; -moveright; -jump; -duck; -speed; -attack; -attack2;");
			return; // Frozen while GUI is open!
		}

		if (!m_shmView) return;

		bool sprinting = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		if (sprinting != m_lastSprinting) {
			m_lastSprinting = sprinting;
			if (!m_mcPhysicsActive) {
				(void)sprinting; // L4D2 has no hl2_normspeed
			}
		}

		bool lbtn = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		if (lbtn != m_lastLButton) {
			m_lastLButton = lbtn;
			PushInput(proto::kInMouseButton, 1, lbtn ? 1 : 0);
			if (lbtn) {
				HitCurrentTarget(35);
			}
		}
		bool rbtn = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
		if (rbtn != m_lastRButton) {
			m_lastRButton = rbtn;
			PushInput(proto::kInMouseButton, 3, rbtn ? 1 : 0);
		}

		auto handleKey = [&](int vk, uint16_t scancode) {
			bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
			uint8_t idx = (uint8_t)(vk & 0xFF);
			if (down != m_keysDown[idx]) {
				m_keysDown[idx] = down;
				PushInput(proto::kInKey, scancode, down ? 1 : 0);
			}
		};

		handleKey('E', 8);          // 8 = 'E' (Inventory)
		handleKey('Q', 20);         // 20 = 'Q' (Drop)
		handleKey('T', 23);         // 23 = 'T' (Chat)
		handleKey(VK_OEM_2, 56);    // 56 = '/' (Command)
		handleKey(VK_RETURN, 40);   // 40 = Return / Enter
		handleKey(VK_ESCAPE, 41);   // 41 = Escape
		handleKey(VK_SPACE, 44);    // 44 = Space
		handleKey(VK_SHIFT, 225);   // 225 = LShift (Sneak / Duck)
		handleKey(VK_CONTROL, 224); // 224 = LCtrl (Sprint)
		handleKey('W', 26);
		handleKey('A', 4);
		handleKey('S', 22);
		handleKey('D', 7);
		handleKey('F', 9);
		handleKey(VK_F8, 65);       // 65 = F8: Left4Craft collision dump around Steve's feet (run.log)
		for (int i = 0; i < 9; i++) {
			handleKey('1' + i, 30 + i);
		}
		handleKey('0', 39);
	}

	void HL2Bridge::PushInput(uint16_t type, uint16_t code, int32_t a, int32_t b, int32_t c)
	{
		if (!m_shmView) return;
		uint64_t head = *(uint64_t*)(m_shmView + proto::kOffInputRing + proto::kInputRingHeadOff);
		uint64_t off = proto::kOffInputRing + proto::kInputRingDataOff + (head % proto::kInputRingEntries) * sizeof(proto::InputEvent);
		proto::InputEvent *ev = (proto::InputEvent*)(m_shmView + off);
		ev->type = type;
		ev->code = code;
		ev->a = a;
		ev->b = b;
		ev->c = c;
		MemoryBarrier();
		*(uint64_t*)(m_shmView + proto::kOffInputRing + proto::kInputRingHeadOff) = head + 1;
	}

	void HL2Bridge::PushTextInput(int32_t codepoint)
	{
		PushInput(proto::kInText, 0, codepoint, 0);
	}

	LRESULT CALLBACK HL2Bridge::SubclassWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		HL2Bridge &b = HL2Bridge::Get();

		if (uMsg == WM_INPUT) {
			if (b.m_guiOpen) {
				return 0; // Prevent Source engine from locking or centering cursor while GUI is open!
			}
		}

		if (b.IsGuiOpen()) {
			if (uMsg == WM_SETCURSOR) {
				SetCursor(LoadCursor(NULL, IDC_ARROW));
				return TRUE;
			}
			if (uMsg == WM_MOUSEMOVE) {
				int x = (short)LOWORD(lParam);
				int y = (short)HIWORD(lParam);
				b.m_cursorX = std::clamp(x, 0, b.m_viewW - 1);
				b.m_cursorY = std::clamp(y, 0, b.m_viewH - 1);
				b.PushInput(proto::kInCursor, 0, b.m_cursorX, b.m_cursorY);
				SetCursor(LoadCursor(NULL, IDC_ARROW));
				return 0;
			}
			if (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP) {
				b.PushInput(proto::kInMouseButton, 1, (uMsg == WM_LBUTTONDOWN) ? 1 : 0);
				return 0;
			}
			if (uMsg == WM_RBUTTONDOWN || uMsg == WM_RBUTTONUP) {
				b.PushInput(proto::kInMouseButton, 3, (uMsg == WM_RBUTTONDOWN) ? 1 : 0);
				return 0;
			}
			if (uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP) {
				b.PushInput(proto::kInMouseButton, 2, (uMsg == WM_MBUTTONDOWN) ? 1 : 0);
				return 0;
			}
			if (uMsg == WM_MOUSEWHEEL) {
				short delta = (short)HIWORD(wParam);
				b.PushInput(proto::kInScroll, 0, delta);
				return 0;
			}
			if (uMsg == WM_CHAR) {
				int32_t cp = (int32_t)wParam;
				if (cp >= 32 || cp == 8 || cp == 13) {
					b.PushTextInput(cp);
				}
				return 0;
			}
			if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP) {
				bool down = (uMsg == WM_KEYDOWN);
				int vk = (int)wParam;
				uint16_t scancode = 0;
				switch (vk) {
					case 'E':       scancode = 8; break;
					case VK_RETURN: scancode = 40; break;
					case VK_ESCAPE: scancode = 41; break;
					case VK_BACK:   scancode = 42; break;
					case VK_TAB:    scancode = 43; break;
					case VK_LEFT:   scancode = 80; break;
					case VK_RIGHT:  scancode = 79; break;
					case VK_UP:     scancode = 82; break;
					case VK_DOWN:   scancode = 81; break;
					case VK_DELETE: scancode = 76; break;
					case VK_HOME:   scancode = 74; break;
					case VK_END:    scancode = 77; break;
					default: break;
				}
				if (scancode != 0) {
					b.PushInput(proto::kInKey, scancode, down ? 1 : 0);
				}
				return 0;
			}
		}
		// Minecraft owns movement: L4D2 must not walk, jump, crouch or walk-slow on these keys too (it has no
		// cl_forwardspeed / hl2_normspeed). ProcessInput still reads them with GetAsyncKeyState for Minecraft.
		if ((uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP) &&
		    b.m_mcPhysicsActive && !b.m_guiOpen && b.MovementKeysToMinecraft()) {
			switch (wParam) {
				case 'W': case 'A': case 'S': case 'D':
				case VK_SPACE: case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
				case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
					return 0;
				default: break;
			}
		}
		return CallWindowProcA(b.m_oldWndProc, hwnd, uMsg, wParam, lParam);
	}

	bool HL2Bridge::MovementKeysToMinecraft()
	{
		if (!m_engineClient) return false;
		void **vt = *(void***)m_engineClient;
		typedef bool (__thiscall *Fn)(void*);
		if (((Fn)vt[l4d2::kConIsVisible])(m_engineClient)) return false; // typing in L4D2's console
		CURSORINFO ci{ sizeof(ci) };
		return !(GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)); // an L4D2 menu shows the cursor
	}

	// left4craft.nut writes ems/left4craft/out.txt ("L4C1", "seq json" lines, "END"); a line stays until acked.
	// Only blockbreak (the infected broke a barricade) is used here: Minecraft breaks that block too.
	void HL2Bridge::PollVScript()
	{
		static char s_path[MAX_PATH] = {0};
		static long long s_lastSeq = 0;
		static char s_lastMap[64] = {0};
		if (!s_path[0]) {
			char exe[MAX_PATH];
			GetModuleFileNameA(nullptr, exe, MAX_PATH);
			char *slash = strrchr(exe, '\\');
			if (slash) *slash = 0;
			snprintf(s_path, sizeof(s_path), "%s\\left4dead2\\ems\\left4craft\\out.txt", exe);
		}
		if (strcmp(s_lastMap, m_lastMapName) != 0) {
			strncpy(s_lastMap, m_lastMapName, sizeof(s_lastMap) - 1);
			s_lastSeq = 0; // the VScript restarts its numbering on every map
		}
		FILE *f = fopen(s_path, "rb");
		if (!f) return;
		static char buf[64 * 1024];
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = 0;
		if (strncmp(buf, "L4C1\n", 5) != 0 || !strstr(buf, "\nEND\n")) return; // still being written
		long long last = s_lastSeq;
		for (char *line = buf + 5; line && *line; ) {
			char *next = strchr(line, '\n');
			if (next) *next++ = 0;
			if (strcmp(line, "END") == 0) break;
			long long seq = atoll(line);
			if (seq > s_lastSeq) {
				last = seq;
				const char *bb = strstr(line, "\"blockbreak\",\"p\":[");
				int bx, by, bz;
				if (bb && sscanf(bb + 18, "%d,%d,%d", &bx, &by, &bz) == 3) {
					m_placedBlocks.erase({bx, by, bz});
					PushInput(proto::kInBreakBlock, 0, bx, by, bz);
				}
			}
			line = next;
		}
		if (last != s_lastSeq) {
			s_lastSeq = last;
			char cmd[64];
			snprintf(cmd, sizeof(cmd), "script L4C.Ack(%lld)", last);
			ExecCmd(cmd);
		}
	}

	void HL2Bridge::HookWindow()
	{
		if (m_oldWndProc) return;
		HWND hwnd = FindWindowA("Valve001", nullptr);
		if (!hwnd) return;
		m_hValveWnd = hwnd;

		if (!g_realSetCursorPos) {
			HMODULE hUser32 = GetModuleHandleA("user32.dll");
			if (hUser32) {
				g_realSetCursorPos = (SetCursorPosFn)GetProcAddress(hUser32, "SetCursorPos");
				if (g_realSetCursorPos) {
					DWORD oldProt;
					if (VirtualProtect((void*)g_realSetCursorPos, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
						std::memcpy(g_savedCursorBytes, (void*)g_realSetCursorPos, 5);
						uint8_t jmp[5] = { 0xE9, 0, 0, 0, 0 };
						*(int32_t*)(jmp + 1) = (int32_t)((char*)Hook_SetCursorPos - (char*)g_realSetCursorPos - 5);
						std::memcpy((void*)g_realSetCursorPos, jmp, 5);
						VirtualProtect((void*)g_realSetCursorPos, 5, oldProt, &oldProt);
					}
				}
			}
		}

		RAWINPUTDEVICE rid;
		rid.usUsagePage = 0x01; // Generic Desktop Controls
		rid.usUsage = 0x02;     // Mouse
		rid.dwFlags = RIDEV_INPUTSINK;
		rid.hwndTarget = hwnd;
		RegisterRawInputDevices(&rid, 1, sizeof(rid));

		m_oldWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassWndProc);
	}

	void HL2Bridge::UnhookWindow()
	{
		if (g_realSetCursorPos && g_savedCursorBytes[0] != 0) {
			DWORD oldProt;
			if (VirtualProtect((void*)g_realSetCursorPos, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
				std::memcpy((void*)g_realSetCursorPos, g_savedCursorBytes, 5);
				VirtualProtect((void*)g_realSetCursorPos, 5, oldProt, &oldProt);
			}
			g_realSetCursorPos = nullptr;
		}
		if (m_oldWndProc && m_hValveWnd) {
			SetWindowLongPtrA(m_hValveWnd, GWLP_WNDPROC, (LONG_PTR)m_oldWndProc);
			m_oldWndProc = nullptr;
			m_hValveWnd = nullptr;
		}
	}
}
