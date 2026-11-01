#include "ui/ImagePicker.h"

#include "core/ImageIO.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "render/HdrImage.h"
#include "scene/Node.h"
#include "ui/Environments.h"
#include "ui/FileDialog.h"
#include "ui/Icons.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QImage>
#include <QMenu>
#include <QPixmap>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QToolButton>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace fs = std::filesystem;

namespace lila::ui
{
namespace
{
const QString kBrowseItem = "\x01browse";
const QString kImageFiles = "Images (*.png *.jpg *.jpeg *.tga *.hdr *.exr *.bmp *.tif *.tiff *.psd *.webp)";

/** Calls opening before its list shows, so files added to the folders since appear. */
class RefillCombo final : public QComboBox
{
public:
    using QComboBox::QComboBox;
    std::function<void()> opening;
    void showPopup() override
    {
        if (opening)
            opening();
        QComboBox::showPopup();
    }
};
} // namespace

namespace imagePicker
{
namespace
{

std::map<std::string, std::unique_ptr<Library>, std::less<>>& libraries()
{
    static std::map<std::string, std::unique_ptr<Library>, std::less<>> m;
    return m;
}

/** "type.attr" to library id. */
std::map<std::string, std::string, std::less<>>& bindings()
{
    static std::map<std::string, std::string, std::less<>> m;
    return m;
}

bool isImage(const fs::path& p)
{
    std::string ext = paths::toUtf8(p.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    static const std::vector<std::string> kExts{".png", ".jpg", ".jpeg", ".tga", ".hdr", ".exr", ".bmp", ".tif", ".tiff", ".psd", ".webp"};
    return std::ranges::find(kExts, ext) != kExts.end();
}

/** The images in dir, by name, under group. */
std::vector<Entry> folderEntries(const fs::path& dir, const QString& group, bool storeFileName)
{
    std::vector<fs::path> files;
    std::error_code ec;
    if (!dir.empty())
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_regular_file(ec) && isImage(e.path()))
                files.push_back(e.path());
    std::ranges::sort(files);
    std::vector<Entry> out;
    for (const fs::path& f : files)
        out.push_back({storeFileName ? paths::toUtf8(f.filename()) : paths::storedPath(f), QString::fromStdString(environments::displayName(f)), f, group});
    return out;
}

fs::path projectSubdir(const char* name) { return Workspace::get().isOpen() ? Workspace::get().stateDir(false) / name : fs::path(); }

void addBuiltIns()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    const fs::path user = PrefStore::get().userDir() / "hdri";
    libraries()["hdri"] = std::make_unique<Library>(Library{
        .id = "hdri",
        .title = "HDRIs",
        .entries =
            [user] {
                std::vector<Entry> out = folderEntries(paths::resourcesDir() / "hdri", "Lila", true);
                for (Entry& e : folderEntries(user, "Mine", false))
                    out.push_back(std::move(e));
                for (Entry& e : folderEntries(projectSubdir("hdri"), "Project", false))
                    out.push_back(std::move(e));
                return out;
            },
        .userDir = user,
        .projectDir = [] { return projectSubdir("hdri"); },
        .filter = "HDR images (*.hdr *.exr);;" + kImageFiles,
    });
    bindings()["light.texture"] = "hdri";
}

/** Linear float RGB to an sRGB 8-bit thumbnail, tone mapped when bright. */
QImage toThumb(int w, int h, const std::function<glm::vec3(int, int)>& at, int size)
{
    const double scale = double(size) / std::max(w, h);
    const int tw = std::max(1, int(std::lround(w * scale))), th = std::max(1, int(std::lround(h * scale)));
    QImage img(tw, th, QImage::Format_RGB32);
    auto enc = [](float c) {
        c = c / (1.0f + c * 0.25f);
        c = std::clamp(c, 0.0f, 1.0f);
        return int(std::lround(255.0f * (c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f)));
    };
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x)
        {
            const glm::vec3 c = at(std::min(w - 1, int((x + 0.5) * w / tw)), std::min(h - 1, int((y + 0.5) * h / th)));
            img.setPixel(x, y, qRgb(enc(c.r), enc(c.g), enc(c.b)));
        }
    return img;
}

QImage loadThumb(const fs::path& file, int size)
{
    std::string ext = paths::toUtf8(file.extension());
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext == ".hdr")
    {
        render::HdrImage hdr;
        std::string error;
        if (!render::loadHdr(file, hdr, error) || hdr.width <= 0)
            return {};
        return toThumb(hdr.width, hdr.height, [&](int x, int y) { return hdr.at(x, y); }, size);
    }
    if (ext == ".exr" || ext == ".tif" || ext == ".tiff" || ext == ".psd" || ext == ".tga" || ext == ".webp")
    {
        const image::Image im = image::read(file);
        if (im.width <= 0)
            return {};
        // Only EXR holds linear light; the rest are display encoded already, so undo it for toThumb.
        const bool linear = ext == ".exr";
        return toThumb(im.width, im.height,
                       [&](int x, int y) {
                           const float* p = &im.rgba[(size_t(y) * size_t(im.width) + size_t(x)) * 4];
                           auto lin = [&](float c) { return linear ? c : std::pow(std::clamp(c, 0.0f, 1.0f), 2.2f); };
                           return glm::vec3(lin(p[0]), lin(p[1]), lin(p[2]));
                       },
                       size);
    }
    QImage img(QString::fromStdString(paths::toUtf8(file)));
    return img.isNull() ? img : img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}
} // namespace

void addLibrary(Library lib)
{
    addBuiltIns();
    auto& slot = libraries()[lib.id];
    if (slot)
        *slot = std::move(lib);
    else
        slot = std::make_unique<Library>(std::move(lib));
}

const Library* library(std::string_view id)
{
    addBuiltIns();
    auto it = libraries().find(id);
    return it == libraries().end() ? nullptr : it->second.get();
}

void setLibraryFor(std::string nodeType, std::string attr, std::string lib)
{
    addBuiltIns();
    bindings()[nodeType + "." + attr] = std::move(lib);
}

const Library* libraryFor(const scene::NodeType& type, std::string_view attr)
{
    addBuiltIns();
    for (const scene::NodeType* t = &type; t; t = t->base)
        if (auto it = bindings().find(t->name + "." + std::string(attr)); it != bindings().end())
            return library(it->second);
    return nullptr;
}

QIcon thumbnail(const fs::path& file, int size)
{
    static std::map<std::pair<std::string, int>, std::pair<fs::file_time_type, QIcon>> cache;
    std::error_code ec;
    const auto stamp = fs::last_write_time(file, ec);
    if (ec)
        return {};
    const auto key = std::pair(paths::toUtf8(file), size);
    if (auto it = cache.find(key); it != cache.end() && it->second.first == stamp)
        return it->second.second;
    QIcon icon;
    try
    {
        if (QImage img = loadThumb(file, size); !img.isNull())
            icon = QIcon(QPixmap::fromImage(img));
    }
    catch (const std::exception& e)
    {
        log::write(LogLevel::Warning, std::string("thumbnail ") + key.first + ": " + e.what());
    }
    cache[key] = {stamp, icon};
    return icon;
}
} // namespace imagePicker

ImagePicker::ImagePicker(const imagePicker::Library* library, QString title, QWidget* parent)
    : QWidget(parent), mLibrary(library), mTitle(std::move(title))
{
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(3);
    auto* combo = new RefillCombo(this);
    combo->opening = [this] { fill(); };
    mCombo = combo;
    mCombo->setIconSize(QSize(48, 24));
    mCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    mCombo->setMinimumContentsLength(10);
    row->addWidget(mCombo, 1);
    connect(mCombo, &QComboBox::activated, this, [this](int index) {
        const QString data = mCombo->itemData(index).toString();
        if (data == kBrowseItem)
            browse();
        else
            Q_EMIT chosen(data);
    });

    if (mLibrary)
    {
        auto* manage = new QToolButton(this);
        manage->setIcon(icon("library-plus"));
        manage->setToolTip("Add " + mLibrary->title.toLower() + " to your library or the project's, or open their folders");
        manage->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(manage);
        const imagePicker::Library* lib = mLibrary;
        connect(menu->addAction("Add to My " + lib->title + "..."), &QAction::triggered, this, [this, lib] { addTo(lib->userDir); });
        QAction* addProject = menu->addAction("Add to Project " + lib->title + "...");
        connect(addProject, &QAction::triggered, this, [this, lib] { addTo(lib->projectDir ? lib->projectDir() : fs::path()); });
        menu->addSeparator();
        auto reveal = [](const fs::path& dir) {
            if (dir.empty())
                return;
            std::error_code ec;
            fs::create_directories(dir, ec);
            QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(paths::toUtf8(dir))));
        };
        connect(menu->addAction("Open My " + lib->title + " Folder"), &QAction::triggered, this, [lib, reveal] { reveal(lib->userDir); });
        QAction* openProject = menu->addAction("Open Project " + lib->title + " Folder");
        connect(openProject, &QAction::triggered, this, [lib, reveal] { reveal(lib->projectDir ? lib->projectDir() : fs::path()); });
        connect(menu, &QMenu::aboutToShow, this, [lib, addProject, openProject] {
            const bool project = lib->projectDir && !lib->projectDir().empty();
            addProject->setEnabled(project);
            openProject->setEnabled(project);
        });
        manage->setMenu(menu);
        row->addWidget(manage);
    }
    fill();
}

void ImagePicker::setValue(const std::string& value)
{
    if (value == mValue && mCombo->count() > 0)
        return;
    mValue = value;
    fill();
}

void ImagePicker::fill()
{
    QSignalBlocker block(mCombo);
    mCombo->clear();
    mCombo->addItem("None", QString());
    int selected = 0;
    QString group;
    auto* model = qobject_cast<QStandardItemModel*>(mCombo->model());
    if (mLibrary && mLibrary->entries)
        for (const imagePicker::Entry& e : mLibrary->entries())
        {
            if (e.group != group)
            {
                group = e.group;
                mCombo->insertSeparator(mCombo->count());
                mCombo->addItem(group);
                if (model)
                    model->item(mCombo->count() - 1)->setEnabled(false);
            }
            mCombo->addItem(imagePicker::thumbnail(e.file), e.name, QString::fromStdString(e.value));
            mCombo->setItemData(mCombo->count() - 1, QString::fromStdString(paths::toUtf8(e.file)), Qt::ToolTipRole);
            if (e.value == mValue)
                selected = mCombo->count() - 1;
        }
    if (!mValue.empty() && selected == 0)
    {
        mCombo->insertSeparator(mCombo->count());
        const fs::path file = paths::resolvePath(mValue);
        std::error_code ec;
        const bool exists = fs::exists(file, ec);
        mCombo->addItem(imagePicker::thumbnail(file), QString::fromStdString(paths::toUtf8(fs::path(paths::fromUtf8(mValue)).filename())) + (exists ? "" : " (missing)"),
                        QString::fromStdString(mValue));
        mCombo->setItemData(mCombo->count() - 1, QString::fromStdString(paths::toUtf8(file)), Qt::ToolTipRole);
        selected = mCombo->count() - 1;
    }
    mCombo->insertSeparator(mCombo->count());
    mCombo->addItem(icon("folder-open"), "Browse...", kBrowseItem);
    mCombo->setCurrentIndex(selected);
}

void ImagePicker::browse()
{
    const QString filter = (mLibrary && !mLibrary->filter.isEmpty() ? mLibrary->filter : kImageFiles) + ";;All Files (*)";
    const QString f = fileDialog::getOpenFileName(this, mTitle, {}, filter);
    if (f.isEmpty())
    {
        fill();
        return;
    }
    Q_EMIT chosen(QString::fromStdString(paths::storedPath(fs::path(f.toStdU16String()))));
}

void ImagePicker::addTo(const fs::path& dir)
{
    if (dir.empty() || !mLibrary)
        return;
    const QStringList files = fileDialog::getOpenFileNames(this, "Add " + mLibrary->title, {}, mLibrary->filter.isEmpty() ? kImageFiles : mLibrary->filter);
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path last;
    for (const QString& f : files)
    {
        const fs::path src(f.toStdU16String());
        fs::path dst = dir / src.filename();
        for (int n = 2; fs::exists(dst, ec); ++n)
            dst = dir / (paths::fromUtf8(paths::toUtf8(src.stem()) + "_" + std::to_string(n)).native() + src.extension().native());
        if (!fs::copy_file(src, dst, ec))
            log::error("Add {}: {}", paths::toUtf8(src), ec.message());
        else
            last = dst;
    }
    if (!last.empty())
        Q_EMIT chosen(QString::fromStdString(paths::storedPath(last)));
}
} // namespace lila::ui
