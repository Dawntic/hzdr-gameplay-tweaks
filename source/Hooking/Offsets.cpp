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

#include <filesystem>
#include "test.h"
RENDERDOC_API_1_7_0 *renderDocApi = nullptr;
void LoadRenderDoc()
{
	std::filesystem::path renderdocPath = L"C:\\Program Files\\RenderDoc\\renderdoc.dll";

	if (!std::filesystem::exists(renderdocPath))
	{
		spdlog::debug("[RenderDoc] renderdoc.dll not found at: {}", renderdocPath.string());
		return;
	}

	HMODULE renderDocModule = LoadLibraryW(renderdocPath.wstring().c_str());
	spdlog::debug("[RenderDoc] Attempting to load renderdoc.dll from {}", renderdocPath.string());

	if (!renderDocModule)
	{
		spdlog::debug("[RenderDoc] Failed to load renderdoc.dll");
		return;
	}

	spdlog::info("[RenderDoc] Loaded renderdoc.dll from {}", renderdocPath.string());

	auto RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)GetProcAddress(renderDocModule, "RENDERDOC_GetAPI");
	if (!RENDERDOC_GetAPI)
	{
		spdlog::info("[RenderDoc] Failed to get RENDERDOC_GetAPI");
		FreeLibrary(renderDocModule);
		return;
	}

	int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_7_0, (void **)&renderDocApi);
	if (ret != 1 || !renderDocApi)
	{
		spdlog::info("[RenderDoc] Failed to get API interface");
		FreeLibrary(renderDocModule);
		renderDocApi = nullptr;
		return;
	}

	std::filesystem::path captureDir = L"C:\\Users\\RG\\Desktop\\HZD_Render_Caps";
	std::filesystem::create_directories(captureDir);
	renderDocApi->SetCaptureFilePathTemplate((captureDir / "capture").string().c_str());
	spdlog::info("[RenderDoc] Capture path set to {}", (captureDir / "capture").string());
}





#include <d3d12.h>
static decltype(&D3D12CreateDevice) OrigD3D12CreateDevice = nullptr;

static HRESULT WINAPI HookedD3D12CreateDevice(IUnknown *pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID riid, void **ppDevice)
{
	spdlog::info("[RenderDoc] D3D12CreateDevice called");
	return OrigD3D12CreateDevice(pAdapter, MinimumFeatureLevel, riid, ppDevice);
}

/*
DECLARE_HOOK_TRANSACTION(D3D12CreateDevice)
{
	HMODULE hD3D12 = GetModuleHandleW(L"d3d12.dll");
	if (!hD3D12)
	{
		spdlog::warn("[RenderDoc] d3d12.dll not loaded yet... skipping hook");
		return true; // non-fatal, don't abort the transaction
	}

	auto addr = reinterpret_cast<uintptr_t>(GetProcAddress(hD3D12, "D3D12CreateDevice"));
	if (!addr)
	{
		spdlog::warn("[RenderDoc] D3D12CreateDevice not found... skipping hook");
		return true; // non-fatal
	}

	return Hooks::WriteJump(addr, &HookedD3D12CreateDevice, reinterpret_cast<void **>(&OrigD3D12CreateDevice));
};
*/

#include "tlhelp32.h"
#include <detours/detours.h>

/*
static decltype(&LoadLibraryExW) OrigLoadLibraryExW = nullptr;

static HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
	HMODULE hMod = OrigLoadLibraryExW(lpLibFileName, hFile, dwFlags);

	if (lpLibFileName)
	{
		auto name = std::filesystem::path(lpLibFileName).filename().wstring();
		std::transform(name.begin(), name.end(), name.begin(), ::towlower);
		spdlog::info("[LoadLibrary] {}", std::filesystem::path(lpLibFileName).filename().string());

		if (name == L"d3d12core.dll"){
			spdlog::info("[RenderDoc] D3D12Core.dll exports:");
			auto dos = reinterpret_cast<PIMAGE_DOS_HEADER>(hMod);
			auto nt = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<uintptr_t>(hMod) + dos->e_lfanew);
			auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
			if (dir.VirtualAddress)
			{
				auto exports = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(reinterpret_cast<uintptr_t>(hMod) + dir.VirtualAddress);
				auto names = reinterpret_cast<uint32_t *>(reinterpret_cast<uintptr_t>(hMod) + exports->AddressOfNames);
				for (uint32_t i = 0; i < exports->NumberOfNames; i++)
					spdlog::info("  {}", reinterpret_cast<const char *>(reinterpret_cast<uintptr_t>(hMod) + names[i]));
			}
		}
	}

	return hMod;
}


DECLARE_HOOK_TRANSACTION(LoadLibraryExW)
{
	// Check if d3d12 is already loaded
	for (const wchar_t *dll : { L"d3d12.dll", L"D3D12Core.dll" })
	{
		HMODULE hMod = GetModuleHandleW(dll);
		spdlog::info("[RenderDoc] {} at startup: {:p}", std::filesystem::path(dll).filename().string(), static_cast<void *>(hMod));

		if (hMod)
		{
			auto addr = reinterpret_cast<uintptr_t>(GetProcAddress(hMod, "D3D12CreateDevice"));
			if (addr)
			{
				// Already inside the transaction — WriteJump adds to it directly
				Hooks::WriteJump(addr, &HookedD3D12CreateDevice, reinterpret_cast<void **>(&OrigD3D12CreateDevice));
				spdlog::info("[RenderDoc] Queued D3D12CreateDevice hook in {}", std::filesystem::path(dll).filename().string());
			}
		}
	}

	// Module snapshot
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	MODULEENTRY32W me { .dwSize = sizeof(me) };
	if (Module32FirstW(hSnap, &me))
	{
		do
		{
			spdlog::info("[Modules] {}", std::filesystem::path(me.szExePath).string());
		} while (Module32NextW(hSnap, &me));
	}
	CloseHandle(hSnap);

	return Hooks::WriteJump(
		reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryExW")),
		&HookedLoadLibraryExW,
		reinterpret_cast<void **>(&OrigLoadLibraryExW));
};
*/


// ntdll LdrLoadDll signature
using LdrLoadDll_t = NTSTATUS(NTAPI *)(PWSTR SearchPath, PULONG LoadFlags, PUNICODE_STRING Name, PHANDLE ModuleHandle);

static LdrLoadDll_t OrigLdrLoadDll = nullptr;

static NTSTATUS NTAPI HookedLdrLoadDll(PWSTR SearchPath, PULONG LoadFlags, PUNICODE_STRING Name, PHANDLE ModuleHandle)
{
	NTSTATUS status = OrigLdrLoadDll(SearchPath, LoadFlags, Name, ModuleHandle);

	if (Name && Name->Buffer && Name->Length > 0)
	{
		// Copy to std::wstring so we have a null-terminated string
		std::wstring fullName(Name->Buffer, Name->Length / sizeof(wchar_t));
		auto pos = fullName.rfind(L'\\');
		std::wstring filename = (pos != std::wstring::npos) ? fullName.substr(pos + 1) : fullName;

		spdlog::info("[LdrLoadDll] {}", std::filesystem::path(filename).string());

		if (_wcsicmp(filename.c_str(), L"d3d12.dll") == 0 || _wcsicmp(filename.c_str(), L"D3D12Core.dll") == 0)
		{
			spdlog::info("[RenderDoc] *** {} loading ***", std::filesystem::path(filename).string());
			/*
			if (ModuleHandle && *ModuleHandle)
			{
				HMODULE hMod = static_cast<HMODULE>(*ModuleHandle);
				auto addr = reinterpret_cast<uintptr_t>(GetProcAddress(hMod, "D3D12CreateDevice"));

				if (addr)
				{
					OrigD3D12CreateDevice = reinterpret_cast<decltype(&D3D12CreateDevice)>(addr);
					DetourTransactionBegin();
					DetourUpdateThread(GetCurrentThread());
					DetourAttach(reinterpret_cast<void **>(&OrigD3D12CreateDevice), &HookedD3D12CreateDevice);
					DetourTransactionCommit();
					spdlog::info("[RenderDoc] D3D12CreateDevice hooked in {}", std::filesystem::path(filename).string());
				}
				else
				{
					spdlog::warn("[RenderDoc] D3D12CreateDevice not exported from {}", std::filesystem::path(filename).string());
				}
			}
			*/
		}
	}

	return status;
}

DECLARE_HOOK_TRANSACTION(LdrLoadDll)
{
	HMODULE hD3D12 = GetModuleHandleW(L"d3d12.dll");
	HMODULE hD3D12Core = GetModuleHandleW(L"D3D12Core.dll");
	OutputDebugStringW(
		hD3D12 ? L"d3d12.dll ALREADY loaded in DECLARE_HOOK_TRANSACTION\n" : L"d3d12.dll not loaded yet in Offsets::DECLARE_HOOK_TRANSACTION\n");
	OutputDebugStringW(
		hD3D12Core ? L"D3D12Core.dll ALREADY loaded in Offsets::DECLARE_HOOK_TRANSACTION\n" : L"D3D12Core.dll not loaded yet in Offsets::DECLARE_HOOK_TRANSACTION\n");

	// Hook LdrLoadDll to catch anything that loads afterward
	HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
	if (!hNtdll)
	{
		spdlog::warn("[RenderDoc] ntdll.dll not found");
		return true;
	}

	auto addr = reinterpret_cast<uintptr_t>(GetProcAddress(hNtdll, "LdrLoadDll"));
	if (!addr)
	{
		spdlog::warn("[RenderDoc] LdrLoadDll not found in ntdll");
		return true;
	}

	return Hooks::WriteJump(addr, &HookedLdrLoadDll, reinterpret_cast<void **>(&OrigLdrLoadDll));
};









namespace Offsets
{
	using namespace detail;

	bool Initialize()
	{
		spdlog::info("{}():", __FUNCTION__);

		//LoadRenderDoc();
		HMODULE hD3D12 = GetModuleHandleW(L"d3d12.dll");
		HMODULE hD3D12Core = GetModuleHandleW(L"D3D12Core.dll");
		OutputDebugStringW(hD3D12 ? L"d3d12.dll ALREADY loaded in Offsets::Initialize\n" : L"d3d12.dll not loaded yet in Offsets::Initialize\n");
		OutputDebugStringW(hD3D12Core ? L"D3D12Core.dll ALREADY loaded in Offsets::Initialize\n" : L"D3D12Core.dll not loaded yet in Offsets::Initialize\n");

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
