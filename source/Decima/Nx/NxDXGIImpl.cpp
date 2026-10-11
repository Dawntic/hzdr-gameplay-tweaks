#include "../../ModConfiguration.h"
#include "NxD3DImpl.h"
#include "NxDXGIImpl.h"

#include "../../RenderDoc/RenderDoc.h"
#include "../../RenderDoc/RenderDocDiag.h"
#include "../RenderPassNames.h"
#include <Windows.h>

namespace HRZR
{

	bool (*OriginalPresent)(NxDXGIImpl *DXGIImpl, void *a2);

	bool HookedPresent(NxDXGIImpl *Thisptr, void *a2)
	{
		// The first present is the earliest point where the device, queue and swapchain all exist, so it's
		// the most informative snapshot of who hooked what. It's also the first point at which the engine's
		// own -attach_renderdoc load has definitely happened.
		const static bool renderDocReady = [&]()
		{
			if (!RenderDoc::Api)
				RenderDoc::AttachToLoaded();

			// Must happen here, not at mod init: the executable's CRT initializers zero this flag and they
			// run after our DllMain.
			if (ModConfiguration.RenderDoc.NameCommandLists)
				RenderPassNames::SetEnabled(true);

			if (ModConfiguration.RenderDoc.Diagnostics)
				RenderDocDiag::Report("first Present");

			return RenderDoc::Api != nullptr;
		}();

		return OriginalPresent(Thisptr, a2);
	}

	DECLARE_HOOK_TRANSACTION(NxDXGIImpl)
	{
		// Present vfunc is 10th index in NxDXGIImpl's virtual table
		const auto vtableEntryNxDXGIImpl = Offsets::Signature(
											   "48 8D 0D ? ? ? ? 66 89 68 08 48 89 08 40 88 68 0A 48 89 68 0C 48 89 68 18 48 89 68 20")
											   .AsRipRelative(7)
											   .AsAdjusted(sizeof(void *) * 10)
											   .ToPointer<uintptr_t>();

		Hooks::WriteJump(*vtableEntryNxDXGIImpl, &HookedPresent, &OriginalPresent);
	};
}
