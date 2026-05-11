#include "../../ModConfiguration.h"
#include "../../ModCoreEvents.h"
#include "../Core/IStreamingManager.h"
#include "../DebugUI/DebugUI.h"
#include "NxD3DImpl.h"
#include "NxDXGIImpl.h"

#include <Windows.h>

namespace HRZR
{
	bool (*OriginalPresent)(NxDXGIImpl *DXGIImpl, void *a2);

	bool HookedPresent(NxDXGIImpl *Thisptr, void *a2)
	{
		const static bool initialized = []()
		{
			if (StreamingManager::GetInstance())
				StreamingManager::GetInstance()->m_CoreFileManager->RegisterEventHandler(&ModCoreEvents::GetInstance());

			return true;
		}();

		const static bool uiInitialized = [&]()
		{
			if (!ModConfiguration.EnableDebugMenu)
				return false;

			DebugUI::Initialize(Thisptr);
			return true;
		}();

		if (uiInitialized)
		{
			DebugUI::RenderUI();
			DebugUI::RenderUID3D(NxD3DImpl::GetSingleton(), Thisptr);
		}

		return OriginalPresent(Thisptr, a2);
	}

	DECLARE_HOOK_TRANSACTION(NxDXGIImpl)
	{
		HMODULE hD3D12 = GetModuleHandleW(L"d3d12.dll");
		HMODULE hD3D12Core = GetModuleHandleW(L"D3D12Core.dll");
		OutputDebugStringW(
			hD3D12 ? L"d3d12.dll ALREADY loaded in HookedPresent\n" : L"d3d12.dll not loaded yet in HookedPresent\n");
		OutputDebugStringW(
			hD3D12Core ? L"D3D12Core.dll ALREADY loaded in HookedPresent\n"
					   : L"D3D12Core.dll not loaded yet in HookedPresent\n");

		// Present vfunc is 10th index in NxDXGIImpl's virtual table
		const auto vtableEntryNxDXGIImpl = Offsets::Signature(
										 "48 8D 0D ? ? ? ? 66 89 68 08 48 89 08 40 88 68 0A 48 89 68 0C 48 89 68 18 48 89 68 20")
										 .AsRipRelative(7)
										 .AsAdjusted(sizeof(void *) * 10)
										 .ToPointer<uintptr_t>();

		Hooks::WriteJump(*vtableEntryNxDXGIImpl, &HookedPresent, &OriginalPresent);
	};
}
