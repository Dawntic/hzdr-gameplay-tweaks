#pragma once

#include <filesystem>

namespace InternalModConfig
{
	struct GlobalSettings
	{
		GlobalSettings() = default;
		GlobalSettings(const GlobalSettings&) = delete;

		// [General]
		bool LogToFile;

		// [RenderDoc]
		struct
		{
			bool Enable;
			bool Diagnostics;
			bool BypassStreamline;
			bool RedirectEngineLoad;
			bool NameCommandLists;
			bool LogCommandListNames;
			bool EmitPassMarkers;
			bool WrapUnnamedLists;
			bool ShaderMarkers;
			bool EnginePassMarkers;
			int CaptureKey;
			std::string DllPath;
			std::string CapturePath;
		} RenderDoc;
	};

	void Initialize(const std::filesystem::path& RootDirectory);
	bool InitializeConfigurationFile();
	std::filesystem::path GetModRelativePath(const std::string_view& PartialPath);
}

extern const InternalModConfig::GlobalSettings& ModConfiguration;
