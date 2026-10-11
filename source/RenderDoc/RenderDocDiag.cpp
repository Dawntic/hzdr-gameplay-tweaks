#include "RenderDocDiag.h"
#include "RenderDoc.h"

#define WIN32_NO_STATUS
#include <Windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <detours/detours.h>

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

	static void ForEachLoadedModule(auto&& Callback)
	{
		const auto peb = NtCurrentTeb()->ProcessEnvironmentBlock;
		const auto ldr = reinterpret_cast<LdrData *>(peb->Ldr);

		if (!ldr)
			return;

		const auto head = &ldr->InLoadOrderModuleList;
		uint32_t index = 0;

		for (auto cur = head->Flink; cur && cur != head; cur = cur->Flink)
		{
			const auto entry = CONTAINING_RECORD(cur, LdrEntry, InLoadOrderLinks);

			if (!entry->DllBase || !entry->BaseDllName.Buffer)
				continue;

			const std::wstring name(entry->BaseDllName.Buffer, entry->BaseDllName.Length / sizeof(wchar_t));
			Callback(index++, name, entry->DllBase, entry->SizeOfImage);
		}
	}

	static std::string Narrow(const std::wstring& Value)
	{
		return std::filesystem::path(Value).filename().string();
	}

	static bool IsWatchedSymbol(const char *Name)
	{
		for (const auto s : WatchedSymbols)
		{
			if (_stricmp(s, Name) == 0)
				return true;
		}

		return false;
	}

	static bool IsWatchedModule(const std::wstring& Name)
	{
		for (const auto m : WatchedModules)
		{
			if (_wcsicmp(m, Name.c_str()) == 0)
				return true;
		}

		return false;
	}

	//
	// Ground truth export lookup. Parsing the export directory by hand cannot be intercepted, so
	// comparing this against GetProcAddress() tells us whether GetProcAddress is being redirected.
	//
	static void *RawExportLookup(HMODULE Module, const char *Name)
	{
		const auto base = reinterpret_cast<uintptr_t>(Module);
		PIMAGE_NT_HEADERS nt = nullptr;

		if (!RenderDoc::ValidPeHeaders(base, nt))
			return nullptr;

		const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

		if (!dir.VirtualAddress)
			return nullptr;

		const auto exports = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(base + dir.VirtualAddress);
		const auto names = reinterpret_cast<uint32_t *>(base + exports->AddressOfNames);
		const auto ordinals = reinterpret_cast<uint16_t *>(base + exports->AddressOfNameOrdinals);
		const auto functions = reinterpret_cast<uint32_t *>(base + exports->AddressOfFunctions);

		for (uint32_t i = 0; i < exports->NumberOfNames; i++)
		{
			if (strcmp(reinterpret_cast<const char *>(base + names[i]), Name) != 0)
				continue;

			const auto rva = functions[ordinals[i]];

			// Forwarded export (points back inside the export directory)
			if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size)
				return nullptr;

			return reinterpret_cast<void *>(base + rva);
		}

		return nullptr;
	}

	// The GetProcAddress pointer the executable itself will call. If RenderDoc patched the exe's IAT this
	// is renderdoc.dll; if it wasn't patched, RenderDoc can never intercept a by-name resolution.
	static decltype(&::GetProcAddress) GetExeResolver()
	{
		decltype(&::GetProcAddress) result = nullptr;

		RenderDoc::ForEachImport(
			GetModuleHandleW(nullptr),
			[&](const char *Dll, const char *Func, void **Slot, bool)
		{
			if (!result && _stricmp(Func, "GetProcAddress") == 0 && _strnicmp(Dll, "KERNEL", 6) == 0)
				result = reinterpret_cast<decltype(&::GetProcAddress)>(*Slot);
		});

		return result ? result : &::GetProcAddress;
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

		if (s_Trace && HIWORD(ProcName) != 0 && IsWatchedSymbol(ProcName))
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

			if (FirstSighting(std::format("load|{}|{}|{}", Narrow(base), static_cast<void *>(Result), caller)))
				spdlog::info("[RDDiag] LoadLibrary(\"{}\") -> {:p}  caller={}", Narrow(base), static_cast<void *>(Result), caller);
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
	static void ReportModuleOrder()
	{
		spdlog::info("[RDDiag] --- module load order (relevant entries) ---");

		int32_t renderdocIndex = -1;
		int32_t d3d12Index = -1;
		int32_t dxgiIndex = -1;

		ForEachLoadedModule(
			[&](uint32_t Index, const std::wstring& Name, void *Base, uint32_t Size)
		{
			if (!IsWatchedModule(Name))
				return;

			if (_wcsicmp(Name.c_str(), L"renderdoc.dll") == 0)
				renderdocIndex = static_cast<int32_t>(Index);
			else if (_wcsicmp(Name.c_str(), L"d3d12.dll") == 0)
				d3d12Index = static_cast<int32_t>(Index);
			else if (_wcsicmp(Name.c_str(), L"dxgi.dll") == 0)
				dxgiIndex = static_cast<int32_t>(Index);

			spdlog::info("[RDDiag]   #{:<3} {:<24} base={:p} size=0x{:X}", Index, Narrow(Name), Base, Size);
		});

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
	}

	static void ReportResolverOwnership()
	{
		spdlog::info("[RDDiag] --- who owns the resolver functions ---");

		const auto realGetProcAddress = ::GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress");
		spdlog::info(
			"[RDDiag]   kernel32!GetProcAddress export = {:p} [{}]",
			reinterpret_cast<void *>(realGetProcAddress),
			RenderDoc::OwnerOf(reinterpret_cast<void *>(realGetProcAddress)));

		// Per-module IAT ownership of the resolver functions. Anything not owned by KERNELBASE/kernel32
		// has been redirected by somebody; we want to see renderdoc.dll here for the exe at minimum.
		ForEachLoadedModule(
			[&](uint32_t, const std::wstring& Name, void *Base, uint32_t)
		{
			const bool isExe = Base == static_cast<void *>(GetModuleHandleW(nullptr));

			if (!isExe && !IsWatchedModule(Name))
				return;

			RenderDoc::ForEachImport(
				static_cast<HMODULE>(Base),
				[&](const char *Dll, const char *Func, void **Slot, bool Delay)
			{
				const bool interesting = _stricmp(Func, "GetProcAddress") == 0 || _stricmp(Func, "LoadLibraryExW") == 0 ||
										 _stricmp(Func, "LoadLibraryW") == 0;

				if (!interesting)
					return;

				spdlog::info(
					"[RDDiag]   {:<24} imports {}!{:<16}{} slot={:p} -> {:p} [{}]",
					Narrow(Name),
					Dll,
					Func,
					Delay ? " (delay)" : "",
					static_cast<void *>(Slot),
					*Slot,
					RenderDoc::OwnerOf(*Slot));
			});
		});
	}

	static void ReportExportTruthTable()
	{
		spdlog::info("[RDDiag] --- export resolution truth table ---");
		spdlog::info("[RDDiag]   (raw = parsed from the export directory, gpa = ::GetProcAddress, exe = the exe's own resolver)");

		const auto exeResolver = GetExeResolver();
		spdlog::info("[RDDiag]   exe resolver = {:p} [{}]", reinterpret_cast<void *>(exeResolver), RenderDoc::OwnerOf(reinterpret_cast<void *>(exeResolver)));

		struct Target
		{
			const wchar_t *Module;
			const char *Symbol;
		};

		constexpr Target targets[] = {
			{ L"d3d12.dll", "D3D12CreateDevice" },
			{ L"d3d12.dll", "D3D12GetDebugInterface" },
			{ L"dxgi.dll", "CreateDXGIFactory" },
			{ L"dxgi.dll", "CreateDXGIFactory1" },
			{ L"dxgi.dll", "CreateDXGIFactory2" },
			{ L"sl.interposer.dll", "D3D12CreateDevice" },
			{ L"sl.interposer.dll", "CreateDXGIFactory2" },
			{ L"amd_ags_x64.dll", "agsDriverExtensionsDX12_CreateDevice" },
			{ L"amd_ags_x64.dll", "agsDriverExtensionsDX12_PushMarker" },
			{ L"amd_ags_x64.dll", "agsDriverExtensionsDX12_PopMarker" },
			{ L"amd_ags_x64.dll", "agsDriverExtensionsDX12_SetMarker" },
		};

		for (const auto& target : targets)
		{
			const auto module = GetModuleHandleW(target.Module);

			if (!module)
			{
				spdlog::info("[RDDiag]   {:<20} {:<38} module not loaded", Narrow(target.Module), target.Symbol);
				continue;
			}

			const auto raw = RawExportLookup(module, target.Symbol);
			const auto gpa = reinterpret_cast<void *>(::GetProcAddress(module, target.Symbol));
			const auto exe = reinterpret_cast<void *>(exeResolver(module, target.Symbol));

			spdlog::info(
				"[RDDiag]   {:<20} {:<38} raw={:p}[{}] gpa={:p}[{}] exe={:p}[{}] {}",
				Narrow(target.Module),
				target.Symbol,
				raw,
				RenderDoc::OwnerOf(raw),
				gpa,
				RenderDoc::OwnerOf(gpa),
				exe,
				RenderDoc::OwnerOf(exe),
				(gpa != raw || exe != raw) ? "<== REDIRECTED" : "");
		}
	}

	static void ReportImportSlots()
	{
		spdlog::info("[RDDiag] --- import slots referencing device/factory creation ---");

		bool any = false;

		ForEachLoadedModule(
			[&](uint32_t, const std::wstring& Name, void *Base, uint32_t)
		{
			RenderDoc::ForEachImport(
				static_cast<HMODULE>(Base),
				[&](const char *Dll, const char *Func, void **Slot, bool Delay)
			{
				if (!IsWatchedSymbol(Func))
					return;

				any = true;
				spdlog::info(
					"[RDDiag]   {:<24} <- {}!{}{} slot={:p} -> {:p} [{}]",
					Narrow(Name),
					Dll,
					Func,
					Delay ? " (delay)" : "",
					static_cast<void *>(Slot),
					*Slot,
					RenderDoc::OwnerOf(*Slot));
			});
		});

		if (!any)
			spdlog::info("[RDDiag]   none - every caller resolves these by name at runtime");
	}

	void Report(const char *Phase)
	{
		spdlog::info("[RDDiag] ================ {} ================", Phase);

		if (const auto commandLine = GetCommandLineW())
			spdlog::info("[RDDiag]   command line: {}", std::filesystem::path(commandLine).string());

		ReportModuleOrder();
		ReportResolverOwnership();
		ReportExportTruthTable();
		ReportImportSlots();

		spdlog::info("[RDDiag] ================ end {} ================", Phase);
	}

	void ProbeCreation()
	{
		spdlog::info("[RDDiag] --- control experiment: create objects the way the game does ---");

		const auto resolver = GetExeResolver();

		// dxgi first, exactly like the engine does (LoadLibrary "dxgi.dll" then CreateDXGIFactory2/1/base)
		if (const auto dxgi = LoadLibraryW(L"dxgi.dll"))
		{
			using PFN_CreateDXGIFactory2 = HRESULT(WINAPI *)(UINT, REFIID, void **);
			const auto create = reinterpret_cast<PFN_CreateDXGIFactory2>(resolver(dxgi, "CreateDXGIFactory2"));

			spdlog::info(
				"[RDDiag]   CreateDXGIFactory2 = {:p} [{}]",
				reinterpret_cast<void *>(create),
				RenderDoc::OwnerOf(reinterpret_cast<void *>(create)));

			if (create)
			{
				IDXGIFactory2 *factory = nullptr;

				if (const auto hr = create(0, IID_PPV_ARGS(&factory)); SUCCEEDED(hr) && factory)
				{
					const auto vtable = *reinterpret_cast<void ***>(factory);
					spdlog::info("[RDDiag]   IDXGIFactory2 vtable[0] = {:p} [{}]", vtable[0], RenderDoc::OwnerOf(vtable[0]));
					factory->Release();
				}
				else
				{
					spdlog::warn("[RDDiag]   CreateDXGIFactory2 failed, hr=0x{:08X}", static_cast<uint32_t>(hr));
				}
			}
		}
		else
		{
			spdlog::warn("[RDDiag]   LoadLibraryW(dxgi.dll) failed");
		}

		if (const auto d3d12 = LoadLibraryW(L"d3d12.dll"))
		{
			const auto create = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(resolver(d3d12, "D3D12CreateDevice"));

			spdlog::info(
				"[RDDiag]   D3D12CreateDevice = {:p} [{}]",
				reinterpret_cast<void *>(create),
				RenderDoc::OwnerOf(reinterpret_cast<void *>(create)));

			if (create)
			{
				ID3D12Device *device = nullptr;

				if (const auto hr = create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)); SUCCEEDED(hr) && device)
				{
					const auto vtable = *reinterpret_cast<void ***>(device);
					spdlog::info("[RDDiag]   ID3D12Device vtable[0] = {:p} [{}]", vtable[0], RenderDoc::OwnerOf(vtable[0]));
					spdlog::info(
						"[RDDiag]   => RenderDoc's D3D12 hooks are {}",
						RenderDoc::OwnerOf(vtable[0]) == "renderdoc.dll" ? "LIVE (so the game bypasses them)" : "NOT INSTALLED");
					device->Release();
				}
				else
				{
					spdlog::warn("[RDDiag]   D3D12CreateDevice failed, hr=0x{:08X}", static_cast<uint32_t>(hr));
				}
			}
		}
		else
		{
			spdlog::warn("[RDDiag]   LoadLibraryW(d3d12.dll) failed");
		}
	}

	static bool IsReadable(const void *Address, size_t Size)
	{
		if (!Address)
			return false;

		MEMORY_BASIC_INFORMATION mbi {};

		if (!VirtualQuery(Address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
			return false;

		constexpr DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY |
								   PAGE_WRITECOPY;

		if (!(mbi.Protect & readable) || (mbi.Protect & PAGE_GUARD))
			return false;

		const auto end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		return reinterpret_cast<uintptr_t>(Address) + Size <= end;
	}

	// Report the vtable owner of any COM-looking pointer stored in the first N fields of an object.
	static void ScanObjectFields(const char *Label, void *Object, size_t FieldCount)
	{
		if (!IsReadable(Object, sizeof(void *)))
		{
			spdlog::warn("[RDDiag]   {} is not readable ({:p})", Label, Object);
			return;
		}

		const auto fields = static_cast<void **>(Object);

		for (size_t i = 0; i < FieldCount; i++)
		{
			if (!IsReadable(&fields[i], sizeof(void *)))
				continue;

			const auto candidate = fields[i];

			if (!IsReadable(candidate, sizeof(void *)))
				continue;

			const auto vtable = *static_cast<void ***>(candidate);

			if (!IsReadable(vtable, sizeof(void *)))
				continue;

			const auto owner = RenderDoc::OwnerOf(vtable[0]);

			// Only interesting if it belongs to a graphics module or a hooking trampoline
			if (owner != "renderdoc.dll" && owner != "dxgi.dll" && owner != "d3d12.dll" && owner != "D3D12Core.dll" &&
				owner != "sl.interposer.dll" && owner != "nvapi64.dll")
				continue;

			spdlog::info("[RDDiag]   {}[{}] = {:p} vtable owned by {}", Label, i, candidate, owner);
		}
	}

	void ReportLiveObjects(void *NxD3DImplPtr, void *NxDXGIImplPtr)
	{
		spdlog::info("[RDDiag] --- live object ownership ---");
		spdlog::info("[RDDiag]   any object below owned by renderdoc.dll is wrapped; anything else is invisible to RenderDoc");

		ScanObjectFields("NxD3DImpl", NxD3DImplPtr, 256);
		ScanObjectFields("NxDXGIImpl", NxDXGIImplPtr, 256);
	}
}
