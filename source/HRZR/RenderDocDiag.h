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
	// Detour GetProcAddress/LoadLibrary*. Must be called BEFORE renderdoc.dll is loaded so that we sit
	// underneath RenderDoc's own IAT patches and observe every resolution attempt.
	//
	//   Trace            - log every watched symbol resolution.
	//   BypassStreamline - answer the game's sl.interposer.dll D3D12/DXGI proxy lookups with the real
	//                      d3d12.dll/dxgi.dll exports instead. Those are the ones RenderDoc hooks, so this
	//                      is what makes capture possible. Costs DLSS frame generation.
	void InstallTraceHooks(bool Trace, bool BypassStreamline);

	// Full snapshot: module load order, IAT ownership, export resolution truth table.
	void Report(const char *Phase);

	// Actually create a throwaway device/factory through the exact code path the game uses, then report
	// which module owns the returned vtables. This is the control experiment that splits
	// "RenderDoc's hooks are dead" from "the game bypasses RenderDoc's hooks".
	void ProbeCreation();

	// Report the owning module of the vtables of the live objects the game is actually rendering with.
	void ReportLiveObjects(void *NxD3DImplPtr, void *NxDXGIImplPtr);

	// Name of the module an address lives in, or "<none/trampoline>" if it isn't inside a loaded image.
	std::string OwnerOf(const void *Address);
}
