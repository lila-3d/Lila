#include "ui/Icons.h"

#include "core/Paths.h"

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

/** Renders the SVG at whatever size is asked for, with currentColor swapped for the grey of the mode. */
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
        QString key = QString("%1x%2/%3/%4").arg(px.width()).arg(px.height()).arg(int(mode)).arg(int(state));
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
    QByteArray tinted(QIcon::Mode mode, QIcon::State state) const
    {
        const char* grey = "#c8c8c8";
        if (mode == QIcon::Disabled)
            grey = "#6e6e6e";
        else if (mode == QIcon::Active || mode == QIcon::Selected || state == QIcon::On)
            grey = "#f2f2f2";
        QByteArray out = mSvg;
        out.replace("currentColor", grey);
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
