#include "ui/FontPicker.h"

#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "curve/Text.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "ui/Icons.h"
#include "ui/MultilineEdit.h"
#include "ui/FileDialog.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStandardItemModel>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QToolButton>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <functional>
#include <set>

namespace lila::ui
{
namespace
{
std::string norm(std::string_view s)
{
    std::string out;
    for (char c : s)
        if (c != ' ' && c != '-' && c != '_')
            out += char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool contains(const std::string& s, std::initializer_list<const char*> words)
{
    std::string n = norm(s);
    return std::any_of(words.begin(), words.end(), [&](const char* w) { return n.find(w) != std::string::npos; });
}

struct FamilyList
{
    std::vector<curve::FontFamily> families = curve::fontFamilies();
    uint64_t version = curve::fontListVersion();
};

FamilyList& familyList()
{
    static FamilyList list;
    return list;
}

const std::vector<curve::FontFamily>& families() { return familyList().families; }

/** Rescans when fonts were installed or removed; returns the list's version. Invalidates FontFamily pointers. */
uint64_t refreshFamilies()
{
    FamilyList& list = familyList();
    if (uint64_t v = curve::fontListVersion(); v != list.version)
        list = FamilyList();
    return list.version;
}

/** The family as Qt draws it, loading the font file into Qt when Qt doesn't know the family (installed since launch). */
QFont familyFont(const QString& family)
{
    static std::set<QString> loaded;
    if (!QFontDatabase::hasFamily(family) && loaded.insert(family).second)
    {
        QString file = QString::fromStdString(curve::findFont(family.toStdString()));
        QFontDatabase::addApplicationFont(file.left(file.lastIndexOf('#') > 2 ? file.lastIndexOf('#') : file.size()));
    }
    return QFont(family);
}

const curve::FontFamily* familyNamed(const std::string& name)
{
    std::string q = norm(name);
    for (const curve::FontFamily& f : families())
        if (norm(f.family) == q)
            return &f;
    return nullptr;
}

/** The family and style a value names: the longest family it starts with, then the style the rest matches. */
std::pair<const curve::FontFamily*, std::string> parse(const std::string& value)
{
    std::string q = norm(value);
    const curve::FontFamily* best = nullptr;
    for (const curve::FontFamily& f : families())
    {
        std::string fn = norm(f.family);
        if (q.starts_with(fn) && (!best || fn.size() > norm(best->family).size()))
            best = &f;
    }
    if (!best)
        return {nullptr, {}};
    std::string rest = q.substr(norm(best->family).size());
    for (const std::string& s : best->styles)
        if (norm(s) == rest || (rest.empty() && contains(s, {"regular", "book", "normal", "roman"})))
            return {best, s};
    return {best, best->styles.empty() ? std::string() : best->styles.front()};
}

bool isRegular(const std::string& style) { return style.empty() || norm(style) == "regular"; }

constexpr const char* kFontPrefs = "fonts";
constexpr size_t kMaxRecent = 8;
/** Marks a dropdown row as a section title rather than a family. */
constexpr int kHeaderRole = Qt::UserRole + 1;

std::vector<std::string> prefList(const char* key)
{
    std::vector<std::string> out;
    json v = PrefStore::get().value(kFontPrefs, key, json::array());
    if (v.is_array())
        for (const json& e : v)
            if (e.is_string())
                out.push_back(e.get<std::string>());
    return out;
}

void addRecent(const std::string& family)
{
    std::vector<std::string> list = prefList("/recent");
    std::erase(list, family);
    list.insert(list.begin(), family);
    if (list.size() > kMaxRecent)
        list.resize(kMaxRecent);
    PrefStore::get().set(kFontPrefs, "/recent", list);
}

bool isFavorite(const std::vector<std::string>& favorites, const QString& family)
{
    return std::find(favorites.begin(), favorites.end(), family.toStdString()) != favorites.end();
}

/** The star at the right end of a dropdown row, where a click toggles the favourite. */
QRect starRect(const QRect& row)
{
    int s = std::min(row.height(), 18);
    return {row.right() - s - 2, row.center().y() - s / 2, s, s};
}

void drawStar(QPainter* p, const QRect& r, const QColor& color, bool filled)
{
    constexpr double pi = 3.14159265358979323846;
    QPainterPath path;
    QPointF c = QRectF(r).center();
    double outer = r.width() * 0.42, inner = outer * 0.45;
    for (int i = 0; i < 10; ++i)
    {
        double a = -pi / 2 + i * pi / 5;
        double rad = i % 2 ? inner : outer;
        QPointF pt(c.x() + rad * std::cos(a), c.y() + rad * std::sin(a));
        if (i)
            path.lineTo(pt);
        else
            path.moveTo(pt);
    }
    path.closeSubpath();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(color, 1.2));
    p->setBrush(filled ? QBrush(color) : Qt::NoBrush);
    p->drawPath(path);
    p->restore();
}

/** Draws each family as its name in the UI font, then a grey sample of the picker's text in that family. */
class FamilyDelegate : public QStyledItemDelegate
{
public:
    /** With stars, every row shows one to click (outlined until starred, on hover); without, only starred rows do. */
    FamilyDelegate(std::function<QString()> sample, bool stars, QObject* parent)
        : QStyledItemDelegate(parent), mSample(std::move(sample)), mStars(stars)
    {
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        if (index.data(kHeaderRole).toBool())
        {
            paintHeader(p, option, index.data().toString());
            return;
        }
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        QString name = opt.text;
        opt.text.clear();
        opt.font = option.font;
        const QWidget* w = opt.widget;
        QStyle* style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, p, w);

        bool selected = opt.state & QStyle::State_Selected;
        QPalette::ColorRole role = selected ? QPalette::HighlightedText : QPalette::Text;
        bool favorite = isFavorite(favoriteFonts(), name);
        QRect star = starRect(opt.rect);
        if (favorite || (mStars && (opt.state & QStyle::State_MouseOver)))
        {
            QColor c = favorite ? QColor(240, 190, 60) : opt.palette.color(role);
            if (!favorite)
                c.setAlphaF(0.45f);
            drawStar(p, star, c, favorite);
        }

        QRect r = opt.rect.adjusted(4, 0, -(star.width() + 8), 0);
        p->save();
        p->setFont(option.font);
        p->setPen(opt.palette.color(role));
        QFontMetrics fm(option.font);
        int nameW = std::min(r.width() * 11 / 20, fm.horizontalAdvance(QString(22, 'n')));
        p->drawText(QRect(r.left(), r.top(), nameW, r.height()), Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(name, Qt::ElideRight, nameW));

        QRect sr(r.left() + nameW + 8, r.top(), r.width() - nameW - 8, r.height());
        if (sr.width() > 8)
        {
            QFont f = familyFont(name);
            f.setPointSizeF(option.font.pointSizeF());
            QColor grey = opt.palette.color(role);
            grey.setAlphaF(0.5f);
            p->setFont(f);
            p->setPen(grey);
            p->drawText(sr, Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(f).elidedText(mSample(), Qt::ElideRight, sr.width()));
        }
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        if (index.data(kHeaderRole).toBool())
            return {s.width(), QFontMetrics(option.font).height() + 6};
        QFont f = familyFont(index.data().toString());
        f.setPointSizeF(option.font.pointSizeF());
        return {s.width(), std::max(s.height(), QFontMetrics(f).height() + 4)};
    }

private:
    static void paintHeader(QPainter* p, const QStyleOptionViewItem& option, const QString& title)
    {
        QFont f = option.font;
        f.setBold(true);
        f.setPointSizeF(option.font.pointSizeF() * 0.85);
        QColor c = option.palette.color(QPalette::Text);
        c.setAlphaF(0.55f);
        p->save();
        p->setFont(f);
        p->setPen(c);
        p->drawText(option.rect.adjusted(4, 2, -4, 0), Qt::AlignBottom | Qt::AlignLeft, title.toUpper());
        p->restore();
    }

    std::function<QString()> mSample;
    bool mStars = false;
};

/** The dropdown's quick sections by title, each family in the first section that has it. */
std::vector<std::pair<QString, std::vector<std::string>>> quickSections()
{
    std::vector<std::pair<QString, std::vector<std::string>>> out;
    std::set<std::string> seen;
    auto add = [&](const char* title, std::vector<std::string> list) {
        std::erase_if(list, [&](const std::string& f) { return !familyNamed(f) || !seen.insert(f).second; });
        if (!list.empty())
            out.emplace_back(title, std::move(list));
    };
    add("In Scene", sceneFonts());
    std::vector<std::string> favorites = favoriteFonts();
    std::sort(favorites.begin(), favorites.end());
    add("Favorites", favorites);
    add("Recent", recentFonts());
    return out;
}
} // namespace

std::vector<std::string> favoriteFonts() { return prefList("/favorites"); }

void setFavoriteFont(const std::string& family, bool favorite)
{
    std::vector<std::string> list = favoriteFonts();
    std::erase(list, family);
    if (favorite)
        list.push_back(family);
    PrefStore::get().set(kFontPrefs, "/favorites", list, Workspace::get().isOpen() ? PrefScope::Workspace : PrefScope::User);
}

std::vector<std::string> recentFonts() { return prefList("/recent"); }

std::vector<std::string> sceneFonts()
{
    std::set<std::string> found;
    scene::Graph& g = scene::Graph::current();
    for (scene::NodeId id : g.nodes())
    {
        const scene::Node* n = g.node(id);
        if (!n)
            continue;
        const std::vector<scene::AttrDef>& attrs = n->type().attrs;
        for (size_t i = 0; i < attrs.size(); ++i)
            if (attrs[i].flags & scene::AttrFont)
                if (const auto* v = std::get_if<std::string>(&n->value(int(i))))
                    if (const curve::FontFamily* fam = parse(*v).first)
                        found.insert(fam->family);
    }
    return {found.begin(), found.end()};
}

bool styleIsBold(const std::string& style) { return contains(style, {"bold", "black", "heavy", "extrabold", "ultrabold"}); }
bool styleIsItalic(const std::string& style) { return contains(style, {"italic", "oblique"}); }

FontPicker::FontPicker(QWidget* parent) : QWidget(parent)
{
    auto* h = new QHBoxLayout(this);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);

    mFamily = new QComboBox;
    mFamily->setEditable(true);
    mFamily->setInsertPolicy(QComboBox::NoInsert);
    mFamily->setMaxVisibleItems(20);
    mFamily->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    mFamily->setToolTip("Font family. Type to filter.");
    fillFamilies();
    // Picks up fonts installed since the list was filled, before the user looks at it.
    mFamily->installEventFilter(this);
    mFamily->completer()->setFilterMode(Qt::MatchContains);
    mFamily->completer()->setCompletionMode(QCompleter::PopupCompletion);
    mFamily->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    auto sample = [this] { return previewText(); };
    mFamily->setItemDelegate(new FamilyDelegate(sample, true, mFamily));
    mFamily->completer()->popup()->setItemDelegate(new FamilyDelegate(sample, false, mFamily));
    mFamily->view()->setMouseTracking(true);
    mFamily->view()->viewport()->installEventFilter(this);
    mFamily->view()->setMinimumWidth(360);
    mFamily->completer()->popup()->setMinimumWidth(360);
    connect(mFamily, &QComboBox::activated, this, [this](int i) { pickFamily(mFamily->itemText(i)); });
    connect(mFamily->lineEdit(), &QLineEdit::editingFinished, this, [this] {
        if (familyNamed(mFamily->currentText().toStdString()))
            pickFamily(mFamily->currentText());
    });

    mStyle = new QComboBox;
    mStyle->setToolTip("Style: the weights and italics the family comes in.");
    mStyle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(mStyle, &QComboBox::activated, this, [this](int) { commit(); });

    mStar = new QToolButton;
    mStar->setIcon(icon("star"));
    mStar->setCheckable(true);
    connect(mStar, &QToolButton::clicked, this, [this] { toggleFavorite(QString::fromStdString(family())); });

    mBrowse = new QToolButton;
    mBrowse->setIcon(icon("folder-open"));
    mBrowse->setToolTip("Use a font file (.ttf, .otf, .ttc)...");
    connect(mBrowse, &QToolButton::clicked, this, [this] {
        std::filesystem::path start = paths::fromUtf8(mValue);
        QString f = ui::fileDialog::getOpenFileName(this, "Font", QString::fromStdU16String(start.parent_path().u16string()),
                                                 "Fonts (*.ttf *.otf *.ttc);;All Files (*)");
        if (f.isEmpty())
            return;
        mValue = paths::storedPath(std::filesystem::path(f.toStdU16String()));
        setValue(mValue);
        Q_EMIT chosen(QString::fromStdString(mValue));
    });

    h->addWidget(mFamily, 3);
    h->addWidget(mStyle, 2);
    h->addWidget(mStar);
    h->addWidget(mBrowse);
}

void FontPicker::setValue(const std::string& font)
{
    mValue = font;
    auto [fam, style] = parse(font);
    QSignalBlocker b1(mFamily), b2(mStyle);
    if (!fam)
    {
        // A file, or a name nothing matches: shown as typed.
        mFamily->setEditText(QString::fromStdString(paths::toUtf8(paths::fromUtf8(font).filename())));
        mFamily->setToolTip(QString::fromStdString(font));
        mStyle->clear();
        mStyle->setEnabled(false);
        updateStar();
        return;
    }
    mFamily->setToolTip("Font family. Type to filter.");
    mFamily->setCurrentIndex(mFamily->findText(QString::fromStdString(fam->family)));
    fillStyles(QString::fromStdString(fam->family), QString::fromStdString(style));
    updateStar();
}

std::string FontPicker::family() const
{
    auto [fam, style] = parse(mValue);
    return fam ? fam->family : std::string();
}

std::string FontPicker::style() const
{
    auto [fam, style] = parse(mValue);
    return style;
}

std::string FontPicker::styleWith(bool bold, bool italic) const
{
    const curve::FontFamily* fam = parse(mValue).first;
    if (!fam)
        return {};
    // Prefer the plainest match: "Bold" over "Bold Condensed".
    const std::string* best = nullptr;
    for (const std::string& s : fam->styles)
        if (styleIsBold(s) == bold && styleIsItalic(s) == italic)
        {
            bool plain = bold || italic ? norm(s) == (bold && italic ? "bolditalic" : bold ? "bold" : "italic") : isRegular(s);
            if (!best || plain)
                best = &s;
            if (plain)
                break;
        }
    return best ? *best : std::string();
}

void FontPicker::setStyle(const std::string& style)
{
    QSignalBlocker block(mStyle);
    mStyle->setCurrentIndex(std::max(0, mStyle->findText(QString::fromStdString(style))));
    commit();
}

void FontPicker::pickFamily(const QString& family)
{
    {
        QSignalBlocker block(mFamily);
        mFamily->setCurrentIndex(mFamily->findText(family));
    }
    fillStyles(family, mStyle->currentText());
    commit();
}

void FontPicker::fillStyles(const QString& family, const QString& keep)
{
    QSignalBlocker block(mStyle);
    mStyle->clear();
    const curve::FontFamily* fam = familyNamed(family.toStdString());
    mStyle->setEnabled(fam != nullptr);
    if (!fam)
        return;
    for (const std::string& s : fam->styles)
        mStyle->addItem(QString::fromStdString(s));
    int i = mStyle->findText(keep);
    if (i < 0)
    {
        // Another family: keep bold and italic if it has them, the plain style of that kind first.
        std::string k = keep.toStdString();
        bool bold = styleIsBold(k), italic = styleIsItalic(k);
        std::string plain = bold && italic ? "bolditalic" : bold ? "bold" : italic ? "italic" : "regular";
        for (int j = 0; j < mStyle->count(); ++j)
        {
            std::string s = mStyle->itemText(j).toStdString();
            if (styleIsBold(s) != bold || styleIsItalic(s) != italic)
                continue;
            if (i < 0)
                i = j;
            if (norm(s) == plain)
            {
                i = j;
                break;
            }
        }
    }
    mStyle->setCurrentIndex(std::max(0, i));
}

void FontPicker::fillFamilies()
{
    QSignalBlocker block(mFamily);
    mFamily->clear();
    mSections = quickSections();
    auto* model = qobject_cast<QStandardItemModel*>(mFamily->model());
    auto addHeader = [&](const QString& title) {
        mFamily->addItem(title);
        QStandardItem* item = model->item(mFamily->count() - 1);
        item->setFlags(Qt::NoItemFlags);
        item->setData(true, kHeaderRole);
    };
    for (const auto& [title, list] : mSections)
    {
        addHeader(title);
        for (const std::string& f : list)
            mFamily->addItem(QString::fromStdString(f));
    }
    if (!mSections.empty())
        addHeader("All Fonts");
    QStringList all;
    for (const curve::FontFamily& f : families())
    {
        all << QString::fromStdString(f.family);
        mFamily->addItem(all.back());
    }
    // Typing filters the families alone, each once, without the section titles.
    auto* completerModel = qobject_cast<QStringListModel*>(mFamily->completer()->model());
    if (!completerModel)
    {
        completerModel = new QStringListModel(mFamily->completer());
        mFamily->completer()->setModel(completerModel);
    }
    completerModel->setStringList(all);
    mFamiliesVersion = familyList().version;
}

bool FontPicker::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == mFamily && (e->type() == QEvent::FocusIn || e->type() == QEvent::MouseButtonPress) &&
        (refreshFamilies() != mFamiliesVersion || quickSections() != mSections))
    {
        fillFamilies();
        setValue(mValue);
    }
    // A click on a row's star toggles the favourite and keeps the dropdown open; the sections refill when it next opens.
    if (obj == mFamily->view()->viewport() && (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonRelease))
    {
        QPoint pos = static_cast<QMouseEvent*>(e)->position().toPoint();
        QModelIndex index = mFamily->view()->indexAt(pos);
        if (index.isValid() && !index.data(kHeaderRole).toBool() && starRect(mFamily->view()->visualRect(index)).contains(pos))
        {
            if (e->type() == QEvent::MouseButtonRelease)
            {
                toggleFavorite(index.data().toString());
                mFamily->view()->viewport()->update();
            }
            return true;
        }
    }
    return QWidget::eventFilter(obj, e);
}

void FontPicker::toggleFavorite(const QString& family)
{
    if (family.isEmpty())
        return;
    setFavoriteFont(family.toStdString(), !isFavorite(favoriteFonts(), family));
    updateStar();
}

void FontPicker::updateStar()
{
    std::string fam = family();
    bool on = !fam.empty() && isFavorite(favoriteFonts(), QString::fromStdString(fam));
    mStar->setEnabled(!fam.empty());
    mStar->setChecked(on);
    mStar->setToolTip(on ? "Favourite: listed at the top of the font dropdown. Click to remove."
                         : "Favourite this font, listing it at the top of the font dropdown. Kept with the workspace when one is open.");
}

QString FontPicker::previewText() const
{
    // The text box the picker sits beside: the Text section, the Attribute Editor or the option box.
    for (QWidget* w = parentWidget(); w; w = w->parentWidget())
        if (auto* edit = w->findChild<MultilineEdit*>())
        {
            QString line = edit->toPlainText().section('\n', 0, 0).trimmed();
            if (!line.isEmpty())
                return line;
            break;
        }
    return "The quick brown fox";
}

void FontPicker::commit()
{
    std::string fam = mFamily->currentText().toStdString();
    if (!familyNamed(fam))
        return;
    std::string style = mStyle->currentText().toStdString();
    std::string v = isRegular(style) ? fam : fam + " " + style;
    if (v == mValue)
        return;
    mValue = v;
    addRecent(fam);
    updateStar();
    Q_EMIT chosen(QString::fromStdString(v));
}
} // namespace lila::ui
