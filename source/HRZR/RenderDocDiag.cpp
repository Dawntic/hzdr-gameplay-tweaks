#include "RenderDocDiag.h"

#define WIN32_NO_STATUS
#include <Windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <detours/detours.h>

#include <filesystem>
#include <intrin.h>
#include <string>

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

	// Which module does this address live in? "<none/trampoline>" means it isn't inside any loaded image,
	// which for a function pointer usually means a trampoline allocated by a hooking library.
	std::string OwnerOf(const void *Address)
	{
		if (!Address)
			return "<null>";

		HMODULE owner = nullptr;

		if (!GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(Address),
				&owner))
			return "<none/trampoline>";

		wchar_t path[MAX_PATH] {};
		GetModuleFileNameW(owner, path, MAX_PATH);

		return std::filesystem::path(path).filename().string();
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

	static bool ValidPeHeaders(uintptr_t Base, PIMAGE_NT_HEADERS& OutNt)
	{
		if (!Base)
			return false;

		const auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(Base);

		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return false;

		const auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(Base + dos->e_lfanew);

		if (nt->Signature != IMAGE_NT_SIGNATURE)
			return false;

		OutNt = nt;
		return true;
	}

	//
	// Ground truth export lookup. Parsing the export directory by hand cannot be intercepted, so
	// comparing this against GetProcAddress() tells us whether GetProcAddress is being redirected.
	//
	static void *RawExportLookup(HMODULE Module, const char *Name)
	{
		const auto base = reinterpret_cast<uintptr_t>(Module);
		PIMAGE_NT_HEADERS nt = nullptr;

		if (!ValidPeHeaders(base, nt))
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

	// Callback(ImportDllName, FunctionName, SlotAddress, IsDelayLoad)
	static void ForEachImport(HMODULE Module, auto&& Callback)
	{
		const auto base = reinterpret_cast<uintptr_t>(Module);
		PIMAGE_NT_HEADERS nt = nullptr;

		if (!ValidPeHeaders(base, nt))
			return;

		if (const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]; dir.VirtualAddress)
		{
			for (auto desc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + dir.VirtualAddress); desc->Name; desc++)
			{
				const auto dllName = reinterpret_cast<const char *>(base + desc->Name);
				const auto intRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;

				auto nameThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + intRva);
				auto addrThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->FirstThunk);

				for (; nameThunk->u1.AddressOfData; nameThunk++, addrThunk++)
				{
					if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal))
						continue;

					const auto importByName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + nameThunk->u1.AddressOfData);
					Callback(dllName, importByName->Name, reinterpret_cast<void **>(&addrThunk->u1.Function), false);
				}
			}
		}

		if (const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT]; dir.VirtualAddress)
		{
			for (auto desc = reinterpret_cast<PIMAGE_DELAYLOAD_DESCRIPTOR>(base + dir.VirtualAddress); desc->DllNameRVA; desc++)
			{
				// Non-RVA based descriptors are ancient and would need different arithmetic
				if (!desc->Attributes.RvaBased)
					continue;

				const auto dllName = reinterpret_cast<const char *>(base + desc->DllNameRVA);

				auto nameThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->ImportNameTableRVA);
				auto addrThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + desc->ImportAddressTableRVA);

				for (; nameThunk->u1.AddressOfData; nameThunk++, addrThunk++)
				{
					if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal))
						continue;

					const auto importByName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + nameThunk->u1.AddressOfData);
					Callback(dllName, importByName->Name, reinterpret_cast<void **>(&addrThunk->u1.Function), true);
				}
			}
		}
	}

	// The GetProcAddress pointer the executable itself will call. If RenderDoc patched the exe's IAT this
	// is renderdoc.dll; if it wasn't patched, RenderDoc can never intercept a by-name resolution.
	static decltype(&::GetProcAddress) GetExeResolver()
	{
		decltype(&::GetProcAddress) result = nullptr;

		ForEachImport(
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
	static bool s_BypassStreamline = false;
	static decltype(&::GetProcAddress) OrigGetProcAddress = nullptr;
	static decltype(&::LoadLibraryExW) OrigLoadLibraryExW = nullptr;
	static decltype(&::LoadLibraryExA) OrigLoadLibraryExA = nullptr;

	// spdlog's sink takes a lock and touches the CRT; never re-enter it from a nested resolve
	static thread_local bool t_InTrace = false;

	struct TraceGuard
	{
		bool Entered = false;

		TraceGuard()
		{
			if (!t_InTrace)
			{
				t_InTrace = true;
				Entered = true;
			}
		}

		~TraceGuard()
		{
			if (Entered)
				t_InTrace = false;
		}
	};

	//
	// Streamline bypass.
	//
	// The game asks sl.interposer.dll for D3D12CreateDevice / CreateDXGIFactory* rather than asking
	// d3d12.dll / dxgi.dll. RenderDoc only hooks the latter, and Streamline reaches the real runtime
	// without going back through a hookable resolution, so RenderDoc never sees the device or the
	// swapchain get created and ends up with zero frame capturers.
	//
	// Answering those lookups with the real module's export puts creation back on the path RenderDoc
	// hooked. Streamline stays loaded and slInit still succeeds; it just no longer sits in front of
	// device and swapchain creation, which is what costs frame generation.
	//
	static const wchar_t *StreamlineProxyHost(const char *Name)
	{
		constexpr const char *d3d12Symbols[] = {
			"D3D12CreateDevice",
			"D3D12GetDebugInterface",
			"D3D12GetInterface",
			"D3D12EnableExperimentalFeatures",
		};

		constexpr const char *dxgiSymbols[] = {
			"CreateDXGIFactory",
			"CreateDXGIFactory1",
			"CreateDXGIFactory2",
			"DXGIGetDebugInterface1",
		};

		for (const auto s : d3d12Symbols)
		{
			if (_stricmp(s, Name) == 0)
				return L"d3d12.dll";
		}

		for (const auto s : dxgiSymbols)
		{
			if (_stricmp(s, Name) == 0)
				return L"dxgi.dll";
		}

		return nullptr;
	}

	static bool IsStreamlineInterposer(HMODULE Module)
	{
		static HMODULE cached = nullptr;

		if (!cached)
			cached = GetModuleHandleW(L"sl.interposer.dll");

		return cached && Module == cached;
	}

	static FARPROC RedirectStreamlineProxy(HMODULE Module, LPCSTR ProcName)
	{
		if (!IsStreamlineInterposer(Module))
			return nullptr;

		const auto hostName = StreamlineProxyHost(ProcName);

		if (!hostName)
			return nullptr;

		// The host is normally already loaded by the time the engine gets here. LoadLibrary is a fallback;
		// it's safe because this path is only reached from engine init, never from a DllMain.
		auto host = GetModuleHandleW(hostName);

		if (!host)
			host = LoadLibraryW(hostName);

		if (!host)
			return nullptr;

		// Resolve through the executable's own resolver, which RenderDoc has patched. That returns
		// RenderDoc's wrapper rather than the raw export.
		static const auto resolver = GetExeResolver();
		const auto substitute = resolver(host, ProcName);

		if (!substitute)
			return nullptr;

		TraceGuard guard;

		if (guard.Entered)
		{
			spdlog::info(
				"[RDDiag] Streamline bypass: sl.interposer!{} -> {}!{} = {:p} [{}]",
				ProcName,
				Narrow(hostName),
				ProcName,
				reinterpret_cast<void *>(substitute),
				OwnerOf(reinterpret_cast<void *>(substitute)));
		}

		return substitute;
	}

	static FARPROC WINAPI HookedGetProcAddress(HMODULE Module, LPCSTR ProcName)
	{
		// HIWORD == 0 means resolution by ordinal, where ProcName is not a pointer
		if (s_BypassStreamline && HIWORD(ProcName) != 0)
		{
			if (const auto substitute = RedirectStreamlineProxy(Module, ProcName))
				return substitute;
		}

		const auto result = OrigGetProcAddress(Module, ProcName);

		if (s_Trace && HIWORD(ProcName) != 0 && IsWatchedSymbol(ProcName))
		{
			TraceGuard guard;

			if (guard.Entered)
			{
				spdlog::info(
					"[RDDiag] GetProcAddress({}, \"{}\") -> {:p} [{}]  caller={}",
					OwnerOf(Module),
					ProcName,
					reinterpret_cast<void *>(result),
					OwnerOf(reinterpret_cast<void *>(result)),
					OwnerOf(_ReturnAddress()));
			}
		}

		return result;
	}

	static void TraceLoad(const wchar_t *Name, HMODULE Result)
	{
		if (!Name || !s_Trace)
			return;

		const std::wstring base = std::filesystem::path(Name).filename().wstring();

		if (!IsWatchedModule(base))
			return;

		TraceGuard guard;

		if (guard.Entered)
			spdlog::info("[RDDiag] LoadLibrary(\"{}\") -> {:p}  caller={}", Narrow(base), static_cast<void *>(Result), OwnerOf(_ReturnAddress()));
	}

	static HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR FileName, HANDLE File, DWORD Flags)
	{
		const auto result = OrigLoadLibraryExW(FileName, File, Flags);
		TraceLoad(FileName, result);

		return result;
	}

	static HMODULE WINAPI HookedLoadLibraryExA(LPCSTR FileName, HANDLE File, DWORD Flags)
	{
		const auto result = OrigLoadLibraryExA(FileName, File, Flags);

		if (FileName)
		{
			const auto wide = std::filesystem::path(FileName).wstring();
			TraceLoad(wide.c_str(), result);
		}

		return result;
	}

	void InstallTraceHooks(bool Trace, bool BypassStreamline)
	{
		s_Trace = Trace;
		s_BypassStreamline = BypassStreamline;

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
		spdlog::info("[RDDiag] Interceptors installed (trace={}, streamline bypass={})", Trace, BypassStreamline);
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
			OwnerOf(reinterpret_cast<void *>(realGetProcAddress)));

		// Per-module IAT ownership of the resolver functions. Anything not owned by KERNELBASE/kernel32
		// has been redirected by somebody; we want to see renderdoc.dll here for the exe at minimum.
		ForEachLoadedModule(
			[&](uint32_t, const std::wstring& Name, void *Base, uint32_t)
		{
			const bool isExe = Base == static_cast<void *>(GetModuleHandleW(nullptr));

			if (!isExe && !IsWatchedModule(Name))
				return;

			ForEachImport(
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
					OwnerOf(*Slot));
			});
		});
	}

	static void ReportExportTruthTable()
	{
		spdlog::info("[RDDiag] --- export resolution truth table ---");
		spdlog::info("[RDDiag]   (raw = parsed from the export directory, gpa = ::GetProcAddress, exe = the exe's own resolver)");

		const auto exeResolver = GetExeResolver();
		spdlog::info("[RDDiag]   exe resolver = {:p} [{}]", reinterpret_cast<void *>(exeResolver), OwnerOf(reinterpret_cast<void *>(exeResolver)));

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
				OwnerOf(raw),
				gpa,
				OwnerOf(gpa),
				exe,
				OwnerOf(exe),
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
			ForEachImport(
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
					OwnerOf(*Slot));
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
				OwnerOf(reinterpret_cast<void *>(create)));

			if (create)
			{
				IDXGIFactory2 *factory = nullptr;

				if (const auto hr = create(0, IID_PPV_ARGS(&factory)); SUCCEEDED(hr) && factory)
				{
					const auto vtable = *reinterpret_cast<void ***>(factory);
					spdlog::info("[RDDiag]   IDXGIFactory2 vtable[0] = {:p} [{}]", vtable[0], OwnerOf(vtable[0]));
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
				OwnerOf(reinterpret_cast<void *>(create)));

			if (create)
			{
				ID3D12Device *device = nullptr;

				if (const auto hr = create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)); SUCCEEDED(hr) && device)
				{
					const auto vtable = *reinterpret_cast<void ***>(device);
					spdlog::info("[RDDiag]   ID3D12Device vtable[0] = {:p} [{}]", vtable[0], OwnerOf(vtable[0]));
					spdlog::info(
						"[RDDiag]   => RenderDoc's D3D12 hooks are {}",
						OwnerOf(vtable[0]) == "renderdoc.dll" ? "LIVE (so the game bypasses them)" : "NOT INSTALLED");
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

			const auto owner = OwnerOf(vtable[0]);

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

	//
	// AGS marker bridge
	//
	// AGS is the only marker API this executable references, so if the engine emits anything it comes
	// through here. We sit under RenderDoc's IAT patch, so we observe the calls the game actually makes.
	// Forwarding re-emits them as PIX ANSI events (metadata 1), which RenderDoc decodes into the event
	// browser tree.
	//
	// Params left opaque on purpose: the AGS struct layouts move between SDK versions and we only need to
	// know whether the engine calls these and what they answer, not what is inside them.
	using AgsInitialize_t = int(__cdecl *)(int, const void *, void **, void *);
	using AgsDX12CreateDevice_t = int(__cdecl *)(void *, const void *, const void *, void *);

	using AgsPushMarker_t = int(__cdecl *)(void *, ID3D12GraphicsCommandList *, const char *);
	using AgsPopMarker_t = int(__cdecl *)(void *, ID3D12GraphicsCommandList *);
	using AgsSetMarker_t = int(__cdecl *)(void *, ID3D12GraphicsCommandList *, const char *);

	static AgsInitialize_t OrigAgsInitialize = nullptr;
	static AgsDX12CreateDevice_t OrigAgsDX12CreateDevice = nullptr;
	static AgsPushMarker_t OrigAgsPushMarker = nullptr;
	static AgsPopMarker_t OrigAgsPopMarker = nullptr;
	static AgsSetMarker_t OrigAgsSetMarker = nullptr;

	static bool s_AgsTrace = false;
	static bool s_AgsForward = false;
	static std::atomic_uint32_t s_AgsCallCount = 0;

	// Enough to prove whether the engine emits markers at all without drowning the log every frame
	static constexpr uint32_t AgsTraceLimit = 64;

	static void TraceAgsCall(const char *Which, const char *Data)
	{
		const auto count = s_AgsCallCount.fetch_add(1);

		if (!s_AgsTrace || count >= AgsTraceLimit)
			return;

		TraceGuard guard;

		if (guard.Entered)
			spdlog::info("[RDDiag] AGS marker #{}: {}(\"{}\")", count, Which, Data ? Data : "");
	}

	static int __cdecl HookedAgsInitialize(int Version, const void *Config, void **Context, void *GpuInfo)
	{
		const auto result = OrigAgsInitialize(Version, Config, Context, GpuInfo);

		TraceGuard guard;

		if (guard.Entered)
			spdlog::info("[RDDiag] agsInitialize(version={}) -> {} (0 == AGS_SUCCESS)", Version, result);

		return result;
	}

	static int __cdecl HookedAgsDX12CreateDevice(void *Context, const void *CreationParams, const void *ExtensionParams, void *Returned)
	{
		const auto result = OrigAgsDX12CreateDevice(Context, CreationParams, ExtensionParams, Returned);

		TraceGuard guard;

		if (guard.Entered)
			spdlog::info("[RDDiag] agsDriverExtensionsDX12_CreateDevice() -> {} (0 == AGS_SUCCESS)", result);

		return result;
	}

	static int __cdecl HookedAgsPushMarker(void *Context, ID3D12GraphicsCommandList *CommandList, const char *Data)
	{
		TraceAgsCall("PushMarker", Data);

		if (s_AgsForward && CommandList && Data)
			CommandList->BeginEvent(1, Data, static_cast<UINT>(strlen(Data) + 1));

		return OrigAgsPushMarker(Context, CommandList, Data);
	}

	static int __cdecl HookedAgsPopMarker(void *Context, ID3D12GraphicsCommandList *CommandList)
	{
		TraceAgsCall("PopMarker", nullptr);

		if (s_AgsForward && CommandList)
			CommandList->EndEvent();

		return OrigAgsPopMarker(Context, CommandList);
	}

	static int __cdecl HookedAgsSetMarker(void *Context, ID3D12GraphicsCommandList *CommandList, const char *Data)
	{
		TraceAgsCall("SetMarker", Data);

		if (s_AgsForward && CommandList && Data)
			CommandList->SetMarker(1, Data, static_cast<UINT>(strlen(Data) + 1));

		return OrigAgsSetMarker(Context, CommandList, Data);
	}

	void InstallAgsMarkerBridge(bool Trace, bool Forward)
	{
		s_AgsTrace = Trace;
		s_AgsForward = Forward;

		static bool installed = false;

		if (installed)
			return;

		const auto ags = GetModuleHandleW(L"amd_ags_x64.dll");

		if (!ags)
		{
			spdlog::warn("[RDDiag] amd_ags_x64.dll not loaded, cannot bridge AGS markers");
			return;
		}

		// Resolved raw so we hook the real implementation rather than whatever sits in front of it
		OrigAgsInitialize = reinterpret_cast<AgsInitialize_t>(RawExportLookup(ags, "agsInitialize"));
		OrigAgsDX12CreateDevice = reinterpret_cast<AgsDX12CreateDevice_t>(RawExportLookup(ags, "agsDriverExtensionsDX12_CreateDevice"));
		OrigAgsPushMarker = reinterpret_cast<AgsPushMarker_t>(RawExportLookup(ags, "agsDriverExtensionsDX12_PushMarker"));
		OrigAgsPopMarker = reinterpret_cast<AgsPopMarker_t>(RawExportLookup(ags, "agsDriverExtensionsDX12_PopMarker"));
		OrigAgsSetMarker = reinterpret_cast<AgsSetMarker_t>(RawExportLookup(ags, "agsDriverExtensionsDX12_SetMarker"));

		if (!OrigAgsInitialize || !OrigAgsDX12CreateDevice || !OrigAgsPushMarker || !OrigAgsPopMarker || !OrigAgsSetMarker)
		{
			spdlog::warn("[RDDiag] AGS marker exports missing");
			return;
		}

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<void **>(&OrigAgsInitialize), &HookedAgsInitialize);
		DetourAttach(reinterpret_cast<void **>(&OrigAgsDX12CreateDevice), &HookedAgsDX12CreateDevice);
		DetourAttach(reinterpret_cast<void **>(&OrigAgsPushMarker), &HookedAgsPushMarker);
		DetourAttach(reinterpret_cast<void **>(&OrigAgsPopMarker), &HookedAgsPopMarker);
		DetourAttach(reinterpret_cast<void **>(&OrigAgsSetMarker), &HookedAgsSetMarker);

		if (DetourTransactionCommit() != NO_ERROR)
		{
			spdlog::error("[RDDiag] Failed to commit AGS marker transaction");
			return;
		}

		installed = true;
		spdlog::info("[RDDiag] AGS marker bridge installed (trace={}, forward={})", Trace, Forward);
	}
}
