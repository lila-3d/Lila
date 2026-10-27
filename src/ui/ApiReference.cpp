#include "ui/ApiReference.h"

#include "ops/Command.h"
#include "core/Profile.h"
#include "python/Python.h"
#include "ui/PythonBridge.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cstring>
#include <functional>
#include <map>

namespace lila::ui
{
namespace
{
constexpr int kEntryRole = Qt::UserRole + 1;
constexpr int kMaxResults = 300;

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

QString esc(const QString& s)
{
    return s.toHtmlEscaped();
}

const char* typeName(ArgType t)
{
    switch (t)
    {
    case ArgType::Bool:
        return "bool";
    case ArgType::Int:
        return "int";
    case ArgType::Float:
        return "float";
    case ArgType::String:
        return "str";
    case ArgType::Vec3:
        return "vec3";
    case ArgType::List:
        return "list";
    case ArgType::Any:
        return "any";
    }
    return "any";
}

QString pyValue(const json& v)
{
    if (v.is_null())
        return "None";
    if (v.is_boolean())
        return v.get<bool>() ? "True" : "False";
    if (v.is_string())
        return qs(json(v.get<std::string>()).dump());
    if (v.is_array())
    {
        QStringList items;
        for (const json& i : v)
            items << pyValue(i);
        return "[" + items.join(", ") + "]";
    }
    return qs(v.dump());
}

bool isCodeLine(const QString& line)
{
    return line.startsWith("    ") || line.startsWith('\t') || line.trimmed().startsWith(">>>");
}

QString dedent(const QStringList& lines)
{
    int indent = INT_MAX;
    for (const QString& l : lines)
        if (!l.trimmed().isEmpty())
            indent = std::min(indent, int(l.size() - QString(l).remove(QRegularExpression("^\\s+")).size()));
    QStringList out;
    for (const QString& l : lines)
        out << (l.trimmed().isEmpty() ? QString() : l.mid(indent == INT_MAX ? 0 : indent));
    while (!out.isEmpty() && out.back().isEmpty())
        out.pop_back();
    return out.join('\n');
}

/** First indented or >>> block of a docstring, as runnable source. */
QString firstExample(const QString& doc)
{
    QStringList lines = doc.split('\n');
    for (int i = 0; i < lines.size(); ++i)
    {
        if (!isCodeLine(lines[i]))
            continue;
        QStringList block;
        while (i < lines.size() && (isCodeLine(lines[i]) || lines[i].trimmed().isEmpty()))
            block << lines[i++];
        QString code = dedent(block);
        code.replace(QRegularExpression("^(>>>|\\.\\.\\.) ?", QRegularExpression::MultilineOption), "");
        return code;
    }
    return {};
}

QString inlineMarkup(const QString& text)
{
    QString html = esc(text);
    static const QRegularExpression ticks("`([^`]+)`");
    html.replace(ticks, "<code>\\1</code>");
    return html;
}

QString codeBlock(const QString& code, const QString& cls = "code")
{
    return "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"6\" class=\"" + cls + "\"><tr><td><pre>" + esc(code) +
           "</pre></td></tr></table>";
}

QString firstLine(const QString& doc)
{
    QString line = doc.section('\n', 0, 0).trimmed();
    return line.size() > 110 ? line.left(107) + "..." : line;
}
} // namespace

ApiReference::ApiReference(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* split = new QSplitter(Qt::Horizontal, this);
    layout->addWidget(split);

    auto* left = new QWidget(split);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(4, 4, 0, 4);
    leftLayout->setSpacing(4);
    mSearch = new QLineEdit(left);
    mSearch->setPlaceholderText("Search commands and Python API");
    mSearch->setClearButtonEnabled(true);
    leftLayout->addWidget(mSearch);
    mTree = new QTreeWidget(left);
    mTree->setColumnCount(2);
    mTree->setHeaderHidden(true);
    mTree->setUniformRowHeights(true);
    mTree->header()->setStretchLastSection(true);
    mTree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    leftLayout->addWidget(mTree);

    auto* right = new QWidget(split);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 4, 4, 4);
    rightLayout->setSpacing(4);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(4);
    mCopySig = new QPushButton("Copy Signature", right);
    mInsert = new QPushButton("Insert Example", right);
    mInsert->setToolTip("Insert the example into the script editor");
    auto* refresh = new QPushButton("Refresh", right);
    refresh->setToolTip("Rebuild from the command registry and the Python package");
    bar->addWidget(mCopySig);
    bar->addWidget(mInsert);
    bar->addStretch(1);
    bar->addWidget(refresh);
    rightLayout->addLayout(bar);
    mView = new QTextBrowser(right);
    mView->setOpenLinks(false);
    rightLayout->addWidget(mView, 1);

    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({260, 560});

    QString mono = monoFont().family();
    mView->document()->setDefaultStyleSheet(themedStyleSheet(R"(
        body { color: #d4d4d4; }
        h2 { color: #f2f2f2; font-size: 15px; margin-bottom: 2px; }
        h3 { color: #bdbdbd; font-size: 12px; margin-top: 14px; margin-bottom: 4px; }
        .kind { color: #8a8a8a; font-size: 11px; font-weight: normal; }
        .muted { color: #8a8a8a; }
        .code, .sig { background-color: #2c2c2c; margin-top: 4px; margin-bottom: 4px; }
        .sig pre { color: #f0f0f0; }
        pre { font-family: '%1'; color: #dcdcdc; margin: 0; white-space: pre-wrap; }
        code { font-family: '%1'; color: #e8e8e8; background-color: #303030; }
        a { color: #e8e8e8; }
        td.f { font-family: '%1'; color: #ececec; }
        th { color: #9a9a9a; font-weight: normal; text-align: left; }
    )")
                                                .arg(mono));

    connect(mSearch, &QLineEdit::textChanged, this, &ApiReference::runSearch);
    connect(mSearch, &QLineEdit::returnPressed, this, [this] {
        if (mTree->topLevelItemCount() > 0)
            mTree->setCurrentItem(mTree->topLevelItem(0));
    });
    connect(mTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (item)
            display(item->data(0, kEntryRole).toInt());
    });
    connect(mView, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == "sym")
            showSymbol(url.path());
    });
    connect(mCopySig, &QPushButton::clicked, this, [this] {
        if (mCurrent >= 0)
            QApplication::clipboard()->setText(mEntries[mCurrent].signatures.join('\n'));
    });
    connect(mInsert, &QPushButton::clicked, this, [this] {
        if (mCurrent >= 0 && !mEntries[mCurrent].example.isEmpty())
            Q_EMIT insertCode(mEntries[mCurrent].example + "\n");
    });
    connect(refresh, &QPushButton::clicked, this, &ApiReference::rebuild);
    display(-1);
}

void ApiReference::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (!mBuilt)
        rebuild();
}

int ApiReference::add(Entry e)
{
    int index = int(mEntries.size());
    if (e.parent >= 0)
        mEntries[e.parent].children.push_back(index);
    mEntries.push_back(std::move(e));
    return index;
}

void ApiReference::rebuild()
{
    LILA_ZONE();

    QString keep = mCurrent >= 0 ? mEntries[mCurrent].key : QString();
    mEntries.clear();
    mCurrent = -1;
    addCommands();
    addPython();
    mBuilt = true;
    if (mSearch->text().isEmpty())
        populateTree();
    else
        runSearch(mSearch->text());
    if (keep.isEmpty() || !showSymbol(keep))
        display(-1);
}

void ApiReference::addCommands()
{
    LILA_ZONE();

    std::vector<const CommandDef*> defs = ops::all();
    std::sort(defs.begin(), defs.end(), [](const CommandDef* a, const CommandDef* b) { return a->name < b->name; });

    int root = add({.key = "lila.ops", .name = "Commands", .kind = "folder",
                    .doc = QString("%1 commands, each callable as lila.ops.name(*objects, flag=value) or by its bare name in the "
                                   "script editor. Flags take long or short names.")
                               .arg(defs.size())});
    std::map<std::string, int> extensionFolders;
    for (const CommandDef* def : defs)
    {
        int parent = root;
        if (!def->owner.empty())
        {
            auto [it, isNew] = extensionFolders.try_emplace(def->owner, -1);
            if (isNew)
                it->second = add({.key = "lila.ops.extension:" + qs(def->owner), .name = qs(def->owner), .kind = "folder",
                                  .doc = "Commands registered by the " + qs(def->owner) + " extension.", .parent = root});
            parent = it->second;
        }

        Entry e;
        e.key = "lila.ops." + qs(def->name);
        e.name = qs(def->name);
        e.kind = "command";
        e.doc = qs(def->doc);
        e.parent = parent;

        QStringList params;
        if (!def->positionalDoc.empty())
            params << qs(def->positionalDoc);
        for (const FlagDef& f : def->flags)
            params << qs(f.name) + "=" + pyValue(f.defaultValue);
        e.signatures << e.name + "(" + params.join(", ") + ")";

        if (!def->flags.empty())
        {
            QString t = "<h3>Flags</h3><table cellspacing=\"0\" cellpadding=\"3\" width=\"100%\">"
                        "<tr><th>Flag</th><th>Short</th><th>Type</th><th>Default</th><th>Description</th></tr>";
            for (const FlagDef& f : def->flags)
            {
                QString about = inlineMarkup(qs(f.doc));
                if (!f.choices.empty())
                {
                    QStringList quoted;
                    for (const std::string& c : f.choices)
                        quoted << "<code>" + esc(pyValue(c)) + "</code>";
                    about += " One of " + quoted.join(", ") + ".";
                }
                if (!f.example.empty())
                    about += "<br><code>" + esc(qs(f.example)) + "</code>";
                bool hasShort = !f.shortName.empty() && f.shortName != f.name;
                t += "<tr><td class=\"f\">" + esc(qs(f.name)) + "</td><td class=\"f\">" + (hasShort ? esc(qs(f.shortName)) : QString()) +
                     "</td><td class=\"muted\">" + typeName(f.type) + "</td><td class=\"f\">" +
                     (f.defaultValue.is_null() ? QString("<span class=\"muted\">unset</span>") : esc(pyValue(f.defaultValue))) +
                     "</td><td>" + about + "</td></tr>";
            }
            e.extraHtml = t + "</table>";
        }

        QStringList examples;
        for (const std::string& x : def->examples)
            examples << qs(x).trimmed();
        e.example = examples.isEmpty() ? firstExample(e.doc) : examples.join("\n\n");
        if (e.example.isEmpty())
        {
            QString arg;
            if (!def->positionalDoc.empty())
            {
                QString word = qs(def->positionalDoc).remove(QRegularExpression("[\\[\\]\\.]")).section(' ', 0, 0);
                arg = "\"" + word + "\"";
            }
            e.example = "ops." + e.name + "(" + arg + ")";
        }
        e.footer = (def->owner.empty() ? QString("Built-in command.") : "From the <b>" + esc(qs(def->owner)) + "</b> extension.") +
                   (def->undoable ? " Undoable." : " Not undoable.");
        add(std::move(e));
    }
}

void ApiReference::addPython()
{
    LILA_ZONE();

    int root = add({.key = "lila", .name = "Python", .kind = "folder", .doc = "The lila Python package, read from the running interpreter."});
    std::string text = pybridge::apiDocJson();
    if (text.empty())
    {
        mEntries[root].doc = python::initialized() ? "The Python walk failed; see the script output for the traceback."
                                                   : "Python is not running, so the Python API is unavailable.";
        return;
    }
    json modules = json::parse(text, nullptr, false);
    if (!modules.is_array())
        return;

    auto str = [](const json& j, const char* key) { return j.contains(key) && j[key].is_string() ? qs(j[key].get<std::string>()) : QString(); };

    std::function<void(const json&, int)> addNode = [&](const json& j, int parent) {
        Entry e;
        e.key = str(j, "qualname");
        e.name = str(j, "name");
        e.kind = str(j, "kind");
        e.doc = str(j, "doc");
        e.parent = parent;
        if (j.contains("signature") && j["signature"].is_array())
            for (const json& s : j["signature"])
                if (s.is_string())
                    e.signatures << qs(s.get<std::string>());
        if (e.kind == "attribute" && j.contains("value"))
            e.signatures << e.name + " = " + str(j, "value");
        if (e.kind == "property" && j.value("readonly", false))
            e.footer = "Read-only.";
        if (j.contains("bases") && j["bases"].is_array() && !j["bases"].empty())
        {
            QStringList bases;
            for (const json& b : j["bases"])
                bases << esc(qs(b.get<std::string>()));
            e.footer = "Inherits " + bases.join(", ") + ".";
        }
        e.example = firstExample(e.doc);
        int index = add(std::move(e));
        if (j.contains("members") && j["members"].is_array())
            for (const json& m : j["members"])
                addNode(m, index);
    };
    for (const json& m : modules)
        addNode(m, root);
}

QString ApiReference::membersHtml(const Entry& e) const
{
    if (e.children.empty())
        return {};
    QString html = "<h3>" + QString(e.kind == "folder" && e.key == "lila.ops" ? "Commands" : "Members") +
                   "</h3><table cellspacing=\"0\" cellpadding=\"2\">";
    for (int c : e.children)
    {
        const Entry& m = mEntries[c];
        html += "<tr><td class=\"f\"><a href=\"sym:" + esc(m.key) + "\">" + esc(m.name) + "</a></td><td class=\"muted\">" + esc(m.kind) +
                "</td><td>" + inlineMarkup(firstLine(m.doc)) + "</td></tr>";
    }
    return html + "</table>";
}

QString ApiReference::renderDoc(const QString& doc) const
{
    QString html;
    QStringList lines = doc.split('\n');
    for (int i = 0; i < lines.size();)
    {
        if (lines[i].trimmed().isEmpty())
        {
            ++i;
            continue;
        }
        if (isCodeLine(lines[i]))
        {
            QStringList block;
            while (i < lines.size() && (isCodeLine(lines[i]) || lines[i].trimmed().isEmpty()))
                block << lines[i++];
            html += codeBlock(dedent(block));
            continue;
        }
        QString para;
        while (i < lines.size() && !lines[i].trimmed().isEmpty() && !isCodeLine(lines[i]))
        {
            QString line = lines[i].trimmed();
            // Keep list items and "Heading:" lines on their own lines; wrapped prose joins up.
            bool own = line.startsWith("- ") || line.startsWith("* ") || (i > 0 && lines[i - 1].trimmed().endsWith(':'));
            if (!para.isEmpty())
                para += own ? "<br>" : " ";
            para += inlineMarkup(line);
            ++i;
        }
        html += "<p>" + para + "</p>";
    }
    return html;
}

void ApiReference::display(int index, const QString& note)
{
    mCurrent = index;
    mCopySig->setEnabled(index >= 0 && !mEntries[index].signatures.isEmpty());
    mInsert->setEnabled(index >= 0 && !mEntries[index].example.isEmpty());
    if (index < 0)
    {
        mView->setHtml("<p class=\"muted\">Pick a command or Python symbol, or search on the left. F1 on a name in the script "
                       "editor jumps here.</p>");
        return;
    }
    const Entry& e = mEntries[index];
    QString html = note + "<h2>" + esc(e.name) + " <span class=\"kind\">" + esc(e.kind == "folder" ? QString() : e.kind) + "</span></h2>";
    if (e.kind != "folder")
        html += "<div class=\"muted\">" + esc(e.key) + "</div>";
    for (const QString& sig : e.signatures)
        html += codeBlock(sig, "sig");
    html += renderDoc(e.doc);
    html += e.extraHtml;
    if (e.kind == "command")
        html += "<h3>Example</h3>" + codeBlock(e.example);
    html += membersHtml(e);
    if (!e.footer.isEmpty())
        html += "<p class=\"muted\">" + e.footer + "</p>";
    mView->setHtml(html);
}

void ApiReference::populateTree()
{
    LILA_ZONE();

    QSignalBlocker block(mTree);
    mTree->clear();
    mTree->setColumnHidden(1, true);
    std::vector<QTreeWidgetItem*> items(mEntries.size(), nullptr);
    for (int i = 0; i < int(mEntries.size()); ++i)
    {
        const Entry& e = mEntries[i];
        QTreeWidgetItem* item = e.parent >= 0 && items[e.parent] ? new QTreeWidgetItem(items[e.parent]) : new QTreeWidgetItem(mTree);
        item->setText(0, e.name);
        item->setData(0, kEntryRole, i);
        item->setToolTip(0, e.key);
        if (e.kind == "folder" || e.kind == "module" || e.kind == "class")
        {
            QFont f = item->font(0);
            f.setBold(true);
            item->setFont(0, f);
        }
        else if (e.kind == "property" || e.kind == "attribute")
            item->setForeground(0, themeShade(170));
        items[i] = item;
    }
    for (int i = 0; i < mTree->topLevelItemCount(); ++i)
        mTree->topLevelItem(i)->setExpanded(true);
}

int ApiReference::fuzzyScore(const QString& query, const QString& candidate)
{
    // Subsequence match; rewards consecutive runs, word starts and prefixes, and penalises gaps.
    int score = 0, qi = 0, run = 0, lastMatch = -1;
    for (int ci = 0; ci < candidate.size() && qi < query.size(); ++ci)
    {
        QChar c = candidate[ci];
        if (c.toLower() != query[qi].toLower())
        {
            run = 0;
            continue;
        }
        int s = 10;
        bool boundary = ci == 0 || candidate[ci - 1] == '.' || candidate[ci - 1] == '_' ||
                        (c.isUpper() && candidate[ci - 1].isLower());
        if (boundary)
            s += 20;
        if (lastMatch == ci - 1)
            s += 15 + run * 5;
        if (lastMatch >= 0)
            s -= std::min(10, ci - lastMatch - 1);
        else
            s -= std::min(15, ci);
        score += s;
        ++run;
        lastMatch = ci;
        ++qi;
    }
    if (qi < query.size())
        return -1;
    if (candidate.compare(query, Qt::CaseInsensitive) == 0)
        score += 200;
    else if (candidate.startsWith(query, Qt::CaseInsensitive))
        score += 60;
    return score - int(candidate.size()) / 4;
}

void ApiReference::runSearch(const QString& text)
{
    LILA_ZONE();

    QString q = text.trimmed();
    if (q.isEmpty())
    {
        populateTree();
        return;
    }
    struct Hit
    {
        int score, index;
    };
    std::vector<Hit> hits;
    bool dotted = q.contains('.');
    for (int i = 0; i < int(mEntries.size()); ++i)
    {
        const Entry& e = mEntries[i];
        if (e.kind == "folder")
            continue;
        int s = fuzzyScore(q, e.name);
        // Dotted queries ("Vector.norm") and second-chance matches go against the qualified name.
        if (dotted || s < 0)
        {
            int k = fuzzyScore(q, e.key);
            if (k >= 0)
                s = std::max(s, dotted ? k : k / 2);
        }
        if (s < 0)
            continue;
        if (e.kind == "command" || e.kind == "class")
            s += 5;
        hits.push_back({s, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (hits.size() > kMaxResults)
        hits.resize(kMaxResults);

    QSignalBlocker block(mTree);
    mTree->clear();
    mTree->setColumnHidden(1, false);
    for (const Hit& h : hits)
    {
        const Entry& e = mEntries[h.index];
        auto* item = new QTreeWidgetItem(mTree);
        item->setText(0, e.name);
        QString where = e.key.left(std::max<qsizetype>(0, e.key.size() - e.name.size() - 1));
        item->setText(1, e.kind == "command" ? QString("command") : where);
        item->setForeground(1, themeShade(130));
        item->setToolTip(0, e.key);
        item->setData(0, kEntryRole, h.index);
    }
}

int ApiReference::find(const QString& symbol) const
{
    QString s = symbol.trimmed();
    if (s.endsWith("()"))
        s.chop(2);
    if (s.isEmpty())
        return -1;

    auto byKey = [&](const QString& key) {
        for (int i = 0; i < int(mEntries.size()); ++i)
            if (mEntries[i].key == key && mEntries[i].kind != "folder")
                return i;
        return -1;
    };
    if (int i = byKey(s); i >= 0)
        return i;
    for (const char* prefix : {"lila.ops.", "ops."})
        if (s.startsWith(prefix))
            if (int i = byKey("lila.ops." + s.mid(int(strlen(prefix)))); i >= 0)
                return i;
    if (int i = byKey("lila.ops." + s); i >= 0)
        return i;

    // "Node.translate" -> lila.nodes.Node.translate; "lila.Node" -> lila.nodes.Node.
    auto bySuffix = [&](const QString& tail) {
        int best = -1;
        for (int i = 0; i < int(mEntries.size()); ++i)
        {
            const Entry& e = mEntries[i];
            if (e.kind != "folder" && e.key.endsWith("." + tail) && (best < 0 || e.key.size() < mEntries[best].key.size()))
                best = i;
        }
        return best;
    };
    if (int i = bySuffix(s); i >= 0)
        return i;
    if (s.startsWith("lila."))
        return bySuffix(s.mid(5));
    return -1;
}

bool ApiReference::showSymbol(const QString& symbol)
{
    LILA_ZONE();

    if (!mBuilt)
        rebuild();
    int index = find(symbol);
    QString note;
    // Dynamic members (node attributes, say) have no entry of their own; show what they hang off.
    for (QString parent = symbol.trimmed(); index < 0 && parent.contains('.');)
    {
        parent = parent.section('.', 0, -2);
        index = find(parent);
        if (index >= 0)
            note = "<p class=\"muted\">No entry for <code>" + esc(symbol) + "</code>; showing <code>" + esc(mEntries[index].key) + "</code>.</p>";
    }
    if (index < 0)
    {
        mSearch->setText(symbol.section('.', -1));
        mView->setHtml("<p class=\"muted\">No reference entry for <code>" + esc(symbol) + "</code>. Closest matches are listed on the left.</p>");
        mCurrent = -1;
        mCopySig->setEnabled(false);
        mInsert->setEnabled(false);
        return false;
    }

    if (!mSearch->text().isEmpty())
    {
        QSignalBlocker block(mSearch);
        mSearch->clear();
        populateTree();
    }
    QTreeWidgetItemIterator it(mTree);
    for (; *it; ++it)
        if ((*it)->data(0, kEntryRole).toInt() == index)
            break;
    if (*it)
    {
        for (QTreeWidgetItem* p = (*it)->parent(); p; p = p->parent())
            p->setExpanded(true);
        QSignalBlocker block(mTree);
        mTree->setCurrentItem(*it);
        mTree->scrollToItem(*it, QAbstractItemView::PositionAtCenter);
    }
    display(index, note);
    return true;
}

std::vector<ApiSymbol> apiSymbols()
{
    LILA_ZONE();

    std::vector<ApiSymbol> out;
    for (const CommandDef* def : ops::all())
    {
        QString name = qs(def->name);
        QString key = "lila.ops." + name;
        out.push_back({key, name, "command", firstLine(qs(def->doc)), name + "("});
        for (const FlagDef& f : def->flags)
        {
            QString call = name + "(" + qs(f.name) + "=";
            out.push_back({key, call + ")", "flag", firstLine(qs(f.doc)), call});
        }
    }

    json modules = json::parse(pybridge::apiDocJson(), nullptr, false);
    if (!modules.is_array())
        return out;
    auto str = [](const json& j, const char* key) { return j.contains(key) && j[key].is_string() ? qs(j[key].get<std::string>()) : QString(); };
    std::function<void(const json&)> walk = [&](const json& j) {
        QString kind = str(j, "kind");
        QString qualname = str(j, "qualname");
        // Commands are listed above from the registry.
        if (!qualname.isEmpty() && !qualname.startsWith("lila.ops."))
        {
            bool member = kind == "method" || kind == "staticmethod" || kind == "classmethod" || kind == "property" || kind == "attribute";
            bool callable = kind == "function" || kind == "class" || kind.endsWith("method");
            QString insert = member ? str(j, "name") : qualname;
            out.push_back({qualname, qualname, kind, firstLine(str(j, "doc")), callable ? insert + "(" : insert});
        }
        if (j.contains("members") && j["members"].is_array())
            for (const json& m : j["members"])
                walk(m);
    };
    for (const json& m : modules)
        walk(m);
    return out;
}
} // namespace lila::ui
