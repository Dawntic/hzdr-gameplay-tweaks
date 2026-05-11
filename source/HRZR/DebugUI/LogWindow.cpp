#include "LogWindow.h"
#include "test.h"

#include <Windows.h>

namespace HRZR::DebugUI
{
	void TriggerCapture()
	{
		if (!renderDocApi)
		{
			spdlog::warn("[RenderDoc] Cannot trigger capture - RenderDoc API not available");
			return;
		}

		// Check how many captures exist before triggering
		uint32_t capturesBefore = renderDocApi->GetNumCaptures();
		spdlog::info("[RenderDoc] Captures before trigger: {}", capturesBefore);

		// Check if we're already mid-capture
		spdlog::info("[RenderDoc] IsFrameCapturing: {}", renderDocApi->IsFrameCapturing());

		renderDocApi->TriggerCapture();

		spdlog::info("[RenderDoc] TriggerCapture() called");

		// TriggerCapture queues for the NEXT Present() call — wait a couple frames
		// then check if the count went up
		std::thread([=]()
		{
			std::this_thread::sleep_for(std::chrono::seconds(3));

			uint32_t capturesAfter = renderDocApi->GetNumCaptures();
			spdlog::info("[RenderDoc] Captures after trigger: {}", capturesAfter);

			if (capturesAfter > capturesBefore)
			{
				// Get the path RenderDoc actually wrote to
				char path[512] = {};
				uint64_t timestamp = 0;
				uint32_t pathLen = sizeof(path);
				renderDocApi->GetCapture(capturesAfter - 1, path, &pathLen, &timestamp);
				spdlog::info("[RenderDoc] Capture written to: {}", path);
			}
			else
			{
				spdlog::warn(
					"[RenderDoc] Capture count did not increase — "
					"swap chain likely not hooked. "
					"RenderDoc may have loaded after D3D device was created.");
			}
		}).detach();
	}

	void LogWindow::Render()
	{
		if (!ImGui::Begin(GetId().c_str(), &m_WindowOpen))
		{
			ImGui::End();
			return;
		}

		// Options menu
		if (ImGui::BeginPopup("Options"))
		{
			ImGui::Checkbox("Auto-scroll", &m_AutoScroll);

			if (ImGui::Button("Render Doc"))
				TriggerCapture();

			ImGui::EndPopup();
		}

		if (ImGui::Button("Options"))
			ImGui::OpenPopup("Options");

		// Main window
		ImGui::SameLine();
		if (ImGui::Button("Clear"))
			Clear();

		ImGui::SameLine();
		bool copy = ImGui::Button("Copy");

		ImGui::SameLine();
		if (m_Filter.Draw("Filter", -100.0f))
			m_LineCountFilterCache = 0;

		ImGui::Separator();
		ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

		if (copy)
			ImGui::LogToClipboard();

		std::lock_guard lock(m_Mutex);

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
		{
			const char *buf = m_Buf.begin();
			const char *bufEnd = m_Buf.end();

			const bool filterActive = m_Filter.IsActive();
			size_t clipperItemCount = m_LineOffsets.size();

			// Put filtered lines into a separate vector
			if (filterActive)
			{
				if (m_LineCountFilterCache != m_LineOffsets.size())
				{
					m_LineCountFilterCache = m_LineOffsets.size();
					m_FilteredLines.clear();

					for (int lineNum = 0; lineNum < m_LineOffsets.size(); lineNum++)
					{
						const char *lineStart = buf + m_LineOffsets[lineNum];
						const char *lineEnd = (lineNum + 1 < m_LineOffsets.size()) ? (buf + m_LineOffsets[lineNum + 1] - 1) : bufEnd;

						if (m_Filter.PassFilter(lineStart, lineEnd))
							m_FilteredLines.push_back(lineNum);
					}
				}

				clipperItemCount = m_FilteredLines.size();
			}

			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(clipperItemCount));

			while (clipper.Step())
			{
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
				{
					int lineNum = filterActive ? m_FilteredLines[i] : i;

					const char *lineStart = buf + m_LineOffsets[lineNum];
					const char *lineEnd = (lineNum + 1 < m_LineOffsets.size()) ? (buf + m_LineOffsets[lineNum + 1] - 1) : bufEnd;

					ImGui::TextUnformatted(lineStart, lineEnd);
				}
			}

			clipper.End();
		}
		ImGui::PopStyleVar();

		if (m_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
			ImGui::SetScrollHereY(1.0f);

		ImGui::EndChild();
		ImGui::End();
	}

	bool LogWindow::Close()
	{
		return !m_WindowOpen;
	}

	std::string LogWindow::GetId() const
	{
		return "Log Window";
	}

	void LogWindow::Clear()
	{
		std::lock_guard lock(m_Mutex);

		m_Buf.clear();
		m_LineOffsets.clear();
	}

	void LogWindow::LogSink::sink_it_(const spdlog::details::log_msg& Message)
	{
		spdlog::memory_buf_t buffer;
		formatter_->format(Message, buffer);

		std::lock_guard lock(m_Mutex);
		{
			int oldSize = m_Buf.size();

			// Auto clear after 1MB of text
			if (oldSize >= (1 * 1024 * 1024))
			{
				oldSize = 0;
				Clear();
			}

			m_Buf.append(buffer.begin(), buffer.end());

			if (m_LineOffsets.empty())
				m_LineOffsets.emplace_back(0);

			for (int newSize = m_Buf.size(); oldSize < newSize; oldSize++)
			{
				if (m_Buf[oldSize] == '\n')
					m_LineOffsets.emplace_back(oldSize + 1);
			}
		}
	}

	void LogWindow::LogSink::flush_()
	{
		// Log is always flushed on write
	}
}
