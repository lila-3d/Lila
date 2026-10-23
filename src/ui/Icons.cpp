#include "ui/Icons.h"

#include "core/Paths.h"
#include "ui/Theme.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

namespace lila::ui
{
namespace
{
QString iconDir()
{
    return QString::fromStdWString((paths::resourcesDir() / "icons" / "tabler").wstring());
}

/** Renders the SVG at whatever size is asked for, with currentColor swapped for the theme's icon colour of the mode. */
class TablerEngine : public QIconEngine
{
public:
    explicit TablerEngine(QByteArray svg) : mSvg(std::move(svg)) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override
    {
        QSvgRenderer renderer(tinted(mode, state));
        renderer.render(painter, rect);
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1.0); }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override
    {
        QSize px = (QSizeF(size) * scale).toSize();
        QString key = QString("%1x%2/%3/%4/%5").arg(px.width()).arg(px.height()).arg(int(mode)).arg(int(state)).arg(themeColor(tintToken(mode, state)).name());
        if (auto it = mCache.find(key); it != mCache.end())
            return *it;
        QPixmap pm(px);
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            QSvgRenderer renderer(tinted(mode, state));
            renderer.render(&p, QRectF(QPointF(0, 0), QSizeF(px)));
        }
        pm.setDevicePixelRatio(scale);
        mCache.insert(key, pm);
        return pm;
    }

    QIconEngine* clone() const override { return new TablerEngine(mSvg); }
    QString key() const override { return "lila.tabler"; }

private:
    static const char* tintToken(QIcon::Mode mode, QIcon::State state)
    {
        if (mode == QIcon::Disabled)
            return "iconDisabled";
        if (mode == QIcon::Active || mode == QIcon::Selected || state == QIcon::On)
            return "iconActive";
        return "iconNormal";
    }

    QByteArray tinted(QIcon::Mode mode, QIcon::State state) const
    {
        QByteArray out = mSvg;
        out.replace("currentColor", themeColor(tintToken(mode, state)).name().toLatin1());
        return out;
    }

    QByteArray mSvg;
    QHash<QString, QPixmap> mCache;
};
} // namespace

QIcon icon(const QString& name)
{
    static QHash<QString, QIcon> cache;
    if (auto it = cache.find(name); it != cache.end())
        return *it;
    QIcon result;
    QFile f(iconDir() + "/" + name + ".svg");
    if (!name.isEmpty() && f.open(QIODevice::ReadOnly))
        result = QIcon(new TablerEngine(f.readAll()));
    cache.insert(name, result);
    return result;
}

const QStringList& iconNames()
{
    static const QStringList names = [] {
        QStringList out;
        for (const QString& file : QDir(iconDir()).entryList({"*.svg"}, QDir::Files, QDir::Name))
            out.push_back(file.chopped(4));
        return out;
    }();
    return names;
}
} // namespace lila::ui
