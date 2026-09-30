#include <execution>
#include <emmintrin.h>
#define WIN32_NO_STATUS
#include <Windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>


namespace Offsets::detail
{
	static std::vector<SignatureStorageWrapper *>& GetInitializationEntries()
	{
		static std::vector<SignatureStorageWrapper *> entries;
		return entries;
	}

	SignatureStorageWrapper::SignatureStorageWrapper(const PatternSpan& Signature, const char *File, size_t Line)
		: m_Signature(Signature),
		  m_File(File),
		  m_Line(Line)
	{
		GetInitializationEntries().emplace_back(this);
	}

	PatternSpan SignatureStorageWrapper::FindLongestNonWildcardRun() const
	{
		PatternSpan longestRun = {};

		for (size_t i = 0; i < m_Signature.size(); i++)
		{
			if (m_Signature[i].Wildcard)
				continue;

			for (size_t j = m_Signature.size(); j-- > i;)
			{
				if (!m_Signature[j].Wildcard)
				{
					longestRun = m_Signature.subspan(i, j - i + 1);
					break;
				}
			}

			break;
		}

		return longestRun;
	}

	bool SignatureStorageWrapper::MatchPattern(ByteSpan::iterator Iterator) const
	{
		return std::equal(
			m_Signature.begin(),
			m_Signature.end(),
			Iterator,
			[](const auto& A, const auto& B)
		{
			return A.Wildcard || A.Value == B;
		});
	}

	ByteSpan::iterator SignatureStorageWrapper::ScanRegion(const ByteSpan& Region) const
	{
		if (m_Signature.empty() || m_Signature.size() > Region.size())
			return Region.end();

		const auto nonWildcardSubrange = FindLongestNonWildcardRun();

		if (nonWildcardSubrange.empty())
			return Region.begin();

		const auto subrangeAdjustment = nonWildcardSubrange.data() - m_Signature.data();
		const auto scanStart = Region.begin() + subrangeAdjustment;
		const auto scanEnd = (Region.end() - m_Signature.size()) + subrangeAdjustment;

		auto pos = scanStart;

		const ptrdiff_t perIterSize = sizeof(__m128i) * 2;
		ptrdiff_t iterCount = (scanEnd - scanStart) / perIterSize;

		const auto firstBlockMask = _mm_set1_epi8(nonWildcardSubrange.front().Value);
		const auto lastBlockMask = _mm_set1_epi8(nonWildcardSubrange.back().Value);

		auto loadMask = [&](const uint32_t VectorIndex)
		{
			const uint32_t offset = VectorIndex * sizeof(__m128i);

			const auto firstBlock = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&pos[offset]));
			const auto lastBlock = _mm_loadu_si128(reinterpret_cast<const __m128i *>(&pos[offset + nonWildcardSubrange.size() - 1]));
			const auto mask = _mm_and_si128(_mm_cmpeq_epi8(firstBlockMask, firstBlock), _mm_cmpeq_epi8(lastBlockMask, lastBlock));

			return static_cast<uint32_t>(_mm_movemask_epi8(mask)) << offset;
		};

		for (; iterCount > 0; iterCount--, pos += perIterSize)
		{
			auto mask = loadMask(0) | loadMask(1);

			while (mask != 0) [[unlikely]]
			{
				const auto bitIndex = _tzcnt_u32(mask);
				mask &= (mask - 1);

				if (MatchPattern(pos + bitIndex - subrangeAdjustment)) [[unlikely]]
					return pos + bitIndex - subrangeAdjustment;
			}
		}

		for (; pos <= scanEnd; pos++)
		{
			if (MatchPattern(pos - subrangeAdjustment))
				return pos - subrangeAdjustment;
		}

		return Region.end();
	}
}

#include <algorithm>
#include <filesystem>
#include "test.h"
#include "ModConfiguration.h"
#include "HRZR/RenderDocDiag.h"
#include "HRZR/RenderPassNames.h"
RENDERDOC_API_1_7_0 *renderDocApi = nullptr;
std::atomic_bool g_wantCapture = false;

// The engine has its own RenderDoc integration behind -attach_renderdoc (see the renderdoc.dll /
// RENDERDOC_GetAPI / source:Tools/RenderDoc/renderdoc.dll strings in the executable). If that's in play we
// must stay out of the way entirely: renderdoc.dll installs its API hooks from DllMain, so a second
// LoadLibrary just bumps the refcount and returns the existing handle without re-hooking anything. Loading
// it ourselves first would therefore *prevent* the engine's attach from ever hooking properly.
bool EngineWillAttachRenderDoc()
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

	if (RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_7_0, (void **)&renderDocApi) != 1 || !renderDocApi)
	{
		spdlog::warn("[RenderDoc] Failed to get API interface");
		renderDocApi = nullptr;
		return false;
	}

	// Only override the capture template when explicitly configured, so we never clobber whatever the
	// engine set up for itself.
	if (!ModConfiguration.RenderDoc.CapturePath.empty())
	{
		const std::filesystem::path capturePath = ModConfiguration.RenderDoc.CapturePath;

		std::error_code ec;
		std::filesystem::create_directories(capturePath.parent_path(), ec);

		renderDocApi->SetCaptureFilePathTemplate(capturePath.string().c_str());
	}

	spdlog::info("[RenderDoc] API acquired. Capture template: {}", renderDocApi->GetCaptureFilePathTemplate());
	return true;
}

// Attach to a renderdoc.dll somebody else already loaded (i.e. the engine's -attach_renderdoc path).
// Never calls LoadLibrary, so it cannot influence hook installation.
bool AttachToLoadedRenderDoc()
{
	if (renderDocApi)
		return true;

	const auto renderDocModule = GetModuleHandleW(L"renderdoc.dll");

	if (!renderDocModule)
		return false;

	if (!AcquireRenderDocApi(renderDocModule))
		return false;

	spdlog::info("[RenderDoc] Attached to an already-loaded renderdoc.dll");
	return true;
}

void LoadRenderDoc()
{
	if (renderDocApi)
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


#include <d3d12.h>
static decltype(&D3D12CreateDevice) OrigD3D12CreateDevice = nullptr;
static HRESULT WINAPI HookedD3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void **ppDevice)
{
	spdlog::info("[RenderDoc] D3D12CreateDevice called");

	OutputDebugStringW(L"------D3D12CreateDevice Called------------------------");

	HRESULT hr = OrigD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);

	if (SUCCEEDED(hr) && ppDevice && *ppDevice)
	{
		void **vtable = *reinterpret_cast<void ***>(*ppDevice);

		// Check the first few vtable slots
		for (int i = 0; i < 4; i++)
		{
			HMODULE hOwner = nullptr;
			GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(vtable[i]),
				&hOwner);

			wchar_t modName[MAX_PATH] {};
			GetModuleFileNameW(hOwner, modName, MAX_PATH);

			spdlog::info(
				"[RenderDoc] Device vtable[{}] = {:p} owned by {}",
				i,
				vtable[i],
				std::filesystem::path(modName).filename().string());
		}
	}

	return hr;
}

#include "tlhelp32.h"
#include <detours/detours.h>

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
		if (NT_SUCCESS(Status) && ModConfiguration.RenderDoc.NameCommandLists &&
			_wcsicmp(filename.c_str(), L"d3d12.dll") == 0)
			HRZR::RenderPassNames::SetEnabled(true);

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
	g_EngineAttachRequested = EngineWillAttachRenderDoc();

	// Hook LdrLoadDll to catch anything that loads afterward
	HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");

	auto addr = reinterpret_cast<uintptr_t>(GetProcAddress(hNtdll, "LdrLoadDll"));

	return Hooks::WriteJump(addr, &HookedLdrLoadDll, reinterpret_cast<void **>(&OrigLdrLoadDll));
};

namespace Offsets
{
	using namespace detail;

	bool Initialize()
	{
		spdlog::info("{}():", __FUNCTION__);

		static bool RDLoaded = false;
		if (!RDLoaded)
		{
			RDLoaded = true;

			const bool engineAttach = EngineWillAttachRenderDoc();
			const bool diagnostics = ModConfiguration.RenderDoc.Diagnostics;
			const bool bypassStreamline = ModConfiguration.RenderDoc.BypassStreamline;

			// Interceptors first so we sit underneath whatever RenderDoc installs
			if (diagnostics || bypassStreamline)
				RenderDocDiag::InstallTraceHooks(diagnostics, bypassStreamline);

			// AGS is the only marker API this executable references, so it is the only place engine frame
			// annotations could come from.
			if (diagnostics || ModConfiguration.RenderDoc.TranslateAgsMarkers)
				RenderDocDiag::InstallAgsMarkerBridge(diagnostics, ModConfiguration.RenderDoc.TranslateAgsMarkers);

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
				LoadRenderDoc();
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

		auto dosHeader = reinterpret_cast<const PIMAGE_DOS_HEADER>(GetModuleHandleW(nullptr));
		auto ntHeaders = reinterpret_cast<const PIMAGE_NT_HEADERS>(reinterpret_cast<uintptr_t>(dosHeader) + dosHeader->e_lfanew);
		auto region = ByteSpan { reinterpret_cast<const uint8_t *>(dosHeader), ntHeaders->OptionalHeader.SizeOfImage };

		auto entries = std::move(GetInitializationEntries());

		std::for_each(
			std::execution::seq,
			entries.begin(),
			entries.end(),
			[&region](auto& P)
		{
			if (const auto itr = P->ScanRegion(region); itr != region.end())
			{
				P->m_Address = reinterpret_cast<uintptr_t>(std::to_address(itr));
				P->m_IsResolved = true;
			}
		});

		const auto failedSignatureCount = std::ranges::count_if(
			entries,
			[](const auto& P)
		{
			if (!P->m_IsResolved && P->m_File)
				spdlog::warn("Failed to resolve signature at {}:{}.", P->m_File, P->m_Line);

			return !P->m_IsResolved;
		});

		if (failedSignatureCount > 0)
		{
			spdlog::error("Failed to resolve {} out of {} signatures.", failedSignatureCount, entries.size());
			return false;
		}

		spdlog::info("Done!");
		return true;
	}

	Offset Relative(uintptr_t RelAddress)
	{
		return Offset(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + RelAddress);
	}

	Offset Absolute(uintptr_t AbsAddress)
	{
		return Offset(AbsAddress);
	}
}
