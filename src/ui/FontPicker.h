#pragma once

#include "core/Export.h"

#include <QWidget>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class QComboBox;
class QToolButton;

namespace lila::ui
{
/**
 * Picks a font as textCurves names one: a family dropdown (each family drawn in itself, type to filter), a style dropdown
 * of that family's styles, a star to favourite the family, and a browse button for a font file. The value is "Family"
 * for its regular style, else "Family Style", or a file path. The dropdown lists the families the scene uses, the
 * favourites and the recent picks above all the others.
 */
class LILA_UI_API FontPicker : public QWidget
{
    Q_OBJECT

public:
    explicit FontPicker(QWidget* parent = nullptr);

    /** Shows a value without emitting chosen. */
    void setValue(const std::string& font);
    std::string value() const { return mValue; }
    std::string family() const;
    std::string style() const;
    /** The family's style closest to the current one with bold and italic as asked, or empty when it has none. */
    std::string styleWith(bool bold, bool italic) const;
    void setStyle(const std::string& style);

Q_SIGNALS:
    void chosen(const QString& font);

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void fillFamilies();
    void pickFamily(const QString& family);
    void fillStyles(const QString& family, const QString& keep);
    void commit();
    void toggleFavorite(const QString& family);
    void updateStar();
    QString previewText() const;

    QComboBox* mFamily = nullptr;
    QComboBox* mStyle = nullptr;
    QToolButton* mStar = nullptr;
    QToolButton* mBrowse = nullptr;
    std::string mValue;
    uint64_t mFamiliesVersion = 0;
    /** The quick sections the dropdown was last filled with, to refill only when they change. */
    std::vector<std::pair<QString, std::vector<std::string>>> mSections;
};

/** Families starred in font pickers: the workspace's when one is open, else the user's. */
LILA_UI_API std::vector<std::string> favoriteFonts();
LILA_UI_API void setFavoriteFont(const std::string& family, bool favorite);
/** Families picked recently, newest first. */
LILA_UI_API std::vector<std::string> recentFonts();
/** Families the current scene's font attributes name, sorted. */
LILA_UI_API std::vector<std::string> sceneFonts();

/** Whether a style name reads as bold (or heavier) or as italic. */
LILA_UI_API bool styleIsBold(const std::string& style);
LILA_UI_API bool styleIsItalic(const std::string& style);
} // namespace lila::ui
