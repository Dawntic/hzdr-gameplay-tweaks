#include "../RenderDoc/ShaderScopes.h"
#include "../ModConfiguration.h"

#include <d3d12.h>
#include <atomic>

//
// BlackSpace engine render pass names.
//
// Every engine pipeline belongs to a render pass defined in the game data (the RenderPassName field of a
// pipeline record). The engine never passes those names to D3D12, but its pipeline state builder has them
// on hand: it is called with the engine pipeline object, and the D3D12 creation happens inside it on the
// same thread. Hooking it lets the shader scopes group each pass's work under a section named after the pass.
//
// Layouts, traced from the device-lost reporter and the "Failed to create PipelineStateObject()" message:
//   pipeline + 0x08  the render pass object, whose +0x28 is the pass's name ID
//   pipeline + 0x24  the pipeline's own name ID (printed as "PipelineId")
//   name table       *(*g_NameTable + 0x58) is an array of 16 byte entries starting with a char *
//
namespace CrimsonDesert::EnginePipelines
{
	// Only the first argument is understood. The rest are forwarded untouched, with two spare in case the
	// builder reads stack arguments; forwarding extra slots only copies the caller's own stack.
	using BuildPipelineStateFn = uintptr_t (*)(const uint8_t *, void *, void *, void *, void *, void *);

	static BuildPipelineStateFn OriginalBuildPipelineState = nullptr;
	static uintptr_t *g_NameTable = nullptr;

	// Engine memory is read under SEH so a layout change in a game update degrades to "no name" instead of a
	// crash. These functions hold no C++ objects, which __try requires.
	static bool ReadNameIds(const uint8_t *Pipeline, uint32_t& PassId, uint32_t& NameId)
	{
		__try
		{
			const auto pass = *reinterpret_cast<const uint8_t *const *>(Pipeline + 0x08);

			if (!pass)
				return false;

			PassId = *reinterpret_cast<const uint32_t *>(pass + 0x28);
			NameId = *reinterpret_cast<const uint32_t *>(Pipeline + 0x24);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	template<size_t N>
	static void CopyName(uint32_t Id, char (&Out)[N])
	{
		Out[0] = '\0';

		__try
		{
			const auto table = *g_NameTable;

			if (!table)
				return;

			const auto entries = *reinterpret_cast<const uintptr_t *>(table + 0x58);

			if (!entries)
				return;

			const auto name = *reinterpret_cast<const char *const *>(entries + static_cast<uintptr_t>(Id) * 16);

			if (!name)
				return;

			size_t i = 0;

			for (; i + 1 < N && name[i] != '\0'; i++)
				Out[i] = name[i];

			Out[i] = '\0';
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Out[0] = '\0';
		}
	}

	static uintptr_t HookedBuildPipelineState(const uint8_t *Pipeline, void *Arg2, void *Arg3, void *Arg4, void *Arg5, void *Arg6)
	{
		char pass[128];
		char name[128];
		pass[0] = name[0] = '\0';

		uint32_t passId = 0;
		uint32_t nameId = 0;

		if (Pipeline && ReadNameIds(Pipeline, passId, nameId))
		{
			CopyName(passId, pass);
			CopyName(nameId, name);
		}

		ShaderScopes::BeginEnginePipeline(pass, name, nullptr);
		const auto result = OriginalBuildPipelineState(Pipeline, Arg2, Arg3, Arg4, Arg5, Arg6);
		ShaderScopes::EndEnginePipeline();

		return result;
	}

	DECLARE_HOOK_TRANSACTION(EnginePipelines)
	{
		if (!ModConfiguration.RenderDoc.ShaderMarkers || !ModConfiguration.RenderDoc.EnginePassMarkers)
			return true;

		// The engine's PSO builder, taking the engine pipeline first. It handles both graphics and compute
		// ("Failed to create PipelineStateObject()" lives in it).
		const uintptr_t builderAddress = Offsets::Signature(
			"4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 08 F8 FF FF 48 81 EC F8 08 00 "
			"00 49 8B F0 4C 8B E9 48 8B 02");

		// Its own lookup of the pipeline and pass names, which loads the name table global
		g_NameTable = Offsets::Signature(
						  "48 8B 05 ? ? ? ? 48 8B 48 58 41 8B 45 24 48 03 C0 48 8B 04 C1 48 89 45 A8 49 8B 45 08 8B 40 28")
						  .AsRipRelative(7)
						  .ToPointer<uintptr_t>();

		spdlog::info("[EnginePipelines] Pipeline state builder at {:#x}, name table at {:p}", builderAddress, static_cast<void *>(g_NameTable));

		return Hooks::WriteJump(builderAddress, &HookedBuildPipelineState, &OriginalBuildPipelineState);
	};
}
