#pragma once

#include <string>

//
// Diagnostics for "RenderDoc loaded but never wrapped anything".
//
// The failure mode being chased is RenderDoc reporting:
//   Couldn't find matching frame capturer ... from 0 device frame capturers and 0 frame capturers
// which means neither D3D12CreateDevice nor CreateDXGIFactory* was ever intercepted by renderdoc.dll.
//
// Everything here is read-only observation apart from InstallTraceHooks(), which detours
// GetProcAddress/LoadLibrary* purely to log. Nothing changes game behaviour.
//
namespace RenderDocDiag
{
	// Detour GetProcAddress/LoadLibrary* to log watched resolutions. Must be called BEFORE renderdoc.dll is
	// loaded so that we sit underneath RenderDoc's own IAT patches and observe every resolution attempt.
	void InstallTraceHooks(bool Trace);

	// Snapshot of the command line and the module load order.
	void Report(const char *Phase);
}
