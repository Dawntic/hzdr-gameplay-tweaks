#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <Windows.h>
#include "renderdoc_app.h"

namespace RenderDoc
{
	// Null until renderdoc.dll has been loaded (or found already loaded) and its API acquired.
	extern RENDERDOC_API_1_7_0 *Api;
	extern std::atomic_bool WantCapture;

	// True when the game was launched with -attach_renderdoc, i.e. the engine owns the RenderDoc attach.
	bool EngineWillAttach();

	// Grab the API from a renderdoc.dll somebody else already loaded. Never calls LoadLibrary.
	bool AttachToLoaded();

	// Load renderdoc.dll from the configured path and acquire its API.
	void Load();

	// One-time setup from Offsets::Initialize: trace hooks, Streamline bypass, then the early RenderDoc load.
	void Initialize();

	// Redirect the game's sl.interposer.dll D3D12/DXGI proxy lookups to the real d3d12.dll/dxgi.dll
	// exports. Those are the ones RenderDoc hooks, so this is what makes capture possible. Costs DLSS
	// frame generation. Must be called BEFORE renderdoc.dll is loaded.
	void InstallStreamlineBypass();

	// Name of the module an address lives in, or "<none/trampoline>" if it isn't inside a loaded image.
	std::string OwnerOf(const void *Address);

	bool ValidPeHeaders(uintptr_t Base, PIMAGE_NT_HEADERS& OutNt);

	// Visit(ImportDllName, FunctionName, SlotAddress, IsDelayLoad)
	using ImportVisitor = std::function<void(const char *, const char *, void **, bool)>;
	void ForEachImport(HMODULE Module, const ImportVisitor& Visit);

	// spdlog's sink takes a lock and touches the CRT; never re-enter it from a nested module resolve.
	// Hooks that log from inside GetProcAddress/LoadLibrary hold one of these around the logging.
	struct LogGuard
	{
		bool Entered = false;

		LogGuard()
		{
			if (!s_InLog)
			{
				s_InLog = true;
				Entered = true;
			}
		}

		~LogGuard()
		{
			if (Entered)
				s_InLog = false;
		}

	private:
		static inline thread_local bool s_InLog = false;
	};
}
