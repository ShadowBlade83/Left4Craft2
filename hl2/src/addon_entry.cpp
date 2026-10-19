#include <windows.h>
#include "compositor.h"

extern "C" __declspec(dllexport) const char *NAME = "HL2Minecraft";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Half-Life 2 x Minecraft Compositor";

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hinstDLL);
		hl2craft::compositor::try_register(hinstDLL);
		hl2craft::compositor::set_active(true);
		break;
	case DLL_PROCESS_DETACH:
		hl2craft::compositor::unregister(hinstDLL);
		break;
	}
	return TRUE;
}
