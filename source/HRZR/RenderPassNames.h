#pragma once

namespace HRZR::RenderPassNames
{
	//
	// Decima builds a descriptive name for every D3D12 command list it opens - frame number, RenderOrder
	// bitfield, render target, queue, viewport, gfx/compute, suborder, sync - and hands it to
	// ID3D12Object::SetName. RenderDoc reads those names.
	//
	// In the shipping build the whole path is gated behind one global byte that nothing ever writes 1 to:
	// the only writer is the CRT initializer that zeroes it. Flipping it by hand turns the naming on.
	//
	bool IsAvailable();
	bool IsEnabled();
	void SetEnabled(bool Enabled);
}
