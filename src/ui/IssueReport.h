#pragma once

#include "core/Export.h"

#include <filesystem>
#include <string>
#include <vector>

class QMainWindow;
class QWidget;

/**
 * Help > Report Issue files an issue on github.com/lila-3d/issues through the telemetry Worker, with the support bundle
 * and a screenshot as private attachments. Help > My Reports lists the reports sent from this machine with their status
 * and maintainer replies, so reporters need no GitHub account.
 */
namespace lila::ui::issueReport
{
/** Status shown for a report: its triage label (Pending, Solved, On Hold, Dismissed), else Open or Closed. */
LILA_UI_API std::string statusText(const std::string& state, const std::vector<std::string>& labels);

LILA_UI_API void showDialog(QMainWindow* window);
/** Opens the report as a Crash about `crashDir`, once any modal dialog (Lila Crashed) has closed. */
LILA_UI_API void showCrash(QMainWindow* window, const std::filesystem::path& crashDir);
LILA_UI_API void showMyReports(QWidget* parent);

/** Reopens a report that was open when Lila closed, and checks sent reports for new replies a little later. */
LILA_UI_API void startup(QMainWindow* window);
} // namespace lila::ui::issueReport
