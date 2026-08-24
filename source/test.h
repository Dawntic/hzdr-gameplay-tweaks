#pragma once
#include "renderdoc_app.h"

//struct RENDERDOC_API_1_7_0;
extern RENDERDOC_API_1_7_0 *renderDocApi; //
extern std::atomic_bool g_wantCapture;

// True when the game was launched with -attach_renderdoc, i.e. the engine owns the RenderDoc attach.
bool EngineWillAttachRenderDoc();

// Grab the API from a renderdoc.dll somebody else already loaded. Never calls LoadLibrary.
bool AttachToLoadedRenderDoc();

void LoadRenderDoc();

//void TriggerCapture();
