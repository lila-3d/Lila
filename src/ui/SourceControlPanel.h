#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QString>
#include <QWidget>

#include <filesystem>
#include <functional>

class QLabel;
class QTreeWidget;

namespace lila::ui
{
/**
 * Pending changelists and their files for the workspace's provider (Perforce or git), with checkout, revert, submit
 * and sync. Every action runs a sc* command, so it echoes and can be scripted.
 */
class LILA_UI_API SourceControlPanel : public QWidget
{
    Q_OBJECT

public:
    explicit SourceControlPanel(QWidget* parent = nullptr);

    void refresh();

private:
    void run(const char* command, const nlohmann::json& call);
    std::vector<std::string> selectedFiles() const;
    std::string selectedChange() const;
    void submit();

    QLabel* mProvider = nullptr;
    QTreeWidget* mTree = nullptr;
};

/** Status-bar text for the open scene ("checked out", "read-only", ...), refreshed after open/save. */
LILA_UI_API void sceneSourceStatus(const std::filesystem::path& scene, std::function<void(QString)> done);
} // namespace lila::ui
