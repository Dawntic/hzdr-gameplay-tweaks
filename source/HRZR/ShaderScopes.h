#pragma once

#include <Windows.h>

//
// Names frame regions after the shaders doing the work.
//
// Some engines never emit a GPU marker or name a command list (Crimson Desert's BlackSpace engine doesn't),
// so RenderDoc shows a flat list of Dispatch/ExecuteIndirect calls. Their shaders still carry the entry
// point name in the DXIL container's PSV0 part, though. This remembers that name for every pipeline state
// the game creates and opens a PIX scope named after it at each SetPipelineState, closing it at the next
// one or when the command list closes. Every draw and dispatch ends up nested under its shader's name.
//
// Engine agnostic: it only touches D3D12 vtables, never game code.
//
namespace ShaderScopes
{
	bool IsEnabled();

	// Wraps a D3D12CreateDevice entry point so every device it returns gets its vtable hooked. The Streamline
	// bypass hands this to the game in place of the real export.
	FARPROC WrapCreateDevice(FARPROC CreateDevice);
}
