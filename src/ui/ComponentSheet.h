#pragma once

#include "core/Export.h"

#include <QWidget>

#include <memory>
#include <string>
#include <utility>
#include <vector>

class QTableView;

namespace lila::ui
{
/** Registers the Component Spreadsheet panel ("componentSheet", toggled by window.componentSheet). */
LILA_UI_API void registerComponentSheet();

struct ComponentSheetState;

/**
 * Maya's Component Editor for skin weights first: the selected vertices as rows, one column per value. Weight columns are
 * the influences with weight on those vertices, heaviest first, then position and colour. Reads through componentValues
 * and writes through setComponentValues, so every edit is an undoable command; edits go to the lead mesh unless Alt is
 * held. Row and cell coordinates below are those shown, after search and hiding.
 */
class LILA_UI_API ComponentSheet : public QWidget
{
    Q_OBJECT

public:
    explicit ComponentSheet(QWidget* parent = nullptr);
    ~ComponentSheet() override;

    /** Re-reads the selection and the values now; the panel otherwise follows them on its own while shown. */
    void refresh();

    /** Shown columns left to right: joint paths for weights, position.x, color.r. */
    std::vector<std::string> columnIds() const;
    /** Influences on the meshes with no weight on the rows, left out until Show all influences ("+N more"). */
    int moreCount() const;
    int rowCount() const;
    QString rowLabel(int row) const;
    /** A shown cell's value; NaN when the row's mesh has no such column. */
    double value(int row, int column) const;

    void setSearch(const QString& text);
    void setPinned(const std::string& columnId, bool pinned);
    void setHideZeroRows(bool on);
    void setHideZeroColumns(bool on);
    void setShowAllInfluences(bool on);
    /** Holds or releases a weight column's influence (its lockWeights) on every mesh. */
    void setHeld(const std::string& columnId, bool held);
    /** Renormalises the selected rows (all shown when none) keeping this column's weights. */
    void normalizeColumn(const std::string& columnId);

    /** Type-to-set: what typing a number and Enter does on these cells. False when nothing could take it. */
    bool typeValue(const std::vector<std::pair<int, int>>& cells, const QString& text);
    /** Pastes rows of tab, comma or space separated numbers with the block's top left at a cell. */
    bool paste(int row, int column, const QString& text);

    QTableView* table() const;

private:
    std::unique_ptr<ComponentSheetState> d;
};
} // namespace lila::ui
