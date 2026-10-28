#pragma once

#include "core/Export.h"

#include <QWidget>

#include <cstdint>
#include <string>

class QComboBox;
class QToolButton;

namespace lila::ui
{
/**
 * Picks a font as textCurves names one: a family dropdown (each family drawn in itself, type to filter), a style dropdown
 * of that family's styles, and a browse button for a font file. The value is "Family" for its regular style, else
 * "Family Style", or a file path.
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
    QString previewText() const;

    QComboBox* mFamily = nullptr;
    QComboBox* mStyle = nullptr;
    QToolButton* mBrowse = nullptr;
    std::string mValue;
    uint64_t mFamiliesVersion = 0;
};

/** Whether a style name reads as bold (or heavier) or as italic. */
LILA_UI_API bool styleIsBold(const std::string& style);
LILA_UI_API bool styleIsItalic(const std::string& style);
} // namespace lila::ui
