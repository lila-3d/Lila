#pragma once

#include "core/Export.h"

#include <QWidget>

#include <vector>

class QLineEdit;
class QPushButton;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

namespace lila::ui
{
/**
 * Searchable reference for every command in the registry and the lila Python package, built live: commands from
 * cmds::all(), Python by walking the modules in the embedded interpreter (lila._apidoc). Built on first show and on
 * Refresh, so extension commands appear once their extension has loaded.
 */
class LILA_UI_API ApiReference : public QWidget
{
    Q_OBJECT

public:
    explicit ApiReference(QWidget* parent = nullptr);

    /** Rebuilds the index from the registry and the interpreter. */
    void rebuild();

public slots:
    /** Finds and shows a symbol: "parent", "lila.ops.parent", "lila.math.Vector.get_safe_normal", "Node.translate". */
    bool showSymbol(const QString& symbol);

signals:
    /** "Insert Example" on an entry with an example, for the script editor. */
    void insertCode(const QString& code);

protected:
    void showEvent(QShowEvent* e) override;

private:
    struct Entry
    {
        QString key;  // fully qualified: lila.ops.parent, lila.nodes.Node.translate
        QString name; // short name shown in the tree
        QString kind; // command, module, class, function, method, staticmethod, classmethod, property, attribute
        QStringList signatures;
        QString doc;
        QString example;
        QString extraHtml; // flag table, member list
        QString footer;
        int parent = -1;
        std::vector<int> children;
    };

    int add(Entry e);
    void addCommands();
    void addPython();
    void populateTree();
    void runSearch(const QString& text);
    /** note: HTML shown above the entry. */
    void display(int index, const QString& note = {});
    int find(const QString& symbol) const;
    QString renderDoc(const QString& doc) const;
    QString membersHtml(const Entry& e) const;
    static int fuzzyScore(const QString& query, const QString& candidate);

    std::vector<Entry> mEntries;
    QLineEdit* mSearch;
    QTreeWidget* mTree;
    QTextBrowser* mView;
    QPushButton* mCopySig;
    QPushButton* mInsert;
    int mCurrent = -1;
    bool mBuilt = false;
};
} // namespace lila::ui
