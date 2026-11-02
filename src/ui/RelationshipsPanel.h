#pragma once

#include "core/Export.h"

#include <QWidget>

#include <string>
#include <vector>

class QLineEdit;
class QTableWidget;

namespace lila::ui
{
/**
 * Every grouping an object can be in, in one place: the selected objects down the side, and as checkable columns their
 * lighting channels, object sets, display layers and animation layers, filtered by one search. Lights and objects share
 * the channel columns, so light linking reads both ways. Replaces Maya's eleven relationship editors.
 */
class LILA_UI_API RelationshipsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit RelationshipsPanel(QWidget* parent = nullptr);
    ~RelationshipsPanel() override;

    /** A column: its kind ("channel", "set", "layer", "animLayer") and the group's name. */
    struct Column
    {
        std::string kind;
        std::string name;
    };
    const std::vector<Column>& columns() const { return mColumns; }
    /** Puts the object in or out of a column's group, through its command. */
    bool setMember(const std::string& object, const Column& column, bool in);

private:
    void scheduleRefresh();
    void refresh();

    QLineEdit* mSearch = nullptr;
    QTableWidget* mTable = nullptr;
    std::vector<Column> mColumns;
    std::vector<std::string> mRows;
    int mListener = 0;
    bool mQueued = false;
    bool mUpdating = false;
};

/** The Relationships dock. */
LILA_UI_API void registerRelationshipsPanel();
} // namespace lila::ui
