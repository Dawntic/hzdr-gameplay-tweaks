#include "RenderPassNames.h"
#include "PCore/String.h"
#include "../ModConfiguration.h"

namespace HRZR::RenderPassNames
{
	// The gate: `cmp byte ptr [rip + flag], al` / `je <skip>` at the top of the command list open path,
	// immediately followed by `mov edx, [rbx + 0x14]` / `mov edi, edx` unpacking the RenderOrder bitfield.
	// Two read sites exist in the binary and no writer other than the zeroing CRT initializer.
	static uint8_t *GetGateFlag()
	{
		static const auto flag = Offsets::Signature("38 05 ? ? ? ? 0F 84 ? ? ? ? 8B 53 14 8B FA")
									.AsRipRelative(6)
									.ToPointer<uint8_t>();

		return flag;
	}

	bool IsAvailable()
	{
		return GetGateFlag() != nullptr;
	}

	bool IsEnabled()
	{
		const auto flag = GetGateFlag();
		return flag && *flag != 0;
	}

	void SetEnabled(bool Enabled)
	{
		const auto flag = GetGateFlag();

		if (!flag)
		{
			spdlog::warn("[PassNames] Gate flag not resolved, command list naming unavailable");
			return;
		}

		Memory::Patch(reinterpret_cast<uintptr_t>(flag), { static_cast<uint8_t>(Enabled ? 1 : 0) });
		spdlog::info("[PassNames] Command list naming {}", Enabled ? "enabled" : "disabled");
	}

	// The engine's SetName wrapper: converts the narrow String to UTF-16 and calls the object's vtable
	// slot 0x30, i.e. ID3D12Object::SetName. Hooked purely to mirror the names into the mod log.
	static void (*OriginalNameD3DObject)(void *Object, const String *Name);

	static void HookedNameD3DObject(void *Object, const String *Name)
	{
		if (ModConfiguration.RenderDoc.LogCommandListNames && Name && Name->data() && !Name->empty())
			spdlog::info("[PassName] {}", *Name);

		OriginalNameD3DObject(Object, Name);
	}

	DECLARE_HOOK_TRANSACTION(RenderPassNames)
	{
		const auto target = Offsets::Signature(
			"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 48 8B 1A 45 33 FF 48 8B 01 4C 8B F1");

		return Hooks::WriteJump(target, &HookedNameD3DObject, &OriginalNameD3DObject);
	};
}
