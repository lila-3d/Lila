#include "ui/SupportBundle.h"

#include "core/CrashHandler.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Version.h"
#include "io/SceneFile.h"
#include "ui/FileDialog.h"

#include <QCheckBox>
#include <QDate>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <miniz.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace lila::ui::support
{
namespace
{
namespace fs = std::filesystem;

constexpr size_t kCrashReports = 3;
constexpr uintmax_t kMaxPrefFile = 2 * 1024 * 1024;

std::string readFile(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool secretKey(std::string key)
{
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* word : {"token", "password", "passwd", "secret", "apikey", "api_key"})
        if (key.find(word) != std::string::npos)
            return true;
    return false;
}

void redactSecrets(json& j)
{
    if (j.is_object())
    {
        for (auto& [k, v] : j.items())
        {
            if (secretKey(k) && !v.is_object() && !v.is_array())
                v = "<redacted>";
            else
                redactSecrets(v);
        }
    }
    else if (j.is_array())
    {
        for (json& v : j)
            redactSecrets(v);
    }
}

class Zip
{
public:
    explicit Zip(const fs::path& file)
    {
        std::u8string u = file.u8string();
        mOk = mz_zip_writer_init_file(&mZip, reinterpret_cast<const char*>(u.c_str()), 0);
    }
    ~Zip()
    {
        if (mOk)
            mz_zip_writer_end(&mZip);
    }

    bool ok() const { return mOk; }
    bool add(const std::string& name, const std::string& data)
    {
        return mOk && mz_zip_writer_add_mem(&mZip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    }
    bool finish()
    {
        if (!mOk || !mz_zip_writer_finalize_archive(&mZip))
            return false;
        mz_zip_writer_end(&mZip);
        mOk = false;
        return true;
    }

private:
    mz_zip_archive mZip{};
    bool mOk = false;
};

std::string zipName(const fs::path& relative)
{
    std::u8string u = relative.generic_u8string();
    return std::string(u.begin(), u.end());
}

void addPrefs(Zip& zip)
{
    const fs::path root = PrefStore::get().userDir();
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
    {
        const fs::path rel = fs::relative(it->path(), root, ec);
        // The telemetry folder holds the install id and queued events; they are not support material.
        if (it->is_directory() && rel == "telemetry")
        {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file() || it->path().extension() != ".json" || it->file_size(ec) > kMaxPrefFile)
            continue;
        json j = json::parse(readFile(it->path()), nullptr, false);
        if (j.is_discarded())
            continue;
        redactSecrets(j);
        zip.add("prefs/" + zipName(rel), j.dump(2));
    }
}

bool sceneAllowed()
{
    return PrefStore::get().value<bool>("general", kAllowScenePref, true);
}
} // namespace

bool writeBundle(const fs::path& zipFile, bool includeScene, std::string* error)
{
    auto fail = [&](const std::string& what) {
        if (error)
            *error = what;
        return false;
    };
    Zip zip(zipFile);
    if (!zip.ok())
        return fail("Can't write " + paths::toUtf8(zipFile));

    zip.add("README.txt", "Lila support bundle, " + version::full() +
                              "\n\nsession.txt     this session: version, system, GPU, recent log and commands\n"
                              "crashes/        the newest crash reports and their minidumps\n"
                              "prefs/          preferences, with tokens and passwords removed\n"
                              "scene/          the open scene, if it was included\n");
    zip.add("session.txt", crash::liveReport());

    std::vector<fs::path> reports = crash::reports();
    for (size_t i = 0; i < reports.size() && i < kCrashReports; ++i)
    {
        const std::string dir = "crashes/" + zipName(reports[i].filename()) + "/";
        for (const char* file : {"report.txt", "minidump.dmp"})
        {
            std::error_code ec;
            if (fs::exists(reports[i] / file, ec))
                zip.add(dir + file, readFile(reports[i] / file));
        }
    }

    addPrefs(zip);

    if (includeScene && sceneAllowed())
    {
        const fs::path current = io::currentPath();
        const std::string name = current.empty() ? "untitled.lila" : zipName(current.filename());
        const fs::path tmp = fs::temp_directory_path() / ("lila-support-" + std::to_string(QDateTime::currentMSecsSinceEpoch()) + "-" + name);
        try
        {
            io::saveCopy(tmp);
            zip.add("scene/" + name, readFile(tmp));
        }
        catch (const std::exception& e)
        {
            zip.add("scene/NOT-INCLUDED.txt", std::string("The scene couldn't be saved: ") + e.what());
        }
        std::error_code ec;
        fs::remove(tmp, ec);
    }

    if (!zip.finish())
        return fail("Can't finish " + paths::toUtf8(zipFile));
    return true;
}

void showDialog(QWidget* parent)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("Save Support Bundle");
    auto* layout = new QVBoxLayout(&dlg);
    auto* text = new QLabel("Saves one zip with Lila's version, your system and GPU, this session's log, your last crash reports and your "
                            "preferences (tokens and passwords removed). Nothing is sent: attach the zip to your bug report.",
                            &dlg);
    text->setWordWrap(true);
    layout->addWidget(text);
    auto* scene = new QCheckBox("Include the open scene", &dlg);
    if (!sceneAllowed())
    {
        scene->setEnabled(false);
        scene->setToolTip("Turned off by your studio");
    }
    layout->addWidget(scene);
    auto* buttons = new QDialogButtonBox(&dlg);
    buttons->addButton("Save...", QDialogButtonBox::AcceptRole)->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg.resize(460, dlg.sizeHint().height());
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))
                                  .filePath("lila-support-" + QDate::currentDate().toString("yyyy-MM-dd") + ".zip");
    QString file = fileDialog::getSaveFileName(parent, "Save Support Bundle", suggested, "Zip archives (*.zip)");
    if (file.isEmpty())
        return;
    if (!file.endsWith(".zip", Qt::CaseInsensitive))
        file += ".zip";
    std::string error;
    if (!writeBundle(fs::path(file.toStdWString()), scene->isChecked(), &error))
        QMessageBox::warning(parent, "Save Support Bundle", QString::fromStdString(error));
    else
        fileOps::reveal(file);
}
} // namespace lila::ui::support
