#include "ui/FontPicker.h"

#include "core/Paths.h"
#include "curve/Text.h"
#include "ui/Icons.h"

#include <QComboBox>
#include <QCompleter>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QToolButton>

#include <algorithm>
#include <cctype>
#include <filesystem>

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

const std::vector<curve::FontFamily>& families()
{
    static const std::vector<curve::FontFamily> f = curve::fontFamilies();
    return f;
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
} // namespace

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
    for (const curve::FontFamily& f : families())
    {
        QString name = QString::fromStdString(f.family);
        mFamily->addItem(name);
        mFamily->setItemData(mFamily->count() - 1, QFont(name), Qt::FontRole);
    }
    mFamily->completer()->setFilterMode(Qt::MatchContains);
    mFamily->completer()->setCompletionMode(QCompleter::PopupCompletion);
    mFamily->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    connect(mFamily, &QComboBox::activated, this, [this](int i) { pickFamily(mFamily->itemText(i)); });
    connect(mFamily->lineEdit(), &QLineEdit::editingFinished, this, [this] {
        if (familyNamed(mFamily->currentText().toStdString()))
            pickFamily(mFamily->currentText());
    });

    mStyle = new QComboBox;
    mStyle->setToolTip("Style: the weights and italics the family comes in.");
    mStyle->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    connect(mStyle, &QComboBox::activated, this, [this](int) { commit(); });

    mBrowse = new QToolButton;
    mBrowse->setIcon(icon("folder-open"));
    mBrowse->setToolTip("Use a font file (.ttf, .otf, .ttc)...");
    connect(mBrowse, &QToolButton::clicked, this, [this] {
        std::filesystem::path start = paths::fromUtf8(mValue);
        QString f = QFileDialog::getOpenFileName(this, "Font", QString::fromStdU16String(start.parent_path().u16string()),
                                                 "Fonts (*.ttf *.otf *.ttc);;All Files (*)");
        if (f.isEmpty())
            return;
        mValue = paths::storedPath(std::filesystem::path(f.toStdU16String()));
        setValue(mValue);
        Q_EMIT chosen(QString::fromStdString(mValue));
    });

    h->addWidget(mFamily, 3);
    h->addWidget(mStyle, 2);
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
        return;
    }
    mFamily->setToolTip("Font family. Type to filter.");
    mFamily->setCurrentIndex(mFamily->findText(QString::fromStdString(fam->family)));
    fillStyles(QString::fromStdString(fam->family), QString::fromStdString(style));
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
    Q_EMIT chosen(QString::fromStdString(v));
}
} // namespace lila::ui
