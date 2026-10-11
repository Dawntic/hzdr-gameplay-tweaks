#include "ShaderScopes.h"
#include "RenderDoc.h"
#include "../ModConfiguration.h"

#include <d3d12.h>
#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace ShaderScopes
{
	//
	// Vtable slot lookup
	//
	// Calls the interface method through a pointer-to-member on a fake object whose vtable entries each
	// return their own index. Keeps the slot numbers tied to d3d12.h instead of hand counting 60 methods.
	//
	template<size_t Index>
	static uintptr_t ReturnSlotIndex()
	{
		return Index;
	}

	template<size_t... Indices>
	static auto MakeSlotTable(std::index_sequence<Indices...>)
	{
		return std::array<const void *, sizeof...(Indices)> { reinterpret_cast<const void *>(&ReturnSlotIndex<Indices>)... };
	}

	template<typename Interface, typename Method>
	static uint32_t SlotOf(Method Interface::*Function)
	{
		static const auto table = MakeSlotTable(std::make_index_sequence<128>());

		struct
		{
			const void *const *Vtable;
		} fake { table.data() };

		using Probe = uintptr_t (Interface::*)();
		const auto probe = reinterpret_cast<Probe>(Function);

		return static_cast<uint32_t>((reinterpret_cast<Interface *>(&fake)->*probe)());
	}

	//
	// Entry point names
	//
	// A DXIL container's PSV0 part starts with the runtime info (size-prefixed), then the resource bindings,
	// then a string table. Runtime info version 3 and later stores the entry function name as an offset into
	// that table, at byte 48.
	//
	static constexpr uint32_t Psv0FourCC = 'P' | ('S' << 8) | ('V' << 16) | ('0' << 24);

	static std::string_view FindContainerPart(const uint8_t *Data, size_t Size, uint32_t FourCC)
	{
		if (!Data || Size < 32 || memcmp(Data, "DXBC", 4) != 0)
			return {};

		const auto partCount = *reinterpret_cast<const uint32_t *>(Data + 28);

		if (32 + static_cast<size_t>(partCount) * 4 > Size)
			return {};

		for (uint32_t i = 0; i < partCount; i++)
		{
			const auto offset = *reinterpret_cast<const uint32_t *>(Data + 32 + i * 4);

			if (static_cast<size_t>(offset) + 8 > Size)
				return {};

			const auto partSize = *reinterpret_cast<const uint32_t *>(Data + offset + 4);

			if (*reinterpret_cast<const uint32_t *>(Data + offset) == FourCC && static_cast<size_t>(offset) + 8 + partSize <= Size)
				return { reinterpret_cast<const char *>(Data + offset + 8), partSize };
		}

		return {};
	}

	static std::string EntryPointOf(const D3D12_SHADER_BYTECODE& Bytecode)
	{
		const auto psv = FindContainerPart(
			static_cast<const uint8_t *>(Bytecode.pShaderBytecode),
			Bytecode.BytecodeLength,
			Psv0FourCC);

		auto readU32 = [&](size_t Offset, uint32_t& Out)
		{
			if (Offset + 4 > psv.size())
				return false;

			memcpy(&Out, psv.data() + Offset, 4);
			return true;
		};

		uint32_t infoSize = 0;

		if (!readU32(0, infoSize) || infoSize < 52)
			return {};

		uint32_t entryOffset = 0;
		uint32_t resourceCount = 0;
		size_t cursor = 4 + infoSize;

		if (!readU32(4 + 48, entryOffset) || !readU32(cursor, resourceCount))
			return {};

		cursor += 4;

		if (resourceCount)
		{
			uint32_t bindSize = 0;

			if (!readU32(cursor, bindSize))
				return {};

			cursor += 4 + static_cast<size_t>(bindSize) * resourceCount;
		}

		uint32_t stringTableSize = 0;

		if (!readU32(cursor, stringTableSize) || cursor + 4 + stringTableSize > psv.size() || entryOffset >= stringTableSize)
			return {};

		const std::string_view table(psv.data() + cursor + 4, stringTableSize);
		const auto name = table.substr(entryOffset);

		return std::string(name.substr(0, name.find('\0')));
	}

	static void AppendStage(std::string& Label, const D3D12_SHADER_BYTECODE& Bytecode)
	{
		const auto name = EntryPointOf(Bytecode);

		if (name.empty())
			return;

		if (!Label.empty())
			Label += " | ";

		Label += name;
	}

	// The container header's 16 byte digest identifies a shader no matter where its bytes were copied to
	static void AppendShaderKey(std::string& Key, const D3D12_SHADER_BYTECODE& Bytecode)
	{
		const auto data = static_cast<const char *>(Bytecode.pShaderBytecode);

		if (data && Bytecode.BytecodeLength >= 20 && memcmp(data, "DXBC", 4) == 0)
			Key.append(data + 4, 16);
	}

	// Pipeline stream subobjects are laid out like d3dx12's CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT: the type,
	// then the payload, the whole thing padded to pointer alignment.
	template<typename T>
	struct alignas(void *) StreamSubobject
	{
		D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type;
		T Inner;
	};

	template<typename T>
	static constexpr std::pair<size_t, size_t> SubobjectLayout()
	{
		return { sizeof(StreamSubobject<T>), offsetof(StreamSubobject<T>, Inner) };
	}

	// { size, payload offset }, or { 0, 0 } for a type this doesn't know how to step over
	static std::pair<size_t, size_t> SubobjectLayout(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type)
	{
		switch (Type)
		{
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: return SubobjectLayout<ID3D12RootSignature *>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: return SubobjectLayout<D3D12_SHADER_BYTECODE>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: return SubobjectLayout<D3D12_STREAM_OUTPUT_DESC>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: return SubobjectLayout<D3D12_BLEND_DESC>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: return SubobjectLayout<UINT>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: return SubobjectLayout<D3D12_RASTERIZER_DESC>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: return SubobjectLayout<D3D12_DEPTH_STENCIL_DESC>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: return SubobjectLayout<D3D12_INPUT_LAYOUT_DESC>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: return SubobjectLayout<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: return SubobjectLayout<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: return SubobjectLayout<D3D12_RT_FORMAT_ARRAY>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: return SubobjectLayout<DXGI_FORMAT>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: return SubobjectLayout<DXGI_SAMPLE_DESC>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: return SubobjectLayout<UINT>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: return SubobjectLayout<D3D12_CACHED_PIPELINE_STATE>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: return SubobjectLayout<D3D12_PIPELINE_STATE_FLAGS>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: return SubobjectLayout<D3D12_DEPTH_STENCIL_DESC1>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: return SubobjectLayout<D3D12_VIEW_INSTANCING_DESC>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: return SubobjectLayout<D3D12_DEPTH_STENCIL_DESC2>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: return SubobjectLayout<D3D12_RASTERIZER_DESC1>();
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: return SubobjectLayout<D3D12_RASTERIZER_DESC2>();
		}

		return { 0, 0 };
	}

	// Calls Visit(bytecode) for each shader stage in the stream, in pipeline order rather than whatever order
	// the stream happens to list them in
	static void ForEachStreamStage(const D3D12_PIPELINE_STATE_STREAM_DESC *Desc, auto&& Visit)
	{
		if (!Desc || !Desc->pPipelineStateSubobjectStream)
			return;

		constexpr D3D12_PIPELINE_STATE_SUBOBJECT_TYPE stageOrder[] = {
			D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS,
			D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS,
			D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,
		};

		const D3D12_SHADER_BYTECODE *stages[D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID] = {};

		auto cursor = static_cast<const uint8_t *>(Desc->pPipelineStateSubobjectStream);
		const auto end = cursor + Desc->SizeInBytes;

		while (cursor + sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE) <= end)
		{
			const auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(cursor);
			const auto [size, payload] = SubobjectLayout(type);

			if (size == 0 || cursor + size > end)
				break;

			if (std::ranges::find(stageOrder, type) != std::end(stageOrder))
				stages[type] = reinterpret_cast<const D3D12_SHADER_BYTECODE *>(cursor + payload);

			cursor += size;
		}

		for (const auto stage : stageOrder)
		{
			if (stages[stage])
				Visit(*stages[stage]);
		}
	}

	static std::string LabelFromStream(const D3D12_PIPELINE_STATE_STREAM_DESC *Desc)
	{
		std::string label;
		ForEachStreamStage(Desc, [&](const D3D12_SHADER_BYTECODE& Stage) { AppendStage(label, Stage); });

		return label;
	}

	static std::string KeyFromStream(const D3D12_PIPELINE_STATE_STREAM_DESC *Desc)
	{
		std::string key;
		ForEachStreamStage(Desc, [&](const D3D12_SHADER_BYTECODE& Stage) { AppendShaderKey(key, Stage); });

		return key;
	}

	//
	// State
	//
	using CreateGraphicsPipelineStateFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, REFIID, void **);
	using CreateComputePipelineStateFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, REFIID, void **);
	using CreatePipelineStateFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device2 *, const D3D12_PIPELINE_STATE_STREAM_DESC *, REFIID, void **);
	using CreateCommandListFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device *, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator *, ID3D12PipelineState *, REFIID, void **);
	using CreateCommandList1Fn = HRESULT(STDMETHODCALLTYPE *)(ID3D12Device4 *, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void **);

	using CloseFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *);
	using ResetFn = HRESULT(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *, ID3D12PipelineState *);
	using SetPipelineStateFn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
	using SetPipelineState1Fn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList4 *, ID3D12StateObject *);

	struct DeviceVtable
	{
		CreateGraphicsPipelineStateFn CreateGraphicsPipelineState = nullptr;
		CreateComputePipelineStateFn CreateComputePipelineState = nullptr;
		CreatePipelineStateFn CreatePipelineState = nullptr;
		CreateCommandListFn CreateCommandList = nullptr;
		CreateCommandList1Fn CreateCommandList1 = nullptr;
	};

	struct ListVtable
	{
		CloseFn Close = nullptr;
		ResetFn Reset = nullptr;
		SetPipelineStateFn SetPipelineState = nullptr;
		SetPipelineState1Fn SetPipelineState1 = nullptr;
	};

	static decltype(&D3D12CreateDevice) g_RealCreateDevice = nullptr;

	// Several vtables can be live at once (RenderDoc's wrappers, plus the real runtime's if anything ever
	// slips past RenderDoc), so originals are kept per vtable rather than per slot
	static std::shared_mutex g_VtableMutex;
	static std::unordered_map<void **, DeviceVtable> g_DeviceVtables;
	static std::unordered_map<void **, ListVtable> g_ListVtables;

	// What a pipeline state is called: an optional section (the engine's render pass) and the scope inside it
	struct PipelineInfo
	{
		std::string Section;
		std::string Label;
	};

	static std::shared_mutex g_NameMutex;
	static std::unordered_map<const void *, PipelineInfo> g_PipelineNames;

	// Engine names for pipelines being created right now on this thread, and for every shader combination the
	// engine has asked for, in case the D3D12 call happens on another thread
	struct EngineNames
	{
		std::string Pass;
		std::string Name;
	};

	static thread_local std::optional<EngineNames> t_EngineContext;
	static std::mutex g_EngineMutex;
	static std::unordered_map<std::string, EngineNames> g_EngineByShaders;

	struct ListScopes
	{
		std::string Section;
		bool SectionOpen = false;
		bool ScopeOpen = false;
	};

	static std::mutex g_ScopeMutex;
	static std::unordered_map<const void *, ListScopes> g_ListScopes;

	static void **VtableOf(const void *Object)
	{
		return *reinterpret_cast<void **const *>(Object);
	}

	template<typename Map>
	static auto OriginalsFor(Map& Vtables, const void *Object)
	{
		std::shared_lock lock(g_VtableMutex);

		const auto entry = Vtables.find(VtableOf(Object));
		return entry != Vtables.end() ? entry->second : typename Map::mapped_type {};
	}

	static std::optional<EngineNames> EngineNamesFor(const std::string& ShaderKey, bool& FromThisThread)
	{
		FromThisThread = t_EngineContext.has_value();

		if (FromThisThread)
			return t_EngineContext;

		if (ShaderKey.empty())
			return std::nullopt;

		std::lock_guard lock(g_EngineMutex);

		const auto entry = g_EngineByShaders.find(ShaderKey);
		return entry != g_EngineByShaders.end() ? std::optional(entry->second) : std::nullopt;
	}

	static void RememberPipeline(void **Pipeline, std::string Label, const std::string& ShaderKey)
	{
		if (!Pipeline || !*Pipeline)
			return;

		PipelineInfo info { {}, std::move(Label) };
		bool fromThisThread = false;

		if (const auto engine = EngineNamesFor(ShaderKey, fromThisThread))
		{
			info.Section = engine->Pass;

			// The engine's own pipeline name leads, with the entry points kept for reference
			if (!engine->Name.empty())
				info.Label = info.Label.empty() ? engine->Name : std::format("{} [{}]", engine->Name, info.Label);
		}

		if (info.Section.empty() && info.Label.empty())
			return;

		std::unique_lock lock(g_NameMutex);
		g_PipelineNames.insert_or_assign(*Pipeline, std::move(info));
	}

	static PipelineInfo InfoOf(const void *Pipeline)
	{
		if (!Pipeline)
			return {};

		std::shared_lock lock(g_NameMutex);

		const auto name = g_PipelineNames.find(Pipeline);
		return name != g_PipelineNames.end() ? name->second : PipelineInfo {};
	}

	// Moves the list's open scopes to match Info: the per-pipeline scope always closes, the section only when
	// the pass changes. KeepSection leaves whatever section is open alone. Recording calls happen outside
	// the lock.
	static void SwitchScope(ID3D12GraphicsCommandList *List, const PipelineInfo& Info, bool KeepSection = false)
	{
		uint32_t ends = 0;
		bool beginSection = false;
		bool beginScope = false;

		{
			std::lock_guard lock(g_ScopeMutex);
			auto& scopes = g_ListScopes[List];

			if (scopes.ScopeOpen)
			{
				scopes.ScopeOpen = false;
				ends++;
			}

			if (!KeepSection)
			{
				if (scopes.SectionOpen && scopes.Section != Info.Section)
				{
					scopes.SectionOpen = false;
					ends++;
				}

				if (!Info.Section.empty() && !scopes.SectionOpen)
				{
					scopes.Section = Info.Section;
					scopes.SectionOpen = beginSection = true;
				}
			}

			scopes.ScopeOpen = beginScope = !Info.Label.empty();

			if (!scopes.SectionOpen && !scopes.ScopeOpen)
				g_ListScopes.erase(List);
		}

		for (uint32_t i = 0; i < ends; i++)
			List->EndEvent();

		// Metadata 1 is a plain ANSI string, which RenderDoc decodes
		if (beginSection)
			List->BeginEvent(1, Info.Section.c_str(), static_cast<UINT>(Info.Section.size() + 1));

		if (beginScope)
			List->BeginEvent(1, Info.Label.c_str(), static_cast<UINT>(Info.Label.size() + 1));
	}

	//
	// Command list hooks
	//
	static HRESULT STDMETHODCALLTYPE HookedClose(ID3D12GraphicsCommandList *This)
	{
		const auto original = OriginalsFor(g_ListVtables, This).Close;

		// Must land before Close, while the list still records
		SwitchScope(This, {});

		return original(This);
	}

	static HRESULT STDMETHODCALLTYPE HookedReset(ID3D12GraphicsCommandList *This, ID3D12CommandAllocator *Allocator, ID3D12PipelineState *InitialState)
	{
		const auto original = OriginalsFor(g_ListVtables, This).Reset;

		// A list must be closed to be reset, so anything still tracked was never ended. Forget it rather
		// than recording EndEvent on a closed list.
		{
			std::lock_guard lock(g_ScopeMutex);
			g_ListScopes.erase(This);
		}

		const auto result = original(This, Allocator, InitialState);

		// Recording only begins once Reset has returned
		if (SUCCEEDED(result) && InitialState)
			SwitchScope(This, InfoOf(InitialState));

		return result;
	}

	static void STDMETHODCALLTYPE HookedSetPipelineState(ID3D12GraphicsCommandList *This, ID3D12PipelineState *Pipeline)
	{
		const auto original = OriginalsFor(g_ListVtables, This).SetPipelineState;

		// Opened before the call so the SetPipelineState itself is the first event inside the scope
		SwitchScope(This, InfoOf(Pipeline));
		original(This, Pipeline);
	}

	static void STDMETHODCALLTYPE HookedSetPipelineState1(ID3D12GraphicsCommandList4 *This, ID3D12StateObject *StateObject)
	{
		const auto original = OriginalsFor(g_ListVtables, This).SetPipelineState1;

		// Raytracing state objects aren't parsed for export names yet. They stay inside whatever pass is
		// already open, since nothing says which pass they belong to.
		SwitchScope(This, { {}, StateObject ? "Raytracing" : "" }, true);
		original(This, StateObject);
	}

	static void PatchCommandList(void **Object)
	{
		if (!Object || !*Object)
			return;

		ID3D12GraphicsCommandList *list = nullptr;

		if (FAILED(static_cast<IUnknown *>(*Object)->QueryInterface(IID_PPV_ARGS(&list))) || !list)
			return;

		const auto vtable = VtableOf(list);
		ID3D12GraphicsCommandList4 *list4 = nullptr;

		// SetPipelineState1 only exists on lists that implement the newer interface through the same vtable
		const bool hasList4 = SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list4))) && list4 && VtableOf(list4) == vtable;

		if (list4)
			list4->Release();

		list->Release();

		const auto closeSlot = SlotOf(&ID3D12GraphicsCommandList::Close);
		const auto resetSlot = SlotOf(&ID3D12GraphicsCommandList::Reset);
		const auto setPipelineSlot = SlotOf(&ID3D12GraphicsCommandList::SetPipelineState);
		const auto setPipeline1Slot = SlotOf(&ID3D12GraphicsCommandList4::SetPipelineState1);

		{
			std::unique_lock lock(g_VtableMutex);

			if (g_ListVtables.contains(vtable))
				return;

			// Originals go in before the patch, so a concurrent call on a sibling list always resolves
			g_ListVtables.emplace(
				vtable,
				ListVtable {
					reinterpret_cast<CloseFn>(vtable[closeSlot]),
					reinterpret_cast<ResetFn>(vtable[resetSlot]),
					reinterpret_cast<SetPipelineStateFn>(vtable[setPipelineSlot]),
					hasList4 ? reinterpret_cast<SetPipelineState1Fn>(vtable[setPipeline1Slot]) : nullptr,
				});
		}

		const auto table = reinterpret_cast<uintptr_t>(vtable);

		Hooks::WriteVirtualFunction(table, closeSlot, &HookedClose);
		Hooks::WriteVirtualFunction(table, resetSlot, &HookedReset);
		Hooks::WriteVirtualFunction(table, setPipelineSlot, &HookedSetPipelineState);

		if (hasList4)
			Hooks::WriteVirtualFunction(table, setPipeline1Slot, &HookedSetPipelineState1);

	}

	//
	// Device hooks
	//
	static HRESULT STDMETHODCALLTYPE HookedCreateGraphicsPipelineState(ID3D12Device *This, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *Desc, REFIID Riid, void **Pipeline)
	{
		const auto result = OriginalsFor(g_DeviceVtables, This).CreateGraphicsPipelineState(This, Desc, Riid, Pipeline);

		if (SUCCEEDED(result) && Desc)
		{
			std::string label;
			std::string key;

			for (const auto stage : { &Desc->VS, &Desc->HS, &Desc->DS, &Desc->GS, &Desc->PS })
			{
				AppendStage(label, *stage);
				AppendShaderKey(key, *stage);
			}

			RememberPipeline(Pipeline, std::move(label), key);
		}

		return result;
	}

	static HRESULT STDMETHODCALLTYPE HookedCreateComputePipelineState(ID3D12Device *This, const D3D12_COMPUTE_PIPELINE_STATE_DESC *Desc, REFIID Riid, void **Pipeline)
	{
		const auto result = OriginalsFor(g_DeviceVtables, This).CreateComputePipelineState(This, Desc, Riid, Pipeline);

		if (SUCCEEDED(result) && Desc)
		{
			std::string key;
			AppendShaderKey(key, Desc->CS);

			RememberPipeline(Pipeline, EntryPointOf(Desc->CS), key);
		}

		return result;
	}

	static HRESULT STDMETHODCALLTYPE HookedCreatePipelineState(ID3D12Device2 *This, const D3D12_PIPELINE_STATE_STREAM_DESC *Desc, REFIID Riid, void **Pipeline)
	{
		const auto result = OriginalsFor(g_DeviceVtables, This).CreatePipelineState(This, Desc, Riid, Pipeline);

		if (SUCCEEDED(result))
			RememberPipeline(Pipeline, LabelFromStream(Desc), KeyFromStream(Desc));

		return result;
	}

	static HRESULT STDMETHODCALLTYPE HookedCreateCommandList(
		ID3D12Device *This,
		UINT NodeMask,
		D3D12_COMMAND_LIST_TYPE Type,
		ID3D12CommandAllocator *Allocator,
		ID3D12PipelineState *InitialState,
		REFIID Riid,
		void **List)
	{
		const auto result = OriginalsFor(g_DeviceVtables, This).CreateCommandList(This, NodeMask, Type, Allocator, InitialState, Riid, List);

		if (SUCCEEDED(result))
		{
			PatchCommandList(List);

			// Created lists start out recording, with InitialState already bound
			ID3D12GraphicsCommandList *list = nullptr;

			if (InitialState && List && *List && SUCCEEDED(static_cast<IUnknown *>(*List)->QueryInterface(IID_PPV_ARGS(&list))))
			{
				SwitchScope(list, InfoOf(InitialState));
				list->Release();
			}
		}

		return result;
	}

	static HRESULT STDMETHODCALLTYPE HookedCreateCommandList1(
		ID3D12Device4 *This,
		UINT NodeMask,
		D3D12_COMMAND_LIST_TYPE Type,
		D3D12_COMMAND_LIST_FLAGS Flags,
		REFIID Riid,
		void **List)
	{
		const auto result = OriginalsFor(g_DeviceVtables, This).CreateCommandList1(This, NodeMask, Type, Flags, Riid, List);

		if (SUCCEEDED(result))
			PatchCommandList(List);

		return result;
	}

	static void PatchDevice(ID3D12Device *Device)
	{
		const auto vtable = VtableOf(Device);

		// Newer methods only when the device reaches them through this same vtable
		auto implements = [&](REFIID Riid)
		{
			void *object = nullptr;

			if (FAILED(Device->QueryInterface(Riid, &object)) || !object)
				return false;

			const bool same = VtableOf(object) == vtable;
			static_cast<IUnknown *>(object)->Release();

			return same;
		};

		const bool hasDevice2 = implements(__uuidof(ID3D12Device2));
		const bool hasDevice4 = implements(__uuidof(ID3D12Device4));

		const auto graphicsSlot = SlotOf(&ID3D12Device::CreateGraphicsPipelineState);
		const auto computeSlot = SlotOf(&ID3D12Device::CreateComputePipelineState);
		const auto streamSlot = SlotOf(&ID3D12Device2::CreatePipelineState);
		const auto listSlot = SlotOf(&ID3D12Device::CreateCommandList);
		const auto list1Slot = SlotOf(&ID3D12Device4::CreateCommandList1);

		{
			std::unique_lock lock(g_VtableMutex);

			if (g_DeviceVtables.contains(vtable))
				return;

			g_DeviceVtables.emplace(
				vtable,
				DeviceVtable {
					reinterpret_cast<CreateGraphicsPipelineStateFn>(vtable[graphicsSlot]),
					reinterpret_cast<CreateComputePipelineStateFn>(vtable[computeSlot]),
					hasDevice2 ? reinterpret_cast<CreatePipelineStateFn>(vtable[streamSlot]) : nullptr,
					reinterpret_cast<CreateCommandListFn>(vtable[listSlot]),
					hasDevice4 ? reinterpret_cast<CreateCommandList1Fn>(vtable[list1Slot]) : nullptr,
				});
		}

		const auto table = reinterpret_cast<uintptr_t>(vtable);

		Hooks::WriteVirtualFunction(table, graphicsSlot, &HookedCreateGraphicsPipelineState);
		Hooks::WriteVirtualFunction(table, computeSlot, &HookedCreateComputePipelineState);
		Hooks::WriteVirtualFunction(table, listSlot, &HookedCreateCommandList);

		if (hasDevice2)
			Hooks::WriteVirtualFunction(table, streamSlot, &HookedCreatePipelineState);

		if (hasDevice4)
			Hooks::WriteVirtualFunction(table, list1Slot, &HookedCreateCommandList1);

	}

	static HRESULT WINAPI HookedD3D12CreateDevice(IUnknown *Adapter, D3D_FEATURE_LEVEL MinimumFeatureLevel, REFIID Riid, void **Device)
	{
		const auto result = g_RealCreateDevice(Adapter, MinimumFeatureLevel, Riid, Device);

		// A null output is a capability probe, there's no device to hook
		if (SUCCEEDED(result) && Device && *Device)
		{
			ID3D12Device *device = nullptr;

			if (SUCCEEDED(static_cast<IUnknown *>(*Device)->QueryInterface(IID_PPV_ARGS(&device))) && device)
			{
				PatchDevice(device);
				device->Release();
			}
		}

		return result;
	}

	bool IsEnabled()
	{
		return ModConfiguration.RenderDoc.ShaderMarkers;
	}

	FARPROC WrapCreateDevice(FARPROC CreateDevice)
	{
		if (!CreateDevice)
			return nullptr;

		g_RealCreateDevice = reinterpret_cast<decltype(&D3D12CreateDevice)>(CreateDevice);
		return reinterpret_cast<FARPROC>(&HookedD3D12CreateDevice);
	}

	void BeginEnginePipeline(const char *Pass, const char *Name, const D3D12_PIPELINE_STATE_STREAM_DESC *Stream)
	{
		EngineNames names { Pass ? Pass : "", Name ? Name : "" };

		if (const auto key = KeyFromStream(Stream); !key.empty())
		{
			std::lock_guard lock(g_EngineMutex);
			g_EngineByShaders.insert_or_assign(key, names);
		}

		t_EngineContext = std::move(names);
	}

	void EndEnginePipeline()
	{
		t_EngineContext.reset();
	}
}
