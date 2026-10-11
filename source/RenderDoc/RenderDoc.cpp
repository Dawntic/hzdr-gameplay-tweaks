#define WIN32_NO_STATUS
#include <Windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>

#include "RenderDoc.h"
#include "RenderDocDiag.h"
#include "ShaderScopes.h"
#include "../ModConfiguration.h"
#if !defined(MOD_GAME_CRIMSON_DESERT)
#include "../Decima/RenderPassNames.h"
#endif

#include <algorithm>
#include <intrin.h>
#include <mutex>
#include <filesystem>
#include "tlhelp32.h"
#include <detours/detours.h>

namespace RenderDoc
{
RENDERDOC_API_1_7_0 *Api = nullptr;


// The engine has its own RenderDoc integration behind -attach_renderdoc (see the renderdoc.dll /
// RENDERDOC_GetAPI / source:Tools/RenderDoc/renderdoc.dll strings in the executable). If that's in play we
// must stay out of the way entirely: renderdoc.dll installs its API hooks from DllMain, so a second
// LoadLibrary just bumps the refcount and returns the existing handle without re-hooking anything. Loading
// it ourselves first would therefore *prevent* the engine's attach from ever hooking properly.
bool EngineWillAttach()
{
	const auto rawCommandLine = GetCommandLineW();

	if (!rawCommandLine)
		return false;

	std::wstring commandLine(rawCommandLine);
	std::transform(commandLine.begin(), commandLine.end(), commandLine.begin(), ::towlower);

	return commandLine.find(L"-attach_renderdoc") != std::wstring::npos;
}

static bool AcquireRenderDocApi(HMODULE Module)
{
	auto RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)GetProcAddress(Module, "RENDERDOC_GetAPI");

	if (!RENDERDOC_GetAPI)
	{
		spdlog::warn("[RenderDoc] Failed to get RENDERDOC_GetAPI");
		return false;
	}

	if (RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_7_0, (void **)&Api) != 1 || !Api)
	{
		spdlog::warn("[RenderDoc] Failed to get API interface");
		Api = nullptr;
		return false;
	}

	// Only override the capture template when explicitly configured, so we never clobber whatever the
	// engine set up for itself.
	if (!ModConfiguration.RenderDoc.CapturePath.empty())
	{
		const std::filesystem::path capturePath = ModConfiguration.RenderDoc.CapturePath;

		std::error_code ec;
		std::filesystem::create_directories(capturePath.parent_path(), ec);

		Api->SetCaptureFilePathTemplate(capturePath.string().c_str());
	}

	spdlog::info("[RenderDoc] API acquired. Capture template: {}", Api->GetCaptureFilePathTemplate());
	return true;
}

// Attach to a renderdoc.dll somebody else already loaded (i.e. the engine's -attach_renderdoc path).
// Never calls LoadLibrary, so it cannot influence hook installation.
bool AttachToLoaded()
{
	if (Api)
		return true;

	const auto renderDocModule = GetModuleHandleW(L"renderdoc.dll");

	if (!renderDocModule)
		return false;

	if (!AcquireRenderDocApi(renderDocModule))
		return false;

	spdlog::info("[RenderDoc] Attached to an already-loaded renderdoc.dll");
	return true;
}

void Load()
{
	if (Api)
	{
		spdlog::debug("[RenderDoc] Already initialized, skipping");
		return;
	}

	const std::filesystem::path renderdocPath = ModConfiguration.RenderDoc.DllPath;

	if (!std::filesystem::exists(renderdocPath))
	{
		spdlog::warn("[RenderDoc] renderdoc.dll not found at: {}", renderdocPath.string());
		return;
	}

	spdlog::debug("[RenderDoc] Attempting to load renderdoc.dll from {}", renderdocPath.string());
	HMODULE renderDocModule = LoadLibraryW(renderdocPath.wstring().c_str());

	if (!renderDocModule)
	{
		spdlog::warn("[RenderDoc] Failed to load renderdoc.dll");
		return;
	}

	spdlog::info("[RenderDoc] Loaded renderdoc.dll from {}", renderdocPath.string());

	if (!AcquireRenderDocApi(renderDocModule))
		FreeLibrary(renderDocModule);
}

}

// ntdll LdrLoadDll signature
using LdrLoadDll_t = NTSTATUS(NTAPI *)(PWSTR SearchPath, PULONG LoadFlags, PUNICODE_STRING Name, PHANDLE ModuleHandle);

static LdrLoadDll_t OrigLdrLoadDll = nullptr;

// Resolved once during Hooks::Initialize rather than inside the hook, so the hook body stays cheap.
static bool g_EngineAttachRequested = false;

// std::filesystem::path::string() runs a locale conversion that throws on unconvertible input. That is
// unusable here: this code runs inside the loader lock and returns into ntdll, where an escaping exception
// terminates the process silently. Convert by hand instead.
static std::wstring WidenLossy(const std::string& Value)
{
	if (Value.empty())
		return {};

	const int required = MultiByteToWideChar(CP_UTF8, 0, Value.data(), static_cast<int>(Value.size()), nullptr, 0);

	if (required <= 0)
		return {};

	std::wstring result(static_cast<size_t>(required), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, Value.data(), static_cast<int>(Value.size()), result.data(), required);

	return result;
}

static std::string NarrowLossy(const wchar_t *Value, size_t Length)
{
	if (!Value || Length == 0)
		return {};

	const int required = WideCharToMultiByte(CP_UTF8, 0, Value, static_cast<int>(Length), nullptr, 0, nullptr, nullptr);

	if (required <= 0)
		return {};

	std::string result(static_cast<size_t>(required), '\0');
	WideCharToMultiByte(CP_UTF8, 0, Value, static_cast<int>(Length), result.data(), required, nullptr, nullptr);

	return result;
}

// Bounded, so a bogus pointer cannot run off into unmapped memory before the guard below catches it.
static std::string NarrowLossyBounded(const wchar_t *Value, size_t MaxChars)
{
	if (!Value)
		return {};

	size_t length = 0;

	while (length < MaxChars && Value[length] != L'\0')
		length++;

	return NarrowLossy(Value, length);
}

// Everything the hook wants to do beyond calling the original, split out so the hook itself can wrap it in
// a __try. Returns the (possibly redirected) status.
static NTSTATUS LdrLoadDllTail(PWSTR SearchPath, PULONG LoadFlags, PUNICODE_STRING Name, PHANDLE ModuleHandle, NTSTATUS Status)
{
	if (!Name || !Name->Buffer || Name->Length == 0)
		return Status;

	try
	{
		const std::wstring fullName(Name->Buffer, Name->Length / sizeof(wchar_t));
		const auto pos = fullName.rfind(L'\\');
		const std::wstring filename = (pos != std::wstring::npos) ? fullName.substr(pos + 1) : fullName;

		const bool isRenderDoc = _wcsicmp(filename.c_str(), L"renderdoc.dll") == 0;

		// Earliest safe point to turn on the engine's debug naming. The executable's CRT initializers zero
		// the gate flag, and they have run by the time anything loads d3d12.dll. Waiting until the first
		// Present is too late for anything named during renderer init - pooled render targets and buffers
		// are constructed once, well before the first frame, and are never renamed.
#if !defined(MOD_GAME_CRIMSON_DESERT)
		if (NT_SUCCESS(Status) && ModConfiguration.RenderDoc.NameCommandLists &&
			_wcsicmp(filename.c_str(), L"d3d12.dll") == 0)
			HRZR::RenderPassNames::SetEnabled(true);
#endif

		// The engine's -attach_renderdoc handler asks the loader for renderdoc.dll and gives up quietly if
		// it isn't where it expects. Rather than guessing that path, satisfy the request with the
		// configured absolute path, keeping the load on the engine's own call.
		static bool s_redirectedEngineLoad = false;

		if (Status == STATUS_DLL_NOT_FOUND && isRenderDoc && !s_redirectedEngineLoad && g_EngineAttachRequested &&
			ModConfiguration.RenderDoc.RedirectEngineLoad)
		{
			s_redirectedEngineLoad = true;

			std::wstring absolutePath = WidenLossy(ModConfiguration.RenderDoc.DllPath);

			UNICODE_STRING redirected {};
			redirected.Buffer = absolutePath.data();
			redirected.Length = static_cast<USHORT>(absolutePath.size() * sizeof(wchar_t));
			redirected.MaximumLength = static_cast<USHORT>(redirected.Length + sizeof(wchar_t));

			Status = OrigLdrLoadDll(nullptr, LoadFlags, &redirected, ModuleHandle);

			spdlog::info(
				"[RenderDoc] Engine asked for \"{}\" and missed. Redirected to \"{}\": status=0x{:08X} handle={:p}",
				NarrowLossy(fullName.data(), fullName.size()),
				ModConfiguration.RenderDoc.DllPath,
				static_cast<uint32_t>(Status),
				ModuleHandle ? *ModuleHandle : nullptr);
		}

		// Loader-level view of module arrival, which catches static/delay-load paths that never touch
		// kernel32!LoadLibrary and therefore never show up in the RenderDocDiag trace hooks. Kept to a
		// short fixed list: this fires for every DLL the process ever loads.
		if (ModConfiguration.RenderDoc.Diagnostics)
		{
			bool interesting = isRenderDoc;

			for (const auto watched : { L"d3d12.dll", L"D3D12Core.dll", L"dxgi.dll", L"sl.interposer.dll", L"nvapi64.dll" })
			{
				if (_wcsicmp(filename.c_str(), watched) == 0)
				{
					interesting = true;
					break;
				}
			}

			// Log the name as requested, not the basename: whether the caller asked for a bare name or a full
			// path decides where the file actually has to live.
			if (interesting)
			{
				spdlog::info(
					"[RDDiag] LdrLoadDll(\"{}\") searchPath=\"{}\" flags=0x{:08X} status=0x{:08X} handle={:p}",
					NarrowLossy(fullName.data(), fullName.size()),
					NarrowLossyBounded(SearchPath, 2048),
					LoadFlags ? *LoadFlags : 0,
					static_cast<uint32_t>(Status),
					ModuleHandle ? *ModuleHandle : nullptr);
			}
		}
	}
	catch (...)
	{
		// A failed diagnostic must never take the loader down.
	}

	return Status;
}

static NTSTATUS NTAPI HookedLdrLoadDll(PWSTR SearchPath, PULONG LoadFlags, PUNICODE_STRING Name, PHANDLE ModuleHandle)
{
	NTSTATUS status = OrigLdrLoadDll(SearchPath, LoadFlags, Name, ModuleHandle);

	// This returns into ntdll's loader. An access violation here is an SEH exception, which /EHsc does NOT
	// route to catch(...) - it would unwind straight through the loader and kill the process with no
	// diagnostics whatsoever. SearchPath in particular is not always a valid string pointer: ntdll passes
	// sentinel values on some internal paths. Hence the hard guard.
	__try
	{
		status = LdrLoadDllTail(SearchPath, LoadFlags, Name, ModuleHandle, status);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}

	return status;
}


DECLARE_HOOK_TRANSACTION(LdrLoadDll)
{
	// Cached here so the hook body never has to parse the command line under the loader lock
	g_EngineAttachRequested = RenderDoc::EngineWillAttach();

	// Hook LdrLoadDll to catch anything that loads afterward
	HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");

	auto addr = reinterpret_cast<uintptr_t>(GetProcAddress(hNtdll, "LdrLoadDll"));

	return Hooks::WriteJump(addr, &HookedLdrLoadDll, reinterpret_cast<void **>(&OrigLdrLoadDll));
};

namespace RenderDoc
{
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

	bool ValidPeHeaders(uintptr_t Base, PIMAGE_NT_HEADERS& OutNt)
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

	// Callback(ImportDllName, FunctionName, SlotAddress, IsDelayLoad)
	void ForEachImport(HMODULE Module, const ImportVisitor& Callback)
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

	// A GetProcAddress that RenderDoc intercepts, or null if none can be found. RenderDoc hooks by patching
	// import tables, so any module's patched GetProcAddress slot will do. The exe's own slot isn't enough:
	// in Crimson Desert it still holds the raw kernel32 export after RenderDoc loads (Denuvo is the likely
	// culprit), so this mod's own import table, which RenderDoc patches too, is the fallback.
	static decltype(&::GetProcAddress) GetRenderDocResolver()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&GetRenderDocResolver),
			&self);

		for (const auto module : { GetModuleHandleW(nullptr), self })
		{
			decltype(&::GetProcAddress) result = nullptr;

			ForEachImport(
				module,
				[&](const char *Dll, const char *Func, void **Slot, bool)
			{
				if (!result && _stricmp(Func, "GetProcAddress") == 0 && OwnerOf(*Slot) == "renderdoc.dll")
					result = reinterpret_cast<decltype(&::GetProcAddress)>(*Slot);
			});

			if (result)
				return result;
		}

		return nullptr;
	}

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

	// Points the executable's static d3d12.dll / dxgi.dll imports at RenderDoc's hooks where they aren't
	// already. If RenderDoc's import patches on the exe are missing, the game's own CreateDXGIFactory1/2 and
	// D3D12GetInterface calls hand back unwrapped objects that then meet wrapped ones. Idempotent.
	static void RepairExeImports(decltype(&::GetProcAddress) Resolver)
	{
		ForEachImport(
			GetModuleHandleW(nullptr),
			[&](const char *Dll, const char *Func, void **Slot, bool)
		{
			if (_stricmp(Dll, "d3d12.dll") != 0 && _stricmp(Dll, "dxgi.dll") != 0)
				return;

			const auto host = GetModuleHandleA(Dll);

			if (!host || OwnerOf(*Slot) == "renderdoc.dll")
				return;

			const auto hooked = reinterpret_cast<void *>(Resolver(host, Func));

			// RenderDoc doesn't hook everything (root signature serialization, say), and raw is correct there
			if (!hooked || OwnerOf(hooked) != "renderdoc.dll")
				return;

			DWORD oldProtect = 0;

			if (!VirtualProtect(Slot, sizeof(void *), PAGE_READWRITE, &oldProtect))
			{
				spdlog::error("[RenderDoc] Cannot unprotect the exe import slot for {}!{}", Dll, Func);
				return;
			}

			const auto previous = *Slot;
			*Slot = hooked;
			VirtualProtect(Slot, sizeof(void *), oldProtect, &oldProtect);

			spdlog::info("[RenderDoc] Exe import {}!{} was {:p} [{}], now RenderDoc's {:p}", Dll, Func, previous, OwnerOf(previous), hooked);
		});
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

		// Resolve through a GetProcAddress RenderDoc intercepts, so the game gets RenderDoc's creation hooks
		// rather than the raw exports. Handing out a raw D3D12CreateDevice while RenderDoc wraps everything
		// else leaves the game with an unwrapped device fed wrapped objects, which crashes inside the driver.
		const bool renderDocLoaded = GetModuleHandleW(L"renderdoc.dll") != nullptr;
		decltype(&::GetProcAddress) resolver = &::GetProcAddress;

		if (renderDocLoaded)
		{
			static const auto renderDocResolver = []()
			{
				const auto result = GetRenderDocResolver();

				LogGuard guard;

				if (guard.Entered)
				{
					if (result)
						RepairExeImports(result);
					else
						spdlog::error("[RenderDoc] Streamline bypass disabled: no GetProcAddress hooked by RenderDoc found");
				}

				return result;
			}();

			// Without RenderDoc's resolver, staying on Streamline's path is the only consistent option
			if (!renderDocResolver)
				return nullptr;

			resolver = renderDocResolver;
		}

		auto substitute = resolver(host, ProcName);

		if (!substitute)
			return nullptr;

		LogGuard guard;

		if (guard.Entered)
		{
			spdlog::info(
				"[RenderDoc] Streamline bypass: sl.interposer!{} -> {}!{} = {:p} [{}]",
				ProcName,
				std::filesystem::path(hostName).string(),
				ProcName,
				reinterpret_cast<void *>(substitute),
				OwnerOf(reinterpret_cast<void *>(substitute)));
		}

		// Every device the game creates comes through here, which is where the shader scopes find the
		// vtables they need
		if (ShaderScopes::IsEnabled() && _stricmp(ProcName, "D3D12CreateDevice") == 0)
			substitute = ShaderScopes::WrapCreateDevice(substitute);

		return substitute;
	}

	// Graphics runtime loads only happen once the game itself is running, after anything that rewrites the
	// exe's import table at startup has finished. That makes them the earliest dependable point to repair it.
	static void RepairOnGraphicsLoad(const wchar_t *Name)
	{
		if (!Name)
			return;

		const auto base = std::filesystem::path(Name).filename().wstring();

		if (_wcsicmp(base.c_str(), L"d3d12.dll") != 0 && _wcsicmp(base.c_str(), L"D3D12Core.dll") != 0 &&
			_wcsicmp(base.c_str(), L"dxgi.dll") != 0 && _wcsicmp(base.c_str(), L"sl.interposer.dll") != 0)
			return;

		if (!GetModuleHandleW(L"renderdoc.dll"))
			return;

		LogGuard guard;

		if (!guard.Entered)
			return;

		if (const auto resolver = GetRenderDocResolver())
			RepairExeImports(resolver);
	}

	static decltype(&::GetProcAddress) OrigGetProcAddress = nullptr;
	static decltype(&::LoadLibraryExW) OrigLoadLibraryExW = nullptr;
	static decltype(&::LoadLibraryExA) OrigLoadLibraryExA = nullptr;

	static FARPROC WINAPI HookedGetProcAddress(HMODULE Module, LPCSTR ProcName)
	{
		// HIWORD == 0 means resolution by ordinal, where ProcName is not a pointer
		if (HIWORD(ProcName) != 0)
		{
			if (const auto substitute = RedirectStreamlineProxy(Module, ProcName))
				return substitute;
		}

		return OrigGetProcAddress(Module, ProcName);
	}

	static HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR FileName, HANDLE File, DWORD Flags)
	{
		const auto result = OrigLoadLibraryExW(FileName, File, Flags);

		if (result)
			RepairOnGraphicsLoad(FileName);

		return result;
	}

	static HMODULE WINAPI HookedLoadLibraryExA(LPCSTR FileName, HANDLE File, DWORD Flags)
	{
		const auto result = OrigLoadLibraryExA(FileName, File, Flags);

		if (result && FileName)
			RepairOnGraphicsLoad(std::filesystem::path(FileName).wstring().c_str());

		return result;
	}

	void InstallStreamlineBypass()
	{
		static bool s_installed = false;

		if (s_installed)
			return;

		const auto kernel32 = GetModuleHandleW(L"kernel32.dll");

		if (!kernel32)
		{
			spdlog::error("[RenderDoc] kernel32.dll not present, cannot install the Streamline bypass");
			return;
		}

		// Resolved through the real exports rather than the IAT so the detour lands on the implementation
		OrigGetProcAddress = reinterpret_cast<decltype(&::GetProcAddress)>(::GetProcAddress(kernel32, "GetProcAddress"));
		OrigLoadLibraryExW = reinterpret_cast<decltype(&::LoadLibraryExW)>(::GetProcAddress(kernel32, "LoadLibraryExW"));
		OrigLoadLibraryExA = reinterpret_cast<decltype(&::LoadLibraryExA)>(::GetProcAddress(kernel32, "LoadLibraryExA"));

		if (!OrigGetProcAddress || !OrigLoadLibraryExW || !OrigLoadLibraryExA)
		{
			spdlog::error("[RenderDoc] Failed to resolve the Streamline bypass targets");
			return;
		}

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<void **>(&OrigGetProcAddress), &HookedGetProcAddress);
		DetourAttach(reinterpret_cast<void **>(&OrigLoadLibraryExW), &HookedLoadLibraryExW);
		DetourAttach(reinterpret_cast<void **>(&OrigLoadLibraryExA), &HookedLoadLibraryExA);

		if (DetourTransactionCommit() != NO_ERROR)
		{
			spdlog::error("[RenderDoc] Failed to commit the Streamline bypass hooks");
			return;
		}

		s_installed = true;
		spdlog::info("[RenderDoc] Streamline bypass installed");
	}
}

namespace RenderDoc
{
	void Initialize()
	{
		static bool s_initialized = false;

		if (s_initialized)
			return;

		s_initialized = true;

		const bool engineAttach = EngineWillAttach();
		const bool diagnostics = ModConfiguration.RenderDoc.Diagnostics;

		// Interceptors first so we sit underneath whatever RenderDoc installs
		if (diagnostics)
			RenderDocDiag::InstallTraceHooks(diagnostics);

		if (ModConfiguration.RenderDoc.BypassStreamline)
			InstallStreamlineBypass();

		if (diagnostics)
			RenderDocDiag::Report("before RenderDoc load");

		if (ModConfiguration.RenderDoc.Enable)
		{
			// Loading early is what makes RenderDoc's hooks land before the device is created. The
			// engine's own -attach_renderdoc load happens much later (after D3D12Core.dll is up), so
			// deferring to it would hook nothing. A bare-name load of an already-loaded module resolves
			// against the loader's module list without touching disk, so the engine's attach still
			// succeeds and binds to this exact instance.
			if (engineAttach)
				spdlog::info("[RenderDoc] -attach_renderdoc present; loading early anyway so the engine's attach binds to our instance.");

			spdlog::info("Loading RenderDoc");
			Load();
		}
		else if (engineAttach)
		{
			spdlog::warn(
				"[RenderDoc] -attach_renderdoc detected but [RenderDoc] Enable is false. The engine attaches after device "
				"creation, so hooks will likely miss. Set Enable = true.");
		}
		else
		{
			spdlog::debug("[RenderDoc] Disabled via [RenderDoc] Enable in mod_config.ini.");
		}

		if (diagnostics)
			RenderDocDiag::Report("after RenderDoc load");
	}
}
