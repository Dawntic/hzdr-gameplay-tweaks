#include <filesystem>
#include <charconv>

#define WIN32_NO_STATUS
#include <Windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>

#include "ModConfiguration.h"

#include "tlhelp32.h"
#include <detours/detours.h>

// ntdll LdrLoadDll signature
using LdrLoadDll_t = NTSTATUS(NTAPI *)(PWSTR SearchPath, PULONG LoadFlags, PUNICODE_STRING Name, PHANDLE ModuleHandle);

static LdrLoadDll_t OrigLdrLoadDll = nullptr;

static NTSTATUS NTAPI HookedLdrLoadDll(PWSTR SearchPath, PULONG LoadFlags, PUNICODE_STRING Name, PHANDLE ModuleHandle)
{
	NTSTATUS status = OrigLdrLoadDll(SearchPath, LoadFlags, Name, ModuleHandle);

	if (Name && Name->Buffer)
	{
		/*
		// Convert UNICODE_STRING to a path and log the filename
		std::wstring_view fullName(Name->Buffer, Name->Length / sizeof(wchar_t));
		// Find last backslash for filename only
		auto pos = fullName.rfind(L'\\');
		auto filename = (pos != std::wstring_view::npos) ? fullName.substr(pos + 1) : fullName;

		// Log every DLL load — this catches static imports, LoadLibrary, everything
		OutputDebugStringW(filename.data()); // use OutputDebugString — no CRT yet
		OutputDebugStringW(L"\n");

		// Check for d3d12
		if (_wcsicmp(filename.data(), L"d3d12.dll") == 0 || _wcsicmp(filename.data(), L"D3D12Core.dll") == 0)
		{
			OutputDebugStringW(L"*** D3D12 LOADING — hooking now ***\n");

			
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
					OutputDebugStringW(L"*** D3D12CreateDevice hooked ***\n");
				}
			}
			
		}
		*/
	}

	return status;
}

BOOL WINAPI RawDllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		// Check if d3d12 is already loaded at this earliest possible point
		HMODULE hD3D12 = GetModuleHandleW(L"d3d12.dll");
		HMODULE hD3D12Core = GetModuleHandleW(L"D3D12Core.dll");
		OutputDebugStringW(hD3D12 ? L"d3d12.dll ALREADY loaded in RawDllMain\n" : L"d3d12.dll not loaded yet in RawDllMain\n");
		OutputDebugStringW(hD3D12Core ? L"D3D12Core.dll ALREADY loaded in RawDllMain\n" : L"D3D12Core.dll not loaded yet in RawDllMain\n");

		// Start vsjitdebugger.exe if a debugger isn't already attached. GAME_DEBUGGER_REQUEST determines the
		// command line and GAME_DEBUGGER_PROC is used to hide the CreateProcessA IAT entry.
		if (char cmd[512] = {}, proc[512] = {}; !IsDebuggerPresent() &&
												GetEnvironmentVariableA("GAME_DEBUGGER_REQUEST", cmd, ARRAYSIZE(cmd)) > 0 &&
												GetEnvironmentVariableA("GAME_DEBUGGER_PROC", proc, ARRAYSIZE(proc)) > 0)
		{
			std::to_chars(cmd + strlen(cmd), std::end(cmd), GetCurrentProcessId());
			auto moduleName = proc;
			auto importName = strchr(proc, '!') + 1;
			importName[-1] = '\0';

			PROCESS_INFORMATION pi = {};

			STARTUPINFOA si = {};
			si.cb = sizeof(si);
			si.dwFlags = STARTF_USESHOWWINDOW;
			si.wShowWindow = SW_HIDE;

			auto c = reinterpret_cast<decltype(&CreateProcessA)>(GetProcAddress(GetModuleHandleA(moduleName), importName));
			c(nullptr, cmd, nullptr, nullptr, false, 0, nullptr, nullptr, &si, &pi);

			WaitForSingleObject(pi.hProcess, INFINITE);
			CloseHandle(pi.hProcess);
			CloseHandle(pi.hThread);
		}
	}
	return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		HMODULE hD3D12 = GetModuleHandleW(L"d3d12.dll");
		HMODULE hD3D12Core = GetModuleHandleW(L"D3D12Core.dll");
		OutputDebugStringW(hD3D12 ? L"d3d12.dll ALREADY loaded in DllMain\n" : L"d3d12.dll not loaded yet in DllMain\n");
		OutputDebugStringW(hD3D12Core ? L"D3D12Core.dll ALREADY loaded in DllMain\n" : L"D3D12Core.dll not loaded yet in DllMain\n");

		// It's extremely unlikely that a path will be this long, but it's a one time check, so who cares.
		std::wstring fullModulePath(8192, '\0');
		uint32_t len = GetModuleFileNameW(GetModuleHandleW(nullptr), fullModulePath.data(), static_cast<DWORD>(fullModulePath.size()));

		fullModulePath.resize(len);

		try
		{
			if (fullModulePath.empty())
				throw std::runtime_error("Unable to obtain executable path via GetModuleFileNameW()");

			InternalModConfig::Initialize(std::filesystem::path(fullModulePath).remove_filename());
		}
		catch (const std::exception& e)
		{
			// Use a hardcoded buffer. std::format can't convert between wchar_t and char.
			std::wstring buffer(8192, '\0');

			swprintf_s(
				buffer.data(),
				buffer.size(),
				L"An exception has occurred on startup: %hs\n\nFailed to initialize HZDR Gameplay Tweaks and Cheat Menu.\n\nExecutable path: %ws",
				e.what(),
				fullModulePath.c_str());

			MessageBoxW(nullptr, buffer.data(), L"Error", MB_ICONERROR);
		}
	}

	return TRUE;
}

extern "C" extern decltype(&RawDllMain) const _pRawDllMain = RawDllMain;
