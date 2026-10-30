#include "ui/FileDrop.h"

#include "io/FileTranslators.h"

#include <QFileInfo>
#include <QMimeData>
#include <QUrl>

namespace lila::ui::fileDrop
{
namespace fs = std::filesystem;
namespace tr = io::translators;

std::vector<fs::path> localFiles(const QMimeData* mime)
{
    std::vector<fs::path> files;
    if (!mime || !mime->hasUrls())
        return files;
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile())
            files.emplace_back(url.toLocalFile().toStdWString());
    return files;
}

Plan plan(const std::vector<fs::path>& files, bool importOnly)
{
    Plan p;
    for (const fs::path& f : files)
    {
        // forPath(Open) also returns import-only translators, which would open as a new untitled scene.
        const tr::FileTranslator* opener = tr::forPath(tr::Op::Open, f);
        if (!importOnly && p.open.empty() && opener && opener->open.supported())
            p.open = f;
        else if (tr::forPath(tr::Op::Import, f))
            p.imports.push_back(f);
        else
            p.skipped.push_back(f);
    }
    return p;
}
} // namespace lila::ui::fileDrop
