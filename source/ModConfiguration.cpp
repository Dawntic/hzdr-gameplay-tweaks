#include <spdlog/sinks/basic_file_sink.h>
#include <toml++/toml.h>
#include "ModConfiguration.h"

namespace InternalModConfig
{
	InternalModConfig::GlobalSettings& GetModifiableSettings()
	{
		static GlobalSettings configuration;
		return configuration;
	}

	std::filesystem::path& GetModifiableRootDirectory()
	{
		static std::filesystem::path rootDirectory;
		return rootDirectory;
	}
}

const InternalModConfig::GlobalSettings& ModConfiguration = InternalModConfig::GetModifiableSettings();

namespace InternalModConfig
{
	void ParseSettings(GlobalSettings& s, const toml::table& Table);
	void PostProcessSettings(GlobalSettings& s);

	void Initialize(const std::filesystem::path& RootDirectory)
	{
		GetModifiableRootDirectory() = RootDirectory;

		if (!InitializeConfigurationFile())
			throw std::runtime_error("Failed to load configuration file.");

		// Main logger. Only writes anywhere when LogToFile is set.
		if (GetModifiableSettings().LogToFile)
		{
			auto logFileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(GetModRelativePath("mod_log.txt").string(), true);

			auto logger = std::make_shared<spdlog::logger>("main_logger", std::move(logFileSink));
			logger->set_level(spdlog::level::debug);
			logger->flush_on(spdlog::level::debug);
			logger->set_pattern("[%H:%M:%S] [%l] %v");

			spdlog::set_default_logger(std::move(logger));
		}

		if (!Offsets::Initialize())
			throw std::runtime_error("Failed to initialize offsets. The game likely updated and is no longer supported.");

		if (!Hooks::Initialize())
			throw std::runtime_error("Failed to initialize hooks.");
	}

	bool InitializeConfigurationFile()
	{
		auto tryParseTomlFile = [](GlobalSettings& Settings, const std::filesystem::path& Path)
		{
			toml::table table;

			try
			{
				table = toml::parse_file(Path.string());
			}
			catch (const toml::parse_error&)
			{
				return false;
			}

			ParseSettings(Settings, table);
			return true;
		};

		// mod_config.ini takes precedence
		if (!tryParseTomlFile(GetModifiableSettings(), GetModRelativePath("mod_config.ini")))
			return false;

		// Then check for mod_*.ini files in the game directory
		for (const auto& entry : std::filesystem::directory_iterator(GetModRelativePath("")))
		{
			if (!entry.is_regular_file() || entry.path().extension() != ".ini")
				continue;

			if (!entry.path().filename().string().starts_with("mod_") || entry.path().filename().string() == "mod_config.ini")
				continue;

			if (!tryParseTomlFile(GetModifiableSettings(), entry.path()))
				return false;
		}

		PostProcessSettings(GetModifiableSettings());
		return true;
	}

	std::filesystem::path GetModRelativePath(const std::string_view& PartialPath)
	{
		auto path = GetModifiableRootDirectory();
		path.append(PartialPath);

		return path;
	}

	void ParseSettings(GlobalSettings& o, const toml::table& Table)
	{
#define PARSE_TOML_RENDERDOC(obj, x) o.RenderDoc.x = (*obj)[#x].value_or(decltype(o.RenderDoc.x) {})

		// [General]
		if (auto general = Table["General"].as_table())
			o.LogToFile = (*general)["LogToFile"].value_or(false);

		// [RenderDoc]
		if (auto renderDoc = Table["RenderDoc"].as_table())
		{
			PARSE_TOML_RENDERDOC(renderDoc, Enable);
			PARSE_TOML_RENDERDOC(renderDoc, Diagnostics);
			PARSE_TOML_RENDERDOC(renderDoc, BypassStreamline);
			PARSE_TOML_RENDERDOC(renderDoc, RedirectEngineLoad);
			PARSE_TOML_RENDERDOC(renderDoc, NameCommandLists);
			PARSE_TOML_RENDERDOC(renderDoc, LogCommandListNames);
			PARSE_TOML_RENDERDOC(renderDoc, EmitPassMarkers);
			PARSE_TOML_RENDERDOC(renderDoc, WrapUnnamedLists);
			PARSE_TOML_RENDERDOC(renderDoc, ShaderMarkers);
			PARSE_TOML_RENDERDOC(renderDoc, EnginePassMarkers);
			o.RenderDoc.CaptureKey = (*renderDoc)["CaptureKey"].value_or(-1);
			PARSE_TOML_RENDERDOC(renderDoc, DllPath);
			PARSE_TOML_RENDERDOC(renderDoc, CapturePath);
		}

#undef PARSE_TOML_RENDERDOC
	}

	void PostProcessSettings(GlobalSettings& s)
	{
		if (s.RenderDoc.DllPath.empty())
			s.RenderDoc.DllPath = "C:\\Program Files\\RenderDoc\\renderdoc.dll";
	}
}
