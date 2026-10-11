#include "RenderDocDiag.h"
#include "RenderDoc.h"

#define WIN32_NO_STATUS
#include <Windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>

#include <detours/detours.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <intrin.h>
#include <mutex>
#include <string>
#include <unordered_set>

namespace RenderDocDiag
{
	//
	// Things we care about. If the game resolves any of these and the answer is not owned by
	// renderdoc.dll, RenderDoc will never see the object that gets created.
	//
	constexpr const char *WatchedSymbols[] = {
		"D3D12CreateDevice",
		"D3D12GetDebugInterface",
		"D3D12GetInterface",
		"D3D12EnableExperimentalFeatures",
		"D3D12SerializeRootSignature",
		"CreateDXGIFactory",
		"CreateDXGIFactory1",
		"CreateDXGIFactory2",
		"DXGIGetDebugInterface1",
		"DXGIDeclareAdapterRemovalSupport",
		"agsDriverExtensionsDX12_CreateDevice",
		"agsDriverExtensionsDX12_PushMarker",
		"agsDriverExtensionsDX12_PopMarker",
		"agsDriverExtensionsDX12_SetMarker",
		"agsInitialize",
		"RENDERDOC_GetAPI",
		"nvapi_QueryInterface",
		"slInit",
		"slSetD3DDevice",
		"slUpgradeInterface",
		"slGetNativeInterface",
	};

	constexpr const wchar_t *WatchedModules[] = {
		L"renderdoc.dll",
		L"d3d12.dll",
		L"D3D12Core.dll",
		L"dxgi.dll",
		L"dxcore.dll",
		L"sl.interposer.dll",
		L"sl.common.dll",
		L"sl.dlss_g.dll",
		L"amd_ags_x64.dll",
		L"nvapi64.dll",
		L"winhttp.dll",
	};

	//
	// PEB walking. InLoadOrderModuleList is literally the answer to "did renderdoc.dll get in before
	// d3d12.dll/dxgi.dll", which is the first thing to rule in or out.
	//
	struct LdrData
	{
		ULONG Length;
		BOOLEAN Initialized;
		HANDLE SsHandle;
		LIST_ENTRY InLoadOrderModuleList;
		LIST_ENTRY InMemoryOrderModuleList;
		LIST_ENTRY InInitializationOrderModuleList;
	};

	struct LdrEntry
	{
		LIST_ENTRY InLoadOrderLinks;
		LIST_ENTRY InMemoryOrderLinks;
		LIST_ENTRY InInitializationOrderLinks;
		PVOID DllBase;
		PVOID EntryPoint;
		ULONG SizeOfImage;
		UNICODE_STRING FullDllName;
		UNICODE_STRING BaseDllName;
	};

	static bool IsWatchedModule(const std::wstring& Name)
	{
		return std::ranges::any_of(WatchedModules, [&](const wchar_t *Module) { return _wcsicmp(Module, Name.c_str()) == 0; });
	}

	//
	// Trace hooks
	//
	static bool s_TraceInstalled = false;
	static bool s_Trace = false;
	static decltype(&::GetProcAddress) OrigGetProcAddress = nullptr;
	static decltype(&::LoadLibraryExW) OrigLoadLibraryExW = nullptr;
	static decltype(&::LoadLibraryExA) OrigLoadLibraryExA = nullptr;

	// Some callers resolve the same export every frame (the Steam overlay polls CreateDXGIFactory*), which
	// buried the log in identical lines. Only the first occurrence of each lookup says anything new.
	static bool FirstSighting(std::string Key)
	{
		static std::mutex lock;
		static std::unordered_set<std::string> seen;

		std::scoped_lock guard(lock);
		return seen.insert(std::move(Key)).second;
	}

	static FARPROC WINAPI HookedGetProcAddress(HMODULE Module, LPCSTR ProcName)
	{
		const auto result = OrigGetProcAddress(Module, ProcName);

		if (s_Trace && HIWORD(ProcName) != 0 && std::ranges::any_of(WatchedSymbols, [&](const char *Symbol) { return _stricmp(Symbol, ProcName) == 0; }))
		{
			RenderDoc::LogGuard guard;

			if (guard.Entered)
			{
				const auto module = RenderDoc::OwnerOf(Module);
				const auto owner = RenderDoc::OwnerOf(reinterpret_cast<void *>(result));
				const auto caller = RenderDoc::OwnerOf(_ReturnAddress());

				if (FirstSighting(std::format("gpa|{}|{}|{}|{}", module, ProcName, owner, caller)))
				{
					spdlog::info(
						"[RDDiag] GetProcAddress({}, \"{}\") -> {:p} [{}]  caller={}",
						module,
						ProcName,
						reinterpret_cast<void *>(result),
						owner,
						caller);
				}
			}
		}

		return result;
	}

	static void TraceLoad(const wchar_t *Name, HMODULE Result, void *ReturnAddress)
	{
		if (!Name || !s_Trace)
			return;

		const std::wstring base = std::filesystem::path(Name).filename().wstring();

		if (!IsWatchedModule(base))
			return;

		RenderDoc::LogGuard guard;

		if (guard.Entered)
		{
			const auto caller = RenderDoc::OwnerOf(ReturnAddress);

			if (FirstSighting(std::format("load|{}|{}|{}", std::filesystem::path(base).string(), static_cast<void *>(Result), caller)))
				spdlog::info("[RDDiag] LoadLibrary(\"{}\") -> {:p}  caller={}", std::filesystem::path(base).string(), static_cast<void *>(Result), caller);
		}
	}

	static HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR FileName, HANDLE File, DWORD Flags)
	{
		const auto result = OrigLoadLibraryExW(FileName, File, Flags);
		TraceLoad(FileName, result, _ReturnAddress());

		return result;
	}

	static HMODULE WINAPI HookedLoadLibraryExA(LPCSTR FileName, HANDLE File, DWORD Flags)
	{
		const auto result = OrigLoadLibraryExA(FileName, File, Flags);

		if (FileName)
		{
			const auto wide = std::filesystem::path(FileName).wstring();
			TraceLoad(wide.c_str(), result, _ReturnAddress());
		}

		return result;
	}

	void InstallTraceHooks(bool Trace)
	{
		s_Trace = Trace;

		if (s_TraceInstalled)
			return;

		const auto kernel32 = GetModuleHandleW(L"kernel32.dll");

		if (!kernel32)
		{
			spdlog::error("[RDDiag] kernel32.dll not present, cannot install trace hooks");
			return;
		}

		// Deliberately resolved through the real exports rather than the IAT so that the detour lands on
		// the actual implementation everything funnels through.
		OrigGetProcAddress = reinterpret_cast<decltype(&::GetProcAddress)>(::GetProcAddress(kernel32, "GetProcAddress"));
		OrigLoadLibraryExW = reinterpret_cast<decltype(&::LoadLibraryExW)>(::GetProcAddress(kernel32, "LoadLibraryExW"));
		OrigLoadLibraryExA = reinterpret_cast<decltype(&::LoadLibraryExA)>(::GetProcAddress(kernel32, "LoadLibraryExA"));

		if (!OrigGetProcAddress || !OrigLoadLibraryExW || !OrigLoadLibraryExA)
		{
			spdlog::error("[RDDiag] Failed to resolve trace targets");
			return;
		}

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<void **>(&OrigGetProcAddress), &HookedGetProcAddress);
		DetourAttach(reinterpret_cast<void **>(&OrigLoadLibraryExW), &HookedLoadLibraryExW);
		DetourAttach(reinterpret_cast<void **>(&OrigLoadLibraryExA), &HookedLoadLibraryExA);

		if (DetourTransactionCommit() != NO_ERROR)
		{
			spdlog::error("[RDDiag] Failed to commit trace hook transaction");
			return;
		}

		s_TraceInstalled = true;
		spdlog::info("[RDDiag] Interceptors installed (trace={})", Trace);
	}

	//
	// Reporting
	//
	void Report(const char *Phase)
	{
		spdlog::info("[RDDiag] ================ {} ================", Phase);

		if (const auto commandLine = GetCommandLineW())
			spdlog::info("[RDDiag]   command line: {}", std::filesystem::path(commandLine).string());

		spdlog::info("[RDDiag] --- module load order (relevant entries) ---");

		int32_t renderdocIndex = -1;
		int32_t d3d12Index = -1;
		int32_t dxgiIndex = -1;

		// InLoadOrderModuleList is literally the answer to "did renderdoc.dll get in before d3d12/dxgi.dll",
		// which is the first thing to rule in or out.
		const auto ldr = reinterpret_cast<LdrData *>(NtCurrentTeb()->ProcessEnvironmentBlock->Ldr);

		if (ldr)
		{
			const auto head = &ldr->InLoadOrderModuleList;
			uint32_t index = 0;

			for (auto cur = head->Flink; cur && cur != head; cur = cur->Flink)
			{
				const auto entry = CONTAINING_RECORD(cur, LdrEntry, InLoadOrderLinks);

				if (!entry->DllBase || !entry->BaseDllName.Buffer)
					continue;

				const std::wstring name(entry->BaseDllName.Buffer, entry->BaseDllName.Length / sizeof(wchar_t));
				const auto moduleIndex = index++;

				if (!IsWatchedModule(name))
					continue;

				if (_wcsicmp(name.c_str(), L"renderdoc.dll") == 0)
					renderdocIndex = static_cast<int32_t>(moduleIndex);
				else if (_wcsicmp(name.c_str(), L"d3d12.dll") == 0)
					d3d12Index = static_cast<int32_t>(moduleIndex);
				else if (_wcsicmp(name.c_str(), L"dxgi.dll") == 0)
					dxgiIndex = static_cast<int32_t>(moduleIndex);

				spdlog::info(
					"[RDDiag]   #{:<3} {:<24} base={:p} size=0x{:X}",
					moduleIndex,
					std::filesystem::path(name).string(),
					entry->DllBase,
					entry->SizeOfImage);
			}
		}

		spdlog::info(
			"[RDDiag]   renderdoc={} d3d12={} dxgi={} => renderdoc loaded {}",
			renderdocIndex,
			d3d12Index,
			dxgiIndex,
			renderdocIndex < 0				 ? "NOT AT ALL"
			: (d3d12Index < 0 && dxgiIndex < 0) ? "before d3d12/dxgi (both absent so far)"
			: (renderdocIndex < d3d12Index || d3d12Index < 0) && (renderdocIndex < dxgiIndex || dxgiIndex < 0)
				? "BEFORE d3d12/dxgi (good)"
				: "AFTER d3d12/dxgi (hooks may have been missed)");

		spdlog::info("[RDDiag] ================ end {} ================", Phase);
	}
}
