#pragma once
#include <windows.h>

namespace hl2craft
{
namespace compositor
{
	bool try_register(void *module);
	void unregister(void *module);
	void set_active(bool active);
}
}
