#include "../../ModConfiguration.h"
#include "../../ModCoreEvents.h"
#include "../Core/IStreamingManager.h"
#include "../DebugUI/DebugUI.h"
#include "NxD3DImpl.h"
#include "NxDXGIImpl.h"

#include "../../test.h"
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

	static bool IsReadableMemory(void *ptr, size_t size)
	{
		if (!ptr)
			return false;
		MEMORY_BASIC_INFORMATION mbi {};
		if (!VirtualQuery(ptr, &mbi, sizeof(mbi)))
			return false;
		if (mbi.State != MEM_COMMIT)
			return false;
		constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY |
								   PAGE_WRITECOPY;
		return (mbi.Protect & readable) && !(mbi.Protect & PAGE_GUARD);
	}

	static bool TryGetVtableOwner(uintptr_t candidate, std::string& outName)
	{
		if (!IsReadableMemory(reinterpret_cast<void *>(candidate), sizeof(void *)))
			return false;

		void **vtablePtr = *reinterpret_cast<void ***>(candidate);

		if (!IsReadableMemory(vtablePtr, sizeof(void *)))
			return false;

		void *firstSlot = vtablePtr[0];
		if (!IsReadableMemory(firstSlot, sizeof(void *)))
			return false;

		HMODULE hOwner = nullptr;
		if (!GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(firstSlot),
				&hOwner))
			return false;

		wchar_t mod[MAX_PATH] {};
		GetModuleFileNameW(hOwner, mod, MAX_PATH);
		outName = std::filesystem::path(mod).filename().string();
		return true;
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

		if (uiInitialized)
		{
			DebugUI::RenderUI();
			DebugUI::RenderUID3D(NxD3DImpl::GetSingleton(), Thisptr);
		}

		auto ret = OriginalPresent(Thisptr, a2);

		static bool s_capturing = false;
		if (renderDocApi && s_capturing)
		{
			s_capturing = false;
			uint32_t result = renderDocApi->EndFrameCapture(nullptr, nullptr);
			spdlog::info("[RenderDoc] EndFrameCapture result: {} — total: {}", result, renderDocApi->GetNumCaptures());
		}

		// Start capture AFTER present — captures everything in the next frame
		if (renderDocApi && g_wantCapture.exchange(false))
		{
			/////////////
			auto *d3d = NxD3DImpl::GetSingleton();
			spdlog::info("[Test] NxD3DImpl ptr: {:p}", static_cast<void *>(d3d));

			auto base = reinterpret_cast<uintptr_t *>(d3d);

			constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;

			for (int i = 0; i < 256; i++)
			{
				// check the source address is readable before reading from it
				void *srcAddr = &base[i];
				MEMORY_BASIC_INFORMATION mbi {};
				if (!VirtualQuery(srcAddr, &mbi, sizeof(mbi)))
					continue;
				if (mbi.State != MEM_COMMIT || !(mbi.Protect & readable))
					continue;
				if (mbi.RegionSize < sizeof(uintptr_t))
					continue;

				uintptr_t candidate = base[i];
				if (!candidate)
					continue;

				// check candidate is readable as a vtable host
				if (!VirtualQuery(reinterpret_cast<void *>(candidate), &mbi, sizeof(mbi)))
					continue;
				if (mbi.State != MEM_COMMIT || !(mbi.Protect & readable))
					continue;
				if (mbi.RegionSize < sizeof(uintptr_t))
					continue;

				uintptr_t vtableAddr = *reinterpret_cast<uintptr_t *>(candidate);
				if (!vtableAddr)
					continue;

				// check vtable address is readable
				if (!VirtualQuery(reinterpret_cast<void *>(vtableAddr), &mbi, sizeof(mbi)))
					continue;
				if (mbi.State != MEM_COMMIT || !(mbi.Protect & readable))
					continue;
				if (mbi.RegionSize < sizeof(uintptr_t))
					continue;

				uintptr_t firstSlot = *reinterpret_cast<uintptr_t *>(vtableAddr);

				HMODULE hOwner = nullptr;
				if (!GetModuleHandleExW(
						GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCWSTR>(firstSlot),
						&hOwner))
					continue;

				wchar_t mod[MAX_PATH] {};
				GetModuleFileNameW(hOwner, mod, MAX_PATH);
				auto name = std::filesystem::path(mod).filename().string();

				spdlog::info("[Test] NxD3DImpl[{}] = {:p} → vtable owned by {}", i, reinterpret_cast<void *>(candidate), name);
			}
			//////////

			auto device = d3d->GetD3D12Device();
			renderDocApi->SetActiveWindow(device, GetGameWindow());

			renderDocApi->StartFrameCapture(nullptr, nullptr);
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
