#pragma once

#include "ui/BakerPanel.h"
#include "ui/BakeCageView.h"

#include "bake/Arrange.h"
#include "bake/Backend.h"
#include "bake/Bake.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/BakePipeline.h"
#include "bake/Maps.h"
#include "bake/Naming.h"
#include "bake/Targets.h"
#include "core/ImageIO.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/FlagFields.h"
#include "ui/Hotkeys.h"
#include "ui/Icons.h"
#include "ui/Menus.h"
#include "ui/NodeLookup.h"
#include "ui/Outliner.h"
#include "ui/Panels.h"
#include "ui/FileDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <cmath>

namespace lila::ui::bakerPanel
{
using json = nlohmann::json;

constexpr const char* kPrefs = "baker";

enum ItemKind
{
    kGroupItem = 1,
    kLowsItem,
    kLowItem,
    kHighsItem,
    kHighItem,
    kCageItem,
    kLintItem,
};

constexpr int kKindRole = Qt::UserRole;
constexpr int kGroupRole = Qt::UserRole + 1;
constexpr int kNodeRole = Qt::UserRole + 2;
constexpr int kFileRole = Qt::UserRole + 3;
constexpr int kPresetRole = Qt::UserRole + 4;

QString qs(const std::string& s);
std::string ss(const QString& s);
std::string shortName(const std::string& path);

/** The project the panel last showed, for actions run without it (menus, shelves, hotkeys). */
std::string& currentProject();
scene::Graph& graph();
void openFolder(const QString& dir);
json runUi(const char* command, const json& call);

/** The backend bakes from the Baker run on: the preference, else the GPU when one is registered. */
std::string backendPref();

/** New projects start from the Baker's chosen target profile. */
json newProjectArgs();
std::string targetPref();
QString backendBadge(const std::string& name);

/** Groups of the project that hold a selected mesh (as low, high or cage), plus selected bakeGroup nodes. */
std::vector<std::string> groupsForSelection(const std::string& project);
/** The selected meshes and every mesh under a selected node; only the selected meshes while Shift is held. */
std::vector<std::string> selectedMeshes();

/** One group per selected mesh, each baking onto itself; one undo step. */
void groupPerMesh();
void cancelBakes();

struct LintIssue
{
    std::string group;
    std::string mesh;
    std::string message;
    std::string severity;
};

/** bakeLint's result in whatever shape: a list, or {issues|warnings|findings: [...]}, of strings or objects. */
std::vector<LintIssue> lintIssues(const json& r);
} // namespace lila::ui::bakerPanel
