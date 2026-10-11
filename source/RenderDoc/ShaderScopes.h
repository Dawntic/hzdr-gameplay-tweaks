#pragma once

#include <Windows.h>

struct D3D12_PIPELINE_STATE_STREAM_DESC;

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

	//
	// Engine context. An engine hook brackets its own pipeline creation with these, and any pipeline state
	// D3D12 creates in between gets Pass as an outer section, so a pass's draws and dispatches are grouped
	// above the per-shader scopes. Name, when given, replaces the shader label.
	//
	// Creation on this same thread is matched directly. The stream's shaders are remembered too, which
	// covers engines that hand the actual D3D12 call to another thread.
	//
	void BeginEnginePipeline(const char *Pass, const char *Name, const D3D12_PIPELINE_STATE_STREAM_DESC *Stream);
	void EndEnginePipeline();
}
