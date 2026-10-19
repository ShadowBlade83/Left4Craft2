// Left 4 Dead 2 specifics that can't be constants: networked variable offsets, found by name.
// ClientClass / RecvTable / RecvProp layouts are hl2sdk branch l4d2 (client_class.h, dt_recv.h), 32-bit:
//   ClientClass { create, createEvent, char *name (+8), RecvTable *table (+12), next, classId }
//   RecvTable   { RecvProp *props (+0), int count (+4), decoder, const char *name }
//   RecvProp    60 bytes: name (+0), type (+4, 6 = DPT_DataTable), dataTable (+40), offset (+44)
#include "hl2_bridge.h"
#include <map>
#include <string>

namespace hl2craft::l4d2
{
	namespace
	{
		constexpr int kRecvPropSize = 60;
		constexpr int kDptDataTable = 6;

		int Find(const void *table, const char *name, int depth)
		{
			if (!table || depth > 8 || IsBadReadPtr(table, 8))
				return -1;
			const char *props = *(const char *const *)table;
			int count = *(const int *)((const char *)table + 4);
			if (!props || count <= 0 || count > 4096 || IsBadReadPtr(props, (size_t)count * kRecvPropSize))
				return -1;
			for (int i = 0; i < count; ++i) {
				const char *prop = props + i * kRecvPropSize;
				const char *propName = *(const char *const *)prop;
				int type = *(const int *)(prop + 4);
				const void *child = *(const void *const *)(prop + 40);
				int offset = *(const int *)(prop + 44);
				if (propName && !IsBadReadPtr(propName, 1) && std::strcmp(propName, name) == 0)
					return offset;
				if (type == kDptDataTable && child) {
					int inner = Find(child, name, depth + 1);
					if (inner >= 0)
						return offset + inner;
				}
			}
			return -1;
		}

		void *GetClientClass(void *ent)
		{
			if (!ent || IsBadReadPtr(ent, 12))
				return nullptr;
			void *networkable = (char *)ent + 8;
			void **vt = *(void ***)networkable;
			if (!vt || IsBadReadPtr(vt, sizeof(void *) * 3))
				return nullptr;
			typedef void *(__thiscall *GetClientClassFn)(void *);
			void *cc = ((GetClientClassFn)vt[kNetGetClientClass])(networkable);
			return cc && !IsBadReadPtr(cc, 16) ? cc : nullptr;
		}

		std::map<std::pair<const void *, std::string>, int> g_cache;
	}

	const char *ClassName(void *ent)
	{
		return SafeClassName(GetClientClass(ent));
	}

	int NetvarOffset(void *ent, const char *name)
	{
		void *cc = GetClientClass(ent);
		if (!cc || !SafeClassName(cc)[0])
			return -1; // not a real networkable: its RecvTable pointer can't be trusted either
		auto key = std::make_pair((const void *)cc, std::string(name));
		auto it = g_cache.find(key);
		if (it != g_cache.end())
			return it->second;
		int off = Find(*(void **)((char *)cc + 12), name, 0);
		g_cache[key] = off;
		return off;
	}
}
