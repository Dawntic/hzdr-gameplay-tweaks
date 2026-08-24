#include "../../ModConfiguration.h"
#include "../../ModCoreEvents.h"
#include "../Core/IStreamingManager.h"
#include "../DebugUI/DebugUI.h"
#include "NxD3DImpl.h"
#include "NxDXGIImpl.h"

#include "../../test.h"
#include "../RenderDocDiag.h"
#include <Windows.h>

namespace HRZR
{

	HWND GetGameWindow()
	{
		struct Data
		{
			DWORD pid;
			HWND hwnd;
		};
		Data data = { GetCurrentProcessId(), nullptr };

		EnumWindows(
			[](HWND hwnd, LPARAM lParam) -> BOOL
		{
			auto *d = reinterpret_cast<Data *>(lParam);
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid == d->pid && IsWindowVisible(hwnd))
			{
				d->hwnd = hwnd;
				return FALSE;
			}
			return TRUE;
		},
			reinterpret_cast<LPARAM>(&data));

		return data.hwnd;
	}

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

		// The first present is the earliest point where the device, queue and swapchain all exist, so it's
		// the most informative snapshot of who hooked what. It's also the first point at which the engine's
		// own -attach_renderdoc load has definitely happened.
		const static bool renderDocReady = [&]()
		{
			if (!renderDocApi)
				AttachToLoadedRenderDoc();

			if (ModConfiguration.RenderDoc.Diagnostics)
			{
				RenderDocDiag::Report("first Present");
				RenderDocDiag::ReportLiveObjects(NxD3DImpl::GetSingleton(), Thisptr);
				RenderDocDiag::ProbeCreation();
			}

			return renderDocApi != nullptr;
		}();

		if (uiInitialized)
		{
			DebugUI::RenderUI();
			DebugUI::RenderUID3D(NxD3DImpl::GetSingleton(), Thisptr);
		}

		auto ret = OriginalPresent(Thisptr, a2);

		// Start/EndFrameCapture must be given the same device+window pair
		static void *s_captureDevice = nullptr;
		static HWND s_captureWindow = nullptr;
		static bool s_capturing = false;

		if (renderDocApi && s_capturing)
		{
			s_capturing = false;
			uint32_t result = renderDocApi->EndFrameCapture(s_captureDevice, s_captureWindow);
			spdlog::info("[RenderDoc] EndFrameCapture result: {} — total: {}", result, renderDocApi->GetNumCaptures());
		}

		// Start capture AFTER present — captures everything in the next frame
		if (renderDocApi && g_wantCapture.exchange(false))
		{
			auto *d3d = NxD3DImpl::GetSingleton();
			auto *device = d3d->GetD3D12Device();

			RenderDocDiag::ReportLiveObjects(d3d, Thisptr);

			const auto vtable = *reinterpret_cast<void ***>(device);
			spdlog::info(
				"[RenderDoc] Capturing with device {:p}, vtable[0] owned by {}",
				static_cast<void *>(device),
				RenderDocDiag::OwnerOf(vtable[0]));

			s_captureDevice = device;
			s_captureWindow = GetGameWindow();

			renderDocApi->SetActiveWindow(s_captureDevice, s_captureWindow);
			renderDocApi->StartFrameCapture(s_captureDevice, s_captureWindow);
			s_capturing = true;
			spdlog::info("[RenderDoc] StartFrameCapture — will end next present");
		}



		return ret;
	}

	DECLARE_HOOK_TRANSACTION(NxDXGIImpl)
	{
		// Present vfunc is 10th index in NxDXGIImpl's virtual table
		const auto vtableEntryNxDXGIImpl = Offsets::Signature(
											   "48 8D 0D ? ? ? ? 66 89 68 08 48 89 08 40 88 68 0A 48 89 68 0C 48 89 68 18 48 89 68 20")
											   .AsRipRelative(7)
											   .AsAdjusted(sizeof(void *) * 10)
											   .ToPointer<uintptr_t>();

		Hooks::WriteJump(*vtableEntryNxDXGIImpl, &HookedPresent, &OriginalPresent);
	};
}
