#include "RenderPassNames.h"
#include "PCore/String.h"
#include "../ModConfiguration.h"

#include <d3d12.h>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace HRZR::RenderPassNames
{
	// The gate: `cmp byte ptr [rip + flag], al` / `je <skip>` at the top of the command list open path,
	// immediately followed by `mov edx, [rbx + 0x14]` / `mov edi, edx` unpacking the RenderOrder bitfield.
	// Two read sites exist in the binary and no writer other than the zeroing CRT initializer.
	static uint8_t *GetGateFlag()
	{
		static const auto flag = Offsets::Signature("38 05 ? ? ? ? 0F 84 ? ? ? ? 8B 53 14 8B FA")
									.AsRipRelative(6)
									.ToPointer<uint8_t>();

		return flag;
	}

	bool IsAvailable()
	{
		return GetGateFlag() != nullptr;
	}

	bool IsEnabled()
	{
		const auto flag = GetGateFlag();
		return flag && *flag != 0;
	}

	void SetEnabled(bool Enabled)
	{
		const auto flag = GetGateFlag();

		if (!flag)
		{
			spdlog::warn("[PassNames] Gate flag not resolved, command list naming unavailable");
			return;
		}

		Memory::Patch(reinterpret_cast<uintptr_t>(flag), { static_cast<uint8_t>(Enabled ? 1 : 0) });
		spdlog::info("[PassNames] Command list naming {}", Enabled ? "enabled" : "disabled");
	}

	//
	// Label formatting
	//
	// The engine hands us the whole descriptor:
	//   "frame : 11445 RenderOrder: 0x00001002 target: gfx queue:0 vp:0 order:ORDER_X suborder:0 sync:2"
	// and we reduce it to a readable title: "Order X 0".
	//

	// Tokens that read badly when plainly title cased. Extend freely, it is just a lookup.
	static constexpr std::pair<std::string_view, std::string_view> TokenOverrides[] = {
		{ "RENDERDATA", "Render Data" },
		{ "WORLDDATA", "World Data" },
		{ "GBUFFER", "GBuffer" },
		{ "FORCEFIELD", "Force Field" },
		{ "PRELIGHTS", "Pre Lights" },
		{ "POSTLIGHTS", "Post Lights" },
		{ "PREPASS", "Prepass" },
		{ "PREALPHA", "Pre Alpha" },
		{ "SKYDOME", "Sky Dome" },
		{ "LOWRES", "Low Res" },
		{ "FULLRESFWD", "Full Res Forward" },
		{ "FULLRESCUSTOMFWD", "Full Res Custom Forward" },
		{ "FWDLOWRES", "Forward Low Res" },
		{ "FWDFOREGRND", "Forward Foreground" },
		{ "FWDBG", "Forward Background" },
		{ "FWD", "Forward" },
		{ "GEOM", "Geometry" },
		{ "ENV", "Environment" },
		// Acronyms that have to stay upper case
		{ "SSAO", "SSAO" },
		{ "HUD", "HUD" },
		{ "GPU", "GPU" },
		{ "AA", "AA" },
		{ "UI", "UI" },
		{ "LOD", "LOD" },
		{ "2D", "2D" },
		{ "3D", "3D" },
	};

	static std::string TitleCase(std::string_view Token)
	{
		for (const auto& [from, to] : TokenOverrides)
		{
			if (Token == from)
				return std::string(to);
		}

		std::string result(Token);

		for (size_t i = 0; i < result.size(); i++)
		{
			const auto c = static_cast<unsigned char>(result[i]);
			result[i] = static_cast<char>((i == 0) ? toupper(c) : tolower(c));
		}

		return result;
	}

	// Value of "<Key><value> " inside the descriptor. Keys carry their leading space so that " order:"
	// cannot match the tail of "suborder:".
	static std::string_view ExtractField(std::string_view Text, std::string_view Key)
	{
		const auto position = Text.find(Key);

		if (position == std::string_view::npos)
			return {};

		auto value = Text.substr(position + Key.size());
		const auto end = value.find(' ');

		return (end == std::string_view::npos) ? value : value.substr(0, end);
	}

	static std::string BuildLabel(std::string_view Order, std::string_view Suborder)
	{
		std::string label;

		for (size_t i = 0; i <= Order.size();)
		{
			const auto next = Order.find('_', i);
			const auto count = (next == std::string_view::npos) ? Order.size() - i : next - i;

			if (count > 0)
			{
				if (!label.empty())
					label += ' ';

				label += TitleCase(Order.substr(i, count));
			}

			if (next == std::string_view::npos)
				break;

			i = next + 1;
		}

		if (!Suborder.empty())
		{
			label += ' ';
			label += Suborder;
		}

		return label;
	}

	//
	// Scoped markers
	//
	// SetName labels an object and produces nothing in RenderDoc's event tree. Frame structure only comes
	// from markers, and the engine's only marker sink is AMD AGS, which never initializes on non-AMD
	// hardware. So open a PIX scope when the engine names a command list and close it when that command
	// list closes, which nests everything recorded in between underneath the pass name.
	//
	using CloseFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *);

	// Close sits at byte offset 0x48 in ID3D12GraphicsCommandList's vtable
	static constexpr uint32_t CloseSlot = 9;

	static std::mutex g_ScopeMutex;
	static std::unordered_map<void **, CloseFn> g_PatchedVtables;
	static std::unordered_set<ID3D12GraphicsCommandList *> g_OpenScopes;

	static HRESULT STDMETHODCALLTYPE HookedClose(ID3D12GraphicsCommandList *This)
	{
		CloseFn original = nullptr;
		bool closeScope = false;

		{
			std::lock_guard lock(g_ScopeMutex);

			if (const auto entry = g_PatchedVtables.find(*reinterpret_cast<void ***>(This)); entry != g_PatchedVtables.end())
				original = entry->second;

			closeScope = g_OpenScopes.erase(This) > 0;
		}

		// EndEvent has to land before Close, and outside the lock: it is a recording call on this thread
		if (closeScope)
			This->EndEvent();

		return original(This);
	}

	static void OpenPassScope(void *Object, const String *Name)
	{
		const std::string_view text(Name->data(), Name->size());

		// Command lists are also named at creation time ("CommandList_Direct"), where the list is still
		// closed and no recording call is legal. Only the open-time descriptor carries an order field.
		const auto order = ExtractField(text, " order:");

		if (order.empty())
			return;

		// The same helper names fences, queues and allocators too
		ID3D12GraphicsCommandList *probe = nullptr;

		if (FAILED(static_cast<IUnknown *>(Object)->QueryInterface(IID_PPV_ARGS(&probe))) || !probe)
			return;

		probe->Release();

		// The engine calls SetDescriptorHeaps on this exact pointer a few instructions later, so it is both
		// a graphics command list and already recording.
		const auto commandList = static_cast<ID3D12GraphicsCommandList *>(Object);
		const auto label = BuildLabel(order, ExtractField(text, " suborder:"));

		bool closeStaleScope = false;

		{
			std::lock_guard lock(g_ScopeMutex);

			// Register the original before patching, so a concurrent Close on a sibling list always resolves
			if (auto vtable = *reinterpret_cast<void ***>(commandList); !g_PatchedVtables.contains(vtable))
			{
				g_PatchedVtables.emplace(vtable, reinterpret_cast<CloseFn>(vtable[CloseSlot]));
				Hooks::WriteVirtualFunction(reinterpret_cast<uintptr_t>(vtable), CloseSlot, &HookedClose);
			}

			// Renamed without closing: end the previous scope so the pairing stays balanced
			closeStaleScope = g_OpenScopes.contains(commandList);
			g_OpenScopes.insert(commandList);
		}

		if (closeStaleScope)
			commandList->EndEvent();

		commandList->BeginEvent(1, label.c_str(), static_cast<UINT>(label.size() + 1));
	}

	// The engine's SetName wrapper: converts the narrow String to UTF-16 and calls the object's vtable
	// slot 0x30, i.e. ID3D12Object::SetName.
	static void (*OriginalNameD3DObject)(void *Object, const String *Name);

	static void HookedNameD3DObject(void *Object, const String *Name)
	{
		const bool usable = Object && Name && Name->data() && !Name->empty();

		if (usable && ModConfiguration.RenderDoc.LogCommandListNames)
			spdlog::info("[PassName] {}", *Name);

		OriginalNameD3DObject(Object, Name);

		if (usable && ModConfiguration.RenderDoc.EmitPassMarkers)
			OpenPassScope(Object, Name);
	}

	DECLARE_HOOK_TRANSACTION(RenderPassNames)
	{
		const auto target = Offsets::Signature(
			"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 48 8B 1A 45 33 FF 48 8B 01 4C 8B F1");

		return Hooks::WriteJump(target, &HookedNameD3DObject, &OriginalNameD3DObject);
	};
}
