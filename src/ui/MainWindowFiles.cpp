#include "ui/MainWindowDetail.h"
#include "ui/FileDialog.h"
#include "scene/Skin.h"

namespace lila::ui
{
using namespace mainWindowDetail;

bool MainWindow::confirmDiscard()
{
    if (!io::modified())
        return true;
    QString name = io::displayName() == "untitled" ? QString("untitled scene") : QString::fromStdString(io::displayName());
    bringToFront(this);
    QMessageBox box(QMessageBox::Question, "Unsaved changes", "Save changes to " + name + "?",
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setDefaultButton(QMessageBox::Save);
    box.button(QMessageBox::Save)->setText("&Save");
    box.button(QMessageBox::Discard)->setText("&Discard");
    box.button(QMessageBox::Cancel)->setText("&Cancel");
    QTimer::singleShot(0, &box, [&box] { bringToFront(&box); });
    auto choice = QMessageBox::StandardButton(box.exec());
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Save)
        return saveScene(false);
    return true;
}

bool MainWindow::saveScene(bool saveAs, const std::filesystem::path& target)
{
    std::filesystem::path path = target.empty() ? io::currentPath() : target;
    if (target.empty() && (saveAs || path.empty()))
    {
        QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
        if (!path.empty())
            start = QString::fromStdWString(path.wstring());
        QString chosen = ui::fileDialog::getSaveFileName(this, "Save Scene", start, "Lila binary (*.lila);;Lila text (*.lilaa)");
        if (chosen.isEmpty())
            return false;
        path = chosen.toStdWString();
    }
    else if (target.empty() && io::upToDate(path))
    {
        log::info("No changes to save");
        return true;
    }
    // Read-only files under source control get checked out first (or we ask, per the autoCheckout pref).
    std::string why;
    vcs::WriteAccess access = vcs::SourceControl::get().requestWrite(path, &why);
    if (access == vcs::WriteAccess::NeedsPrompt)
    {
        auto answer = QMessageBox::question(this, "Check out?", QString::fromStdString(why + "\n\nCheck it out and save?"));
        if (answer != QMessageBox::Yes)
            return false;
        access = vcs::SourceControl::get().requestWrite(path, &why, true);
    }
    if (access == vcs::WriteAccess::Denied)
    {
        QMessageBox::warning(this, "Save failed", QString::fromStdString(why));
        return false;
    }
    try
    {
        ops::execute("saveScene", {{"args", {path.generic_string()}}}, CallSource::UI);
        addRecentFile(path);
        vcs::markForAddIfNew(path);
        refreshSourceStatus();
        return true;
    }
    catch (const CommandError& e)
    {
        QMessageBox::warning(this, "Save failed", e.what());
        return false;
    }
}

void MainWindow::openScene(const std::filesystem::path& given)
{
    if (mStartupDialog)
        mStartupDialog->close();
    if (!confirmDiscard())
        return;
    std::filesystem::path path = given;
    if (path.empty())
    {
        QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
        QString chosen = ui::fileDialog::getOpenFileName(this, "Open Scene", start,
                                                      QString::fromStdString(io::translators::dialogFilter(io::translators::Op::Open, "All supported")));
        if (chosen.isEmpty())
            return;
        path = chosen.toStdWString();
    }
    try
    {
        io::translators::run(io::translators::Op::Open, path, json::object(), CallSource::UI);
        addRecentFile(path);
        refreshSourceStatus();
        mViewports->frameUnlessRestored();
    }
    catch (const CommandError& e)
    {
        QMessageBox::warning(this, "Open failed", e.what());
    }
}

void MainWindow::openRecoveredScene(const std::filesystem::path& file)
{
    if (!confirmDiscard())
        return;
    try
    {
        ops::execute("openRecoveredScene", {{"args", {QString::fromStdWString(file.generic_wstring()).toStdString()}}, {"force", true}}, CallSource::UI);
        mViewports->frameUnlessRestored();
    }
    catch (const CommandError& e)
    {
        QMessageBox::warning(this, "Open failed", e.what());
    }
}

void MainWindow::incrementAndSave()
{
    if (io::currentPath().empty())
    {
        saveScene(true);
        return;
    }
    saveScene(false, io::history::nextIncrement(io::currentPath()));
}

void MainWindow::showVersions()
{
    auto* dialog = new VersionsDialog(io::currentPath(),
                                      {[this] { return confirmDiscard(); },
                                       [this] {
                                           refreshSourceStatus();
                                           mViewports->frameUnlessRestored();
                                       }},
                                      this);
    dialog->show();
}

void MainWindow::startup(const std::vector<io::recovery::Session>& stale, bool showStartup)
{
    if (themeChooserPending())
        runFirstRunSetup(this);
    std::vector<io::recovery::Candidate> candidates = io::recovery::findRecoverable(stale);
    io::recovery::keep(candidates);
    io::recovery::forget(stale);
    io::recovery::prune(PrefStore::get().value<int>("general", "/recovery/keepDays", 30));
    std::filesystem::path crashReport = pendingCrashReport(stale, !candidates.empty());
    if (!crashReport.empty() && candidates.empty())
    {
        RecoveryDialog dialog(candidates, crashReport, this);
        dialog.exec();
        crash::acknowledge(crashReport);
    }
    if (!candidates.empty())
    {
        RecoveryDialog dialog(candidates, crashReport, this);
        dialog.exec();
        if (!crashReport.empty())
            crash::acknowledge(crashReport);
        const io::recovery::Candidate& c = dialog.selected();
        try
        {
            if (dialog.choice() == RecoveryDialog::Choice::Recover)
            {
                io::loadVersion(c.file, c.scene, !c.scene.empty(), "untitled (recovered)");
                if (!c.scene.empty())
                    addRecentFile(c.scene);
                mViewports->frameUnlessRestored();
                return;
            }
            if (dialog.choice() == RecoveryDialog::Choice::OpenSaved)
            {
                openScene(c.scene);
                return;
            }
        }
        catch (const std::exception& e)
        {
            QMessageBox::warning(this, "Recovery failed", e.what());
        }
    }
    if (!showStartup)
        return;
    const bool welcome = PrefStore::get().value<bool>("general", kShowWelcomePref, true);
    if (welcome || PrefStore::get().value<bool>("general", kShowRecentPref, true))
        showStartupDialog(welcome);
}

void MainWindow::showStartupDialog(bool welcome)
{
    if (mStartupDialog)
        mStartupDialog->close();
    StartupDialog::Hooks hooks{
        .open = [this](const std::filesystem::path& p) { openScene(p); },
        .openExample = [this](const io::examples::Example& x) { openExample(x); },
        .newScene = [] { actions::run("file.new"); },
    };
    auto* dialog = new StartupDialog(std::move(hooks), welcome, this);
    mStartupDialog = dialog;
    dialog->present();
}

void MainWindow::openExample(const io::examples::Example& example)
{
    if (!confirmDiscard())
        return;
    try
    {
        ops::execute("openExample", {{"args", {example.name}}, {"force", true}}, CallSource::UI);
        mViewports->frameUnlessRestored();
    }
    catch (const CommandError& e)
    {
        QMessageBox::warning(this, "Open failed", e.what());
    }
}

void MainWindow::importFile()
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    QString file = ui::fileDialog::getOpenFileName(this, "Import", start,
                                                QString::fromStdString(io::translators::dialogFilter(io::translators::Op::Import, "Scenes and meshes")));
    if (file.isEmpty())
        return;
    runTranslator(io::translators::Op::Import, file, {});
}

void MainWindow::convertMayaFolder()
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    QString dir = ui::fileDialog::getExistingDirectory(this, "Convert Maya Files in Folder", start);
    if (dir.isEmpty() || !confirmDiscard())
        return;
    try
    {
        json results = ops::execute("mayaConvert", {{"source", dir.toStdString()}, {"force", true}}, CallSource::UI);
        int failed = 0;
        for (const json& r : results)
            failed += r.contains("error") ? 1 : 0;
        QMessageBox::information(this, "Convert Maya Files",
                                 QString("Converted %1 of %2 file(s) to .lila next to the originals. Details are in the log.")
                                     .arg(int(results.size()) - failed)
                                     .arg(int(results.size())));
    }
    catch (const CommandError& e)
    {
        QMessageBox::warning(this, "Convert Maya Files", e.what());
    }
}

void MainWindow::exportSelection()
{
    QString start = Workspace::get().isOpen() ? QString::fromStdWString(Workspace::get().root().wstring()) : QString();
    const std::string last = PrefStore::get().value<std::string>("general", "/export/lastFormat", "");
    const QString filter = QString::fromStdString(io::translators::dialogFilter(io::translators::Op::Export, {}));
    // Qt preselects only an exact entry, so match the remembered format's label to its whole entry.
    QString selected;
    for (const io::translators::FileTranslator* t : io::translators::supporting(io::translators::Op::Export))
        if (t->id == last)
            for (const QString& entry : filter.split(";;"))
                if (entry.startsWith(QString::fromStdString(t->label + " (")))
                    selected = entry;
    QString file = ui::fileDialog::getSaveFileName(this, "Export Selection", start, filter, &selected);
    if (file.isEmpty())
        return;
    // The chosen filter entry names the format when the typed name has no extension a translator knows.
    std::string translator;
    if (const io::translators::FileTranslator* byExt = io::translators::forPath(io::translators::Op::Export, file.toStdWString()))
        PrefStore::get().set("general", "/export/lastFormat", byExt->id);
    else
        for (const io::translators::FileTranslator* t : io::translators::supporting(io::translators::Op::Export))
            if (selected.startsWith(QString::fromStdString(t->label + " (")))
            {
                translator = t->id;
                PrefStore::get().set("general", "/export/lastFormat", t->id);
                if (!t->extensions.empty())
                    file += QString::fromStdString(t->extensions.front());
            }
    runTranslator(io::translators::Op::Export, file, translator);
}

void MainWindow::sendToUnreal()
{
    // First send asks where each object goes; after that, saving the scene re-exports and reimports it.
    json selection = ops::execute("ls", {{"selection", true}});
    if (selection.empty())
    {
        QMessageBox::information(this, "Send to Unreal", "Select the objects to send first.");
        return;
    }
    json existing = ops::execute("unrealMappings", json::object());
    auto leafName = [](const std::string& path) { return path.substr(path.rfind('|') + 1); };
    auto meshesUnder = [](const std::string& path) {
        scene::Graph& g = scene::Graph::current();
        std::vector<scene::NodeId> out;
        const scene::Node* n = g.find(path);
        if (!n)
            return out;
        std::vector<scene::NodeId> ids = g.descendants(n->id());
        ids.push_back(n->id());
        for (scene::NodeId id : ids)
            if (const scene::Node* d = g.node(id); d && d->type().isA("mesh") && !d->type().isA("lattice"))
                out.push_back(id);
        return out;
    };
    json send = json::array();
    std::vector<std::string> meshless;
    for (const json& name : selection)
    {
        const std::string node = name.get<std::string>();
        const std::vector<scene::NodeId> meshes = meshesUnder(node);
        scene::Graph& g = scene::Graph::current();
        const bool skinned = std::ranges::any_of(meshes, [&g](scene::NodeId id) { return scene::skin::find(g, id) != scene::kNoNode; });
        // A static mapping of a skinned mesh is asked again, so it can go to a skeletal mesh asset.
        bool mapped = false;
        for (const json& m : existing)
            mapped = mapped || (m.value("node", "") == node && !(skinned && m.value("kind", "static") == "static"));
        if (mapped)
        {
            send.push_back(name);
            continue;
        }
        if (meshes.empty())
        {
            meshless.push_back(node);
            continue;
        }
        bool ok = false;
        QString path = QInputDialog::getText(this, "Send to Unreal", QString::fromStdString("Content path for " + node), QLineEdit::Normal,
                                             QString::fromStdString((skinned ? "/Game/Characters/SK_" : "/Game/Meshes/SM_") + leafName(node)), &ok);
        if (!ok || path.isEmpty())
            return;
        try
        {
            ops::execute("unrealMap", {{"args", {name}}, {"contentPath", path.toStdString()}, {"kind", skinned ? "skeletal" : "static"}}, CallSource::UI);
            send.push_back(name);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    }
    for (const std::string& node : meshless)
        log::warn("Send to Unreal: {} has no mesh under it, skipped (a skeleton goes with the skinned meshes)", node);
    if (send.empty())
    {
        if (!meshless.empty())
            QMessageBox::information(this, "Send to Unreal", "Nothing to send: select meshes or the group holding them. A skeleton goes with its skinned meshes.");
        return;
    }
    run("unrealSync", {{"args", send}});
}

void MainWindow::refreshSourceStatus()
{
    QPointer<QLabel> label = mSourceLabel;
    sceneSourceStatus(io::currentPath(), [label](QString text) {
        if (label)
            label->setText(text);
    });
}

void MainWindow::addRecentFile(const std::filesystem::path& path)
{
    json recent = PrefStore::get().value("general", "/recentFiles", json::array());
    if (!recent.is_array())
        recent = json::array();
    std::string s = path.generic_string();
    recent.erase(std::remove(recent.begin(), recent.end(), json(s)), recent.end());
    recent.insert(recent.begin(), s);
    const size_t keep = size_t(std::max(1.0, PrefStore::get().value<double>("general", "/recentFilesMax", 20.0)));
    if (recent.size() > keep)
        recent.erase(recent.begin() + ptrdiff_t(keep), recent.end());
    PrefStore::get().set("general", "/recentFiles", recent);
}

void MainWindow::refreshTitle()
{
    auto& ws = Workspace::get();
    QString scene = QString::fromStdString(io::displayName());
    QString title = scene + (io::modified() ? "*" : "");
    if (ws.isOpen())
    {
        std::filesystem::path root = ws.root();
        if (root.has_parent_path() && root.parent_path() != root.root_path())
            title += " - " + QString::fromStdWString(root.parent_path().filename().wstring()) + "\\" + QString::fromStdWString(root.filename().wstring());
        else
            title += " - " + QString::fromStdWString(root.filename().wstring());
    }
    setWindowTitle(title + " - Lila " + version::number());
}

} // namespace lila::ui
