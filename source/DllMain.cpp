#include <filesystem>
#include <Windows.h>
#include "ModConfiguration.h"

BOOL WINAPI DllMain(HINSTANCE hInstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
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
				L"An exception has occurred on startup: %hs\n\nFailed to initialize the HZDR RenderDoc support mod.\n\nExecutable "
				L"path: %ws",
				e.what(),
				fullModulePath.c_str());

			MessageBoxW(nullptr, buffer.data(), L"Error", MB_ICONERROR);
		}
	}

	return TRUE;
}
