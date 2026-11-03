#include "ui/ComponentSheet.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "ui/AttrView.h"
#include "ui/Icons.h"
#include "ui/Panels.h"
#include "ui/Theme.h"
#include "ui/ViewportDisplay.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTableView>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>

namespace lila::ui
{
namespace
{
using scene::Graph;

constexpr double kNoValue = std::numeric_limits<double>::quiet_NaN();

struct CsRow
{
    std::string mesh;
    uint32_t id = 0;
};

struct CsColumn
{
    std::string id, label, kind, joint;
    bool held = false;
    double total = 0.0;
    std::vector<std::string> links;
};

struct CsData
{
    std::vector<CsRow> rows;
    std::vector<CsColumn> cols;
    std::vector<std::vector<double>> values;
};

class CsModel;
class CsTable;
class CsHeader;
class CsMiniMap;

/** A middle drag on a column header: the cells it moves and where they started. */
struct CsScrub
{
    int col = -1;
    QPoint last;
    double accum = 0.0;
    bool open = false;
    size_t mark = 0;
    std::vector<std::pair<int, int>> cells;
    std::vector<double> start;
};

QString csShortName(const std::string& path)
{
    const size_t bar = path.rfind('|');
    return QString::fromStdString(bar == std::string::npos ? path : path.substr(bar + 1));
}

/** The weight ramp of the viewport's paint display, faint so the numbers stay legible. */
QColor csWeightShade(double w)
{
    const bool gray = viewportDisplay::grayscaleWeights();
    glm::vec4 c = viewportDisplay::weightColor(float(std::clamp(w, 0.0, 1.0)), gray);
    auto encode = [gray](float x) { return gray ? std::clamp(x, 0.0f, 1.0f) : std::pow(std::clamp(x, 0.0f, 1.0f), 1.0f / 2.2f); };
    QColor q = QColor::fromRgbF(encode(c.r), encode(c.g), encode(c.b));
    q.setAlphaF(0.28f);
    return q;
}
} // namespace

struct ComponentSheetState
{
    ComponentSheet* self = nullptr;
    CsData data;
    std::vector<int> rowMap, colMap; // shown index to data index
    std::string lead;
    bool multiMesh = false;
    int more = 0;

    QString search;
    std::vector<std::string> pinned;
    bool hideZeroRows = false, hideZeroCols = false, showAll = false;
    bool showWeights = true, showPosition = true, showColor = true;
    std::string highlighted;

    const Graph* lastGraph = nullptr;
    uint64_t lastVersion = ~0ull, lastSerial = ~0ull;
    bool lastMulti = false;
    std::vector<scene::NodeId> lastSelection;
    /** components::serial right after the sheet selected rows' components itself: the rows stay while it holds. */
    uint64_t ownSerial = ~0ull;
    std::optional<CsScrub> scrub;
    int attrListener = 0;

    CsModel* model = nullptr;
    CsTable* table = nullptr;
    CsHeader* header = nullptr;
    CsMiniMap* map = nullptr;
    QLineEdit* searchField = nullptr;
    QToolButton* moreButton = nullptr;
    QToolButton* showAllButton = nullptr;
    QToolButton* hideRowsButton = nullptr;
    QToolButton* hideColsButton = nullptr;

    void refresh(bool force);
    void fetch(bool keepRows);
    void layout();
    void relayout();
    bool cellEditable(int r, int c) const;
    bool apply(const std::vector<std::pair<int, int>>& cells, const std::vector<double>& values, bool relative, const char* label);
    bool typeInto(const std::vector<std::pair<int, int>>& cells, double v);
    std::vector<std::pair<int, int>> selectedCells() const;
    bool isSelected(const QModelIndex& i) const;
    std::vector<int> rowsForColumn(int shownCol) const;
    bool pasteAt(int row, int col, const QString& text);
    QString copyText() const;
    void syncSelection();
    void setHeld(int c, bool held);
    void normalize(int shownCol);
    void togglePin(const std::string& id, bool on);
    void scrollToColumn(int shownCol);
    void columnMenu(int shownCol, QPoint global);
    void beginScrub(int shownCol, QPoint at);
    void scrubTo(QPoint at, Qt::KeyboardModifiers mods);
    void endScrub();
    int shownColumnOf(const std::string& id) const;
};

namespace
{
class CsModel final : public QAbstractTableModel
{
public:
    explicit CsModel(ComponentSheetState& s) : s(s) {}

    void reset()
    {
        beginResetModel();
        endResetModel();
    }

    void changed()
    {
        if (rowCount() && columnCount())
            Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1));
        if (columnCount())
            Q_EMIT headerDataChanged(Qt::Horizontal, 0, columnCount() - 1);
        if (rowCount())
            Q_EMIT headerDataChanged(Qt::Vertical, 0, rowCount() - 1);
    }

    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(s.rowMap.size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : int(s.colMap.size()); }

    QVariant data(const QModelIndex& i, int role) const override
    {
        if (!i.isValid() || size_t(i.row()) >= s.rowMap.size() || size_t(i.column()) >= s.colMap.size())
            return {};
        const int r = s.rowMap[size_t(i.row())], c = s.colMap[size_t(i.column())];
        const double v = s.data.values[size_t(r)][size_t(c)];
        const CsColumn& col = s.data.cols[size_t(c)];
        switch (role)
        {
        case Qt::DisplayRole:
        case Qt::EditRole: return std::isnan(v) ? QString() : attrView::formatNumber(v);
        case Qt::TextAlignmentRole: return int(Qt::AlignRight | Qt::AlignVCenter);
        case Qt::ForegroundRole:
            if ((col.kind == "weight" && v == 0.0) || !s.cellEditable(r, c))
                return themeColor("dimText");
            return {};
        case Qt::BackgroundRole:
            if (col.kind == "weight" && v > 0.0)
                return csWeightShade(v);
            return {};
        default: return {};
        }
    }

    QVariant headerData(int section, Qt::Orientation o, int role) const override
    {
        if (o == Qt::Vertical)
        {
            if (section < 0 || size_t(section) >= s.rowMap.size())
                return {};
            const CsRow& row = s.data.rows[size_t(s.rowMap[size_t(section)])];
            if (role == Qt::DisplayRole)
                return (s.multiMesh ? csShortName(row.mesh) + "." : QString()) + QString("vtx[%1]").arg(row.id);
            if (role == Qt::ForegroundRole && row.mesh != s.lead && !attrView::multiEditHeld())
                return themeColor("dimText");
            if (role == Qt::ToolTipRole && row.mesh != s.lead)
                return QString("%1 is not the lead: hold Alt to edit it too.").arg(csShortName(row.mesh));
            if (role == Qt::TextAlignmentRole)
                return int(Qt::AlignLeft | Qt::AlignVCenter);
            return {};
        }
        if (section < 0 || size_t(section) >= s.colMap.size())
            return {};
        const CsColumn& col = s.data.cols[size_t(s.colMap[size_t(section)])];
        const bool pinned = std::ranges::find(s.pinned, col.id) != s.pinned.end();
        switch (role)
        {
        case Qt::DisplayRole: return QString::fromStdString(col.label);
        case Qt::DecorationRole:
            if (pinned)
                return icon("pin");
            if (col.held)
                return icon("lock");
            return {};
        case Qt::ForegroundRole:
            if (col.id == s.highlighted)
                return themeColor("accent");
            if (col.held)
                return themeColor("dimText");
            return {};
        case Qt::ToolTipRole:
        {
            QString tip;
            if (col.kind == "weight")
                tip = QString("%1\nTotal %2 on these vertices%3")
                          .arg(QString::fromStdString(col.joint.empty() ? col.label : col.joint), attrView::formatNumber(col.total),
                               QString(col.held ? "\nHeld: edits and normalizing leave it alone." : ""));
            else
                tip = QString(col.kind == "position" ? "Position %1 (object space)" : "Colour %1 (current colour set)")
                          .arg(QString::fromStdString(col.label));
            return tip + "\nMiddle-drag to scrub; right-click to pin" + (col.kind == "weight" ? ", hold or normalize." : ".");
        }
        default: return {};
        }
    }

    Qt::ItemFlags flags(const QModelIndex& i) const override
    {
        if (!i.isValid() || size_t(i.row()) >= s.rowMap.size() || size_t(i.column()) >= s.colMap.size())
            return Qt::NoItemFlags;
        Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (s.cellEditable(s.rowMap[size_t(i.row())], s.colMap[size_t(i.column())]))
            f |= Qt::ItemIsEditable;
        return f;
    }

    bool setData(const QModelIndex& i, const QVariant& value, int role) override
    {
        if (role != Qt::EditRole || !i.isValid())
            return false;
        bool ok = false;
        const double v = value.toString().trimmed().toDouble(&ok);
        if (!ok)
            return false;
        // Typing over a selection sets every selected cell; otherwise just this one.
        std::vector<std::pair<int, int>> cells;
        if (s.isSelected(i))
            cells = s.selectedCells();
        else
            cells.push_back({s.rowMap[size_t(i.row())], s.colMap[size_t(i.column())]});
        if (!s.typeInto(cells, v))
            return false;
        // The editor is still closing: re-read once it has.
        QTimer::singleShot(0, s.self, [st = &s] { st->refresh(true); });
        return true;
    }

private:
    ComponentSheetState& s;
};

class CsHeader final : public QHeaderView
{
public:
    CsHeader(ComponentSheetState& s, QWidget* parent) : QHeaderView(Qt::Horizontal, parent), s(s)
    {
        setSectionsClickable(true);
        setHighlightSections(true);
        setContextMenuPolicy(Qt::CustomContextMenu);
    }

protected:
    void paintSection(QPainter* p, const QRect& rect, int logicalIndex) const override
    {
        QHeaderView::paintSection(p, rect, logicalIndex);
        if (logicalIndex >= 0 && size_t(logicalIndex) < s.colMap.size() && !s.highlighted.empty() &&
            s.data.cols[size_t(s.colMap[size_t(logicalIndex)])].id == s.highlighted)
            p->fillRect(QRect(rect.left(), rect.bottom() - 1, rect.width(), 2), themeColor("accent"));
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::MiddleButton)
        {
            const int section = logicalIndexAt(e->position().toPoint());
            if (section >= 0)
                s.beginScrub(section, e->position().toPoint());
            e->accept();
            return;
        }
        QHeaderView::mousePressEvent(e);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (s.scrub)
        {
            s.scrubTo(e->position().toPoint(), e->modifiers());
            e->accept();
            return;
        }
        QHeaderView::mouseMoveEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::MiddleButton)
        {
            s.endScrub();
            e->accept();
            return;
        }
        QHeaderView::mouseReleaseEvent(e);
    }

private:
    ComponentSheetState& s;
};

class CsTable final : public QTableView
{
public:
    CsTable(ComponentSheetState& s, QWidget* parent) : QTableView(parent), s(s) {}

    bool editing() const { return state() == QAbstractItemView::EditingState; }

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->matches(QKeySequence::Copy))
        {
            QApplication::clipboard()->setText(s.copyText());
            return;
        }
        if (e->matches(QKeySequence::Paste))
        {
            const QModelIndex at = currentIndex();
            if (at.isValid())
                s.pasteAt(at.row(), at.column(), QApplication::clipboard()->text());
            return;
        }
        QTableView::keyPressEvent(e);
    }

    QModelIndex moveCursor(CursorAction action, Qt::KeyboardModifiers mods) override
    {
        const QModelIndex at = currentIndex();
        if ((mods & Qt::ControlModifier) && at.isValid() && model())
        {
            const int lastRow = model()->rowCount() - 1, lastCol = model()->columnCount() - 1;
            switch (action)
            {
            case MoveLeft: return model()->index(at.row(), 0);
            case MoveRight: return model()->index(at.row(), lastCol);
            case MoveUp: return model()->index(0, at.column());
            case MoveDown: return model()->index(lastRow, at.column());
            default: break;
            }
        }
        return QTableView::moveCursor(action, mods);
    }

private:
    ComponentSheetState& s;
};

/** Every shown column as a tick (pinned and highlighted ones in accent) with the scrolled-to range boxed; click to scroll. */
class CsMiniMap final : public QWidget
{
public:
    CsMiniMap(ComponentSheetState& s, QWidget* parent) : QWidget(parent), s(s)
    {
        setFixedHeight(6);
        setCursor(Qt::PointingHandCursor);
        setToolTip("All columns; the box is what the table shows. Click to scroll there.");
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const int n = int(s.colMap.size());
        if (n == 0 || !s.header)
            return;
        QPainter p(this);
        const double w = width();
        QColor dim = themeColor("dimText");
        dim.setAlpha(140);
        for (int i = 0; i < n; ++i)
        {
            const CsColumn& col = s.data.cols[size_t(s.colMap[size_t(i)])];
            const bool lit = col.id == s.highlighted || std::ranges::find(s.pinned, col.id) != s.pinned.end();
            const int x = int((i + 0.5) * w / n);
            p.fillRect(QRect(x, 1, 1, height() - 2), lit ? themeColor("accent") : dim);
        }
        int first = s.header->visualIndexAt(0);
        int last = s.header->visualIndexAt(s.header->viewport()->width() - 1);
        if (first < 0)
            first = 0;
        if (last < 0)
            last = n - 1;
        QColor box = themeColor("text");
        box.setAlpha(150);
        p.setPen(box);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(first * w / n, 0.5, (last - first + 1) * w / n - 1.0, height() - 1.0));
    }

    void mousePressEvent(QMouseEvent* e) override { jump(e->position().x()); }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (e->buttons() & Qt::LeftButton)
            jump(e->position().x());
    }

private:
    void jump(double x)
    {
        const int n = int(s.colMap.size());
        if (n > 0 && width() > 0)
            s.scrollToColumn(std::clamp(int(x / width() * n), 0, n - 1));
    }

    ComponentSheetState& s;
};

QToolButton* csToggle(QWidget* parent, const QString& text, const QString& tip, bool on)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setChecked(on);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setAutoRaise(true);
    return b;
}
} // namespace

// ------------------------------------------------------------------------------------------------ state

void ComponentSheetState::fetch(bool keepRows)
{
    json call = {{"columns", {"allWeights", "position", "color"}}};
    if (keepRows)
    {
        std::vector<std::string> meshes;
        json vertices = json::array();
        for (const CsRow& r : data.rows)
        {
            if (std::ranges::find(meshes, r.mesh) == meshes.end())
                meshes.push_back(r.mesh);
            vertices.push_back({r.mesh, r.id});
        }
        call["args"] = meshes;
        call["vertices"] = vertices;
    }
    json j;
    try
    {
        j = ops::execute("componentValues", call, CallSource::Script);
    }
    catch (const std::exception&)
    {
    }
    CsData next;
    if (j.is_object())
    {
        for (const json& r : j["rows"])
            next.rows.push_back({r[0].get<std::string>(), r[1].get<uint32_t>()});
        for (const json& c : j["columns"])
        {
            CsColumn col;
            col.id = c.value("id", "");
            col.label = c.value("label", "");
            col.kind = c.value("kind", "");
            col.joint = c.value("joint", "");
            col.held = c.value("held", false);
            col.total = c.value("total", 0.0);
            if (c.contains("links"))
                for (const json& l : c["links"])
                    col.links.push_back(l.get<std::string>());
            next.cols.push_back(std::move(col));
        }
        for (const json& line : j["values"])
        {
            std::vector<double> v;
            v.reserve(line.size());
            for (const json& x : line)
                v.push_back(x.is_number() ? x.get<double>() : kNoValue);
            next.values.push_back(std::move(v));
        }
    }
    data = std::move(next);

    Graph& g = Graph::current();
    lead.clear();
    const scene::NodeId leadId = attrView::lead();
    if (leadId != scene::kNoNode)
        if (scene::Node* n = g.node(leadId))
            lead = n->path();
    if (std::ranges::none_of(data.rows, [&](const CsRow& r) { return r.mesh == lead; }))
        lead = data.rows.empty() ? std::string() : data.rows[0].mesh;
    multiMesh = std::ranges::any_of(data.rows, [&](const CsRow& r) { return r.mesh != data.rows[0].mesh; });
}

void ComponentSheetState::layout()
{
    QStringList tokens;
    for (const QString& t : search.split(QRegularExpression("[\\s,]+"), Qt::SkipEmptyParts))
        tokens << t.toLower();
    auto matches = [&](const CsColumn& c) {
        if (tokens.isEmpty())
            return true;
        const QString label = QString::fromStdString(c.label).toLower();
        for (const QString& t : tokens)
        {
            if (c.kind == "position" && (QString("position").startsWith(t) || t == "pos"))
                return true;
            if (c.kind == "color" && (QString("colour").startsWith(t) || QString("color").startsWith(t)))
                return true;
            if (c.kind == "weight" && label.contains(t))
                return true;
        }
        return false;
    };
    auto kindShown = [&](const CsColumn& c) {
        return c.kind == "weight" ? showWeights : c.kind == "position" ? showPosition : showColor;
    };
    auto anyNonZero = [&](int c) {
        return std::ranges::any_of(data.values, [c](const std::vector<double>& line) {
            const double v = line[size_t(c)];
            return !std::isnan(v) && v != 0.0;
        });
    };

    std::vector<int> pinnedCols, weightCols, otherCols;
    more = 0;
    for (int c = 0; c < int(data.cols.size()); ++c)
    {
        const CsColumn& col = data.cols[size_t(c)];
        if (!kindShown(col))
            continue;
        if (std::ranges::find(pinned, col.id) != pinned.end())
        {
            pinnedCols.push_back(c);
            continue;
        }
        if (col.kind != "weight")
        {
            if (matches(col))
                otherCols.push_back(c);
            continue;
        }
        if (col.total <= 0.0 && !showAll && col.id != highlighted && tokens.isEmpty())
        {
            ++more;
            continue;
        }
        if (!matches(col) || (hideZeroCols && !anyNonZero(c)))
            continue;
        weightCols.push_back(c);
    }
    std::ranges::sort(pinnedCols, [&](int a, int b) {
        return std::ranges::find(pinned, data.cols[size_t(a)].id) < std::ranges::find(pinned, data.cols[size_t(b)].id);
    });
    colMap = pinnedCols;
    colMap.insert(colMap.end(), weightCols.begin(), weightCols.end());
    colMap.insert(colMap.end(), otherCols.begin(), otherCols.end());

    rowMap.clear();
    const bool weightsShown = std::ranges::any_of(colMap, [&](int c) { return data.cols[size_t(c)].kind == "weight"; });
    for (int r = 0; r < int(data.rows.size()); ++r)
    {
        if (hideZeroRows)
        {
            bool zero = true;
            for (int c : colMap)
            {
                if (weightsShown && data.cols[size_t(c)].kind != "weight")
                    continue;
                const double v = data.values[size_t(r)][size_t(c)];
                if (!std::isnan(v) && v != 0.0)
                {
                    zero = false;
                    break;
                }
            }
            if (zero)
                continue;
        }
        rowMap.push_back(r);
    }
}

/** layout() after a view setting changed, then the model, the "+N more" button and the mini-map. */
void ComponentSheetState::relayout()
{
    layout();
    model->reset();
    moreButton->setText(QString("+%1 more").arg(more));
    moreButton->setVisible(more > 0 && !showAll);
    map->update();
}

void ComponentSheetState::refresh(bool force)
{
    Graph& g = Graph::current();
    const uint64_t serial = scene::components::serial();
    const bool multi = attrView::multiEditHeld();
    const bool same = &g == lastGraph && g.version() == lastVersion && serial == lastSerial && g.selection() == lastSelection;
    if (!force && (scrub || (same && multi == lastMulti) || table->editing()))
        return;
    if (!force && same)
    {
        // Only Alt changed: what is editable and dimmed.
        lastMulti = multi;
        model->changed();
        return;
    }
    lastGraph = &g;
    lastVersion = g.version();
    lastSerial = serial;
    lastMulti = multi;
    lastSelection = g.selection();

    std::vector<std::string> oldCols;
    for (int c : colMap)
        oldCols.push_back(data.cols[size_t(c)].id);
    std::vector<std::pair<std::string, uint32_t>> oldRows;
    for (int r : rowMap)
        oldRows.push_back({data.rows[size_t(r)].mesh, data.rows[size_t(r)].id});

    fetch(serial == ownSerial && !data.rows.empty());

    // A selected joint (Outliner or viewport) lights its column and scrolls to it.
    std::string lit;
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id))
        {
            const std::string path = n->path();
            for (const CsColumn& c : data.cols)
                if (c.kind == "weight" && !c.joint.empty() && c.joint == path)
                    lit = c.id;
            if (!lit.empty())
                break;
        }
    const bool newLight = !lit.empty() && lit != highlighted;
    highlighted = lit;

    layout();
    std::vector<std::string> cols;
    for (int c : colMap)
        cols.push_back(data.cols[size_t(c)].id);
    std::vector<std::pair<std::string, uint32_t>> rows;
    for (int r : rowMap)
        rows.push_back({data.rows[size_t(r)].mesh, data.rows[size_t(r)].id});
    if (cols == oldCols && rows == oldRows)
        model->changed();
    else
        model->reset();
    moreButton->setText(QString("+%1 more").arg(more));
    moreButton->setVisible(more > 0 && !showAll);
    map->update();
    header->viewport()->update();
    if (newLight)
        scrollToColumn(shownColumnOf(highlighted));
}

int ComponentSheetState::shownColumnOf(const std::string& id) const
{
    for (size_t i = 0; i < colMap.size(); ++i)
        if (data.cols[size_t(colMap[i])].id == id)
            return int(i);
    return -1;
}

void ComponentSheetState::scrollToColumn(int shownCol)
{
    if (shownCol < 0 || !header)
        return;
    const int x = header->sectionPosition(shownCol) + header->sectionSize(shownCol) / 2 - table->viewport()->width() / 2;
    table->horizontalScrollBar()->setValue(std::max(0, x));
    map->update();
}

bool ComponentSheetState::cellEditable(int r, int c) const
{
    const double v = data.values[size_t(r)][size_t(c)];
    if (std::isnan(v))
        return false;
    const CsColumn& col = data.cols[size_t(c)];
    if (col.kind == "weight" && col.held)
        return false;
    return data.rows[size_t(r)].mesh == lead || attrView::multiEditHeld();
}

bool ComponentSheetState::isSelected(const QModelIndex& i) const { return table && table->selectionModel()->isSelected(i); }

std::vector<std::pair<int, int>> ComponentSheetState::selectedCells() const
{
    std::vector<std::pair<int, int>> out;
    for (const QModelIndex& i : table->selectionModel()->selectedIndexes())
        if (size_t(i.row()) < rowMap.size() && size_t(i.column()) < colMap.size())
            out.push_back({rowMap[size_t(i.row())], colMap[size_t(i.column())]});
    return out;
}

/** The data rows a column action works on: the rows selected in that column, else every shown row. */
std::vector<int> ComponentSheetState::rowsForColumn(int shownCol) const
{
    std::set<int> rows;
    for (const QModelIndex& i : table->selectionModel()->selectedIndexes())
        if (i.column() == shownCol && size_t(i.row()) < rowMap.size())
            rows.insert(rowMap[size_t(i.row())]);
    if (rows.empty())
        return rowMap;
    return {rows.begin(), rows.end()};
}

bool ComponentSheetState::apply(const std::vector<std::pair<int, int>>& cells, const std::vector<double>& values, bool relative, const char* label)
{
    // One setComponentValues per column, all in one undo step.
    std::map<int, std::pair<json, json>> byCol;
    std::vector<std::string> meshes;
    for (size_t k = 0; k < cells.size(); ++k)
    {
        auto [r, c] = cells[k];
        const CsRow& row = data.rows[size_t(r)];
        auto& [verts, vals] = byCol[c];
        verts.push_back({row.mesh, row.id});
        vals.push_back(values[k]);
        if (std::ranges::find(meshes, row.mesh) == meshes.end())
            meshes.push_back(row.mesh);
    }
    if (byCol.empty())
        return false;
    try
    {
        UndoChunk chunk(label);
        for (auto& [c, e] : byCol)
            ops::execute("setComponentValues",
                         {{"args", meshes}, {"column", data.cols[size_t(c)].id}, {"vertices", e.first}, {"values", e.second}, {"relative", relative}},
                         scrub ? CallSource::Script : CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
        return false;
    }
    if (meshes.size() > 1)
        attrView::noteMultiEditUsed();
    return true;
}

bool ComponentSheetState::typeInto(const std::vector<std::pair<int, int>>& cells, double v)
{
    std::vector<std::pair<int, int>> ok;
    for (auto [r, c] : cells)
        if (cellEditable(r, c))
            ok.push_back({r, c});
    return apply(ok, std::vector<double>(ok.size(), v), false, "Set Component Values");
}

bool ComponentSheetState::pasteAt(int row, int col, const QString& text)
{
    std::vector<std::vector<double>> block;
    for (QString line : text.split('\n'))
    {
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        std::vector<double> nums;
        for (const QString& part : line.split(QRegularExpression("[\\t,; ]+"), Qt::SkipEmptyParts))
        {
            bool ok = false;
            const double v = part.toDouble(&ok);
            if (!ok)
                return false;
            nums.push_back(v);
        }
        block.push_back(std::move(nums));
    }
    if (block.empty())
        return false;
    std::vector<std::pair<int, int>> cells;
    std::vector<double> values;
    // One number pasted over a selection fills it.
    if (block.size() == 1 && block[0].size() == 1 && table && table->selectionModel()->selectedIndexes().size() > 1)
    {
        for (auto [r, c] : selectedCells())
            if (cellEditable(r, c))
            {
                cells.push_back({r, c});
                values.push_back(block[0][0]);
            }
    }
    else
    {
        for (size_t i = 0; i < block.size(); ++i)
            for (size_t j = 0; j < block[i].size(); ++j)
            {
                const size_t sr = size_t(row) + i, sc = size_t(col) + j;
                if (sr >= rowMap.size() || sc >= colMap.size())
                    continue;
                const int r = rowMap[sr], c = colMap[sc];
                if (!cellEditable(r, c))
                    continue;
                cells.push_back({r, c});
                values.push_back(block[i][j]);
            }
    }
    return apply(cells, values, false, "Paste Component Values");
}

QString ComponentSheetState::copyText() const
{
    const QModelIndexList sel = table->selectionModel()->selectedIndexes();
    if (sel.isEmpty())
        return {};
    int r0 = INT_MAX, r1 = -1, c0 = INT_MAX, c1 = -1;
    for (const QModelIndex& i : sel)
    {
        r0 = std::min(r0, i.row());
        r1 = std::max(r1, i.row());
        c0 = std::min(c0, i.column());
        c1 = std::max(c1, i.column());
    }
    QStringList lines;
    for (int r = r0; r <= r1; ++r)
    {
        QStringList cells;
        for (int c = c0; c <= c1; ++c)
            cells << (table->selectionModel()->isSelected(model->index(r, c)) ? model->data(model->index(r, c), Qt::DisplayRole).toString() : QString());
        lines << cells.join('\t');
    }
    return lines.join('\n');
}

void ComponentSheetState::syncSelection()
{
    const QModelIndexList rows = table->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return;
    std::vector<std::pair<std::string, json>> perMesh;
    for (const QModelIndex& i : rows)
    {
        if (size_t(i.row()) >= rowMap.size())
            continue;
        const CsRow& row = data.rows[size_t(rowMap[size_t(i.row())])];
        auto it = std::ranges::find_if(perMesh, [&](const auto& e) { return e.first == row.mesh; });
        if (it == perMesh.end())
        {
            perMesh.push_back({row.mesh, json::array()});
            it = perMesh.end() - 1;
        }
        it->second.push_back(row.id);
    }
    try
    {
        UndoChunk chunk("Select Components");
        bool first = true;
        for (auto& [mesh, ids] : perMesh)
        {
            ops::execute("selectComponents", {{"args", {mesh}}, {"vertices", ids}, {"add", !first}}, CallSource::UI);
            first = false;
        }
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
        return;
    }
    ownSerial = scene::components::serial();
}

void ComponentSheetState::setHeld(int c, bool held)
{
    const CsColumn& col = data.cols[size_t(c)];
    if (col.kind != "weight" || col.links.empty())
        return;
    try
    {
        UndoChunk chunk(held ? "Hold Influence" : "Release Influence");
        for (const std::string& link : col.links)
            ops::execute("setAttr", {{"args", {link + ".lockWeights", held}}}, CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
    }
    refresh(true);
}

void ComponentSheetState::normalize(int shownCol)
{
    if (shownCol < 0 || size_t(shownCol) >= colMap.size())
        return;
    const int c = colMap[size_t(shownCol)];
    std::vector<std::pair<int, int>> cells;
    std::vector<double> values;
    for (int r : rowsForColumn(shownCol))
        if (cellEditable(r, c))
        {
            cells.push_back({r, c});
            values.push_back(data.values[size_t(r)][size_t(c)]);
        }
    // Setting a column to what it holds rescales the other unheld influences of each row to fill the rest.
    if (apply(cells, values, false, "Normalize Weights"))
        refresh(true);
}

void ComponentSheetState::togglePin(const std::string& id, bool on)
{
    std::erase(pinned, id);
    if (on)
        pinned.push_back(id);
    relayout();
}

void ComponentSheetState::columnMenu(int shownCol, QPoint global)
{
    if (shownCol < 0 || size_t(shownCol) >= colMap.size())
        return;
    const int c = colMap[size_t(shownCol)];
    const CsColumn col = data.cols[size_t(c)];
    const bool isPinned = std::ranges::find(pinned, col.id) != pinned.end();
    QMenu menu(table);
    menu.addAction(icon(isPinned ? "pinned-off" : "pin"), isPinned ? "Unpin" : "Pin Left", [this, id = col.id, isPinned] { togglePin(id, !isPinned); });
    if (col.kind == "weight")
    {
        menu.addAction(icon(col.held ? "lock-open" : "lock"), col.held ? "Release" : "Hold", [this, c, held = col.held] { setHeld(c, !held); })
            ->setToolTip("Hold keeps this influence's weights while others are edited or normalized (lockWeights).");
        QAction* norm = menu.addAction("Normalize", [this, shownCol] { normalize(shownCol); });
        norm->setEnabled(!col.held);
        norm->setToolTip("Rescale the other unheld influences of the selected rows (all shown when none) so each sums to 1.");
    }
    menu.exec(global);
}

void ComponentSheetState::beginScrub(int shownCol, QPoint at)
{
    if (size_t(shownCol) >= colMap.size())
        return;
    CsScrub sc;
    sc.col = colMap[size_t(shownCol)];
    sc.last = at;
    for (int r : rowsForColumn(shownCol))
        if (cellEditable(r, sc.col))
        {
            sc.cells.push_back({r, sc.col});
            sc.start.push_back(data.values[size_t(r)][size_t(sc.col)]);
        }
    if (!sc.cells.empty())
        scrub = std::move(sc);
}

void ComponentSheetState::scrubTo(QPoint at, Qt::KeyboardModifiers mods)
{
    // The release can go missing (focus stolen mid-drag); a move with no button held ends the drag.
    if (QApplication::mouseButtons() == Qt::NoButton)
    {
        endScrub();
        return;
    }
    CsScrub& sc = *scrub;
    const int dx = at.x() - sc.last.x();
    sc.last = at;
    if (dx == 0)
        return;
    const bool weight = data.cols[size_t(sc.col)].kind == "weight";
    const double base = (weight ? 0.01 : 0.1) * ((mods & Qt::ControlModifier) ? 0.1 : (mods & Qt::ShiftModifier) ? 10.0 : 1.0);
    sc.accum += dx * attrView::scrubStep(base, dx);
    UndoStack& u = UndoStack::get();
    if (!sc.open)
    {
        u.openChunk(weight ? "Scrub Weights" : "Scrub Component Values");
        sc.open = true;
        sc.mark = u.openEdits();
    }
    // Each step re-applies from the start values, so renormalising never compounds.
    u.rollbackTo(sc.mark);
    std::vector<double> values;
    for (double v : sc.start)
        values.push_back(weight ? std::clamp(v + sc.accum, 0.0, 1.0) : v + sc.accum);
    apply(sc.cells, values, false, "Scrub");
    refresh(true);
}

void ComponentSheetState::endScrub()
{
    if (!scrub)
        return;
    const bool open = scrub->open;
    scrub.reset();
    if (open)
        UndoStack::get().closeChunk();
    refresh(true);
}

// ------------------------------------------------------------------------------------------------ widget

ComponentSheet::ComponentSheet(QWidget* parent) : QWidget(parent), d(std::make_unique<ComponentSheetState>())
{
    ComponentSheetState& s = *d;
    ComponentSheetState* st = d.get();
    s.self = this;
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(3);

    auto* bar = new QHBoxLayout();
    bar->setSpacing(2);
    s.searchField = new QLineEdit(this);
    s.searchField->setObjectName("componentSheetSearch");
    s.searchField->setPlaceholderText("Columns: type a joint, 'pos', 'colour'");
    s.searchField->setClearButtonEnabled(true);
    s.searchField->addAction(icon("search"), QLineEdit::LeadingPosition);
    bar->addWidget(s.searchField, 1);
    auto chip = [&](const QString& text, const QString& tip, bool& flag) {
        QToolButton* b = csToggle(this, text, tip, flag);
        QObject::connect(b, &QToolButton::toggled, this, [st, f = &flag](bool on) {
            *f = on;
            st->relayout();
        });
        bar->addWidget(b);
        return b;
    };
    chip("Weights", "Skin weight columns, one per influence.", s.showWeights);
    chip("Position", "Position X, Y and Z in object space, on the mesh under its modifiers.", s.showPosition);
    chip("Colour", "The current colour set's R, G, B and A, averaged where a vertex's faces differ; setting one writes all its faces.",
         s.showColor);
    bar->addSpacing(8);
    s.hideRowsButton = chip("Hide zero rows", "Hide vertices with no weight in the shown columns.", s.hideZeroRows);
    s.hideColsButton = chip("Hide zero columns", "Hide influences with no weight on these vertices.", s.hideZeroCols);
    s.showAllButton = chip("Show all influences", "Also list the influences with no weight on these vertices.", s.showAll);
    s.moreButton = new QToolButton(this);
    s.moreButton->setAutoRaise(true);
    s.moreButton->setToolTip("Influences with no weight on these vertices: click to list them.");
    s.moreButton->setVisible(false);
    QObject::connect(s.moreButton, &QToolButton::clicked, this, [st] { st->showAllButton->setChecked(true); });
    bar->addWidget(s.moreButton);
    v->addLayout(bar);

    s.map = new CsMiniMap(s, this);
    v->addWidget(s.map);

    s.table = new CsTable(s, this);
    s.table->setObjectName("componentSheetTable");
    s.header = new CsHeader(s, s.table);
    s.table->setHorizontalHeader(s.header);
    s.model = new CsModel(s);
    s.model->setParent(this);
    s.table->setModel(s.model);
    s.header->setSectionResizeMode(QHeaderView::Interactive);
    s.header->setDefaultSectionSize(64);
    s.table->verticalHeader()->setDefaultSectionSize(20);
    s.table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    s.table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    s.table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    s.table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    s.table->setEditTriggers(QAbstractItemView::AnyKeyPressed | QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    s.table->setWordWrap(false);
    s.table->setCornerButtonEnabled(true);
    setEmptyHint(s.table, "Select vertices to list their skin weights and positions.");
    v->addWidget(s.table, 1);

    QObject::connect(s.searchField, &QLineEdit::textChanged, this, [st](const QString& t) {
        st->search = t;
        st->relayout();
    });
    QObject::connect(s.header, &QHeaderView::customContextMenuRequested, this,
                     [st](QPoint at) { st->columnMenu(st->header->logicalIndexAt(at), st->header->mapToGlobal(at)); });
    QObject::connect(s.table->selectionModel(), &QItemSelectionModel::selectionChanged, this, [st] { st->syncSelection(); });
    QObject::connect(s.table->horizontalScrollBar(), &QScrollBar::valueChanged, this, [st] { st->map->update(); });
    QObject::connect(s.header, &QHeaderView::sectionResized, this, [st] { st->map->update(); });

    s.attrListener = attrView::addListener([st] {
        if (st->model)
            st->model->changed();
    });
    // Follows the selection, the scene and Alt while shown; cheap when nothing changed.
    auto* timer = new QTimer(this);
    timer->setInterval(150);
    QObject::connect(timer, &QTimer::timeout, this, [this] {
        if (isVisible())
            d->refresh(false);
    });
    timer->start();
}

ComponentSheet::~ComponentSheet()
{
    attrView::removeListener(d->attrListener);
    if (d->scrub && d->scrub->open)
        UndoStack::get().closeChunk();
}

void ComponentSheet::refresh() { d->refresh(true); }

std::vector<std::string> ComponentSheet::columnIds() const
{
    std::vector<std::string> out;
    for (int c : d->colMap)
        out.push_back(d->data.cols[size_t(c)].id);
    return out;
}

int ComponentSheet::moreCount() const { return d->more; }
int ComponentSheet::rowCount() const { return int(d->rowMap.size()); }

QString ComponentSheet::rowLabel(int row) const { return d->model->headerData(row, Qt::Vertical, Qt::DisplayRole).toString(); }

double ComponentSheet::value(int row, int column) const
{
    if (row < 0 || column < 0 || size_t(row) >= d->rowMap.size() || size_t(column) >= d->colMap.size())
        return kNoValue;
    return d->data.values[size_t(d->rowMap[size_t(row)])][size_t(d->colMap[size_t(column)])];
}

void ComponentSheet::setSearch(const QString& text) { d->searchField->setText(text); }
void ComponentSheet::setPinned(const std::string& columnId, bool pinned) { d->togglePin(columnId, pinned); }
void ComponentSheet::setHideZeroRows(bool on) { d->hideRowsButton->setChecked(on); }
void ComponentSheet::setHideZeroColumns(bool on) { d->hideColsButton->setChecked(on); }
void ComponentSheet::setShowAllInfluences(bool on) { d->showAllButton->setChecked(on); }

void ComponentSheet::setHeld(const std::string& columnId, bool held)
{
    for (size_t c = 0; c < d->data.cols.size(); ++c)
        if (d->data.cols[c].id == columnId)
            d->setHeld(int(c), held);
}

void ComponentSheet::normalizeColumn(const std::string& columnId) { d->normalize(d->shownColumnOf(columnId)); }

bool ComponentSheet::typeValue(const std::vector<std::pair<int, int>>& cells, const QString& text)
{
    bool ok = false;
    const double v = text.trimmed().toDouble(&ok);
    if (!ok)
        return false;
    std::vector<std::pair<int, int>> targets;
    for (auto [r, c] : cells)
        if (r >= 0 && c >= 0 && size_t(r) < d->rowMap.size() && size_t(c) < d->colMap.size())
            targets.push_back({d->rowMap[size_t(r)], d->colMap[size_t(c)]});
    const bool done = d->typeInto(targets, v);
    d->refresh(true);
    return done;
}

bool ComponentSheet::paste(int row, int column, const QString& text)
{
    const bool done = d->pasteAt(row, column, text);
    d->refresh(true);
    return done;
}

QTableView* ComponentSheet::table() const { return d->table; }

void registerComponentSheet()
{
    panels::add({.id = "componentSheet",
                 .title = "Component Spreadsheet",
                 .create = [](QWidget* p) -> QWidget* { return new ComponentSheet(p); },
                 .area = Qt::BottomDockWidgetArea,
                 .icon = "table-column",
                 .open = false,
                 .toggleAction = "window.componentSheet",
                 .floatFirst = true});
}
} // namespace lila::ui
