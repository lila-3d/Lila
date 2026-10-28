#include "ui/ThemeEditor.h"

#include "core/PrefStore.h"
#include "ops/Command.h"
#include "ui/Icons.h"
#include "ui/ThemeModel.h"
#include "ui/FileDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QScrollBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace lila::ui
{
namespace
{
QString qs(const std::string& s) { return QString::fromStdString(s); }

QIcon swatch(themes::Rgba c)
{
    QPixmap pm(28, 14);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.fillRect(pm.rect(), QColor(128, 128, 128));
    p.fillRect(pm.rect(), QColor(c.r, c.g, c.b, c.a));
    p.setPen(QColor(0, 0, 0, 120));
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    return QIcon(pm);
}

class ThemeEditor : public QWidget
{
public:
    explicit ThemeEditor(QWidget* parent) : QWidget(parent)
    {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel("Theme", this));
        mPick = new QComboBox(this);
        mPick->setMinimumWidth(180);
        mPick->setToolTip("The interface theme; switches at once");
        row->addWidget(mPick, 1);
        auto button = [&](const char* iconName, const char* tip, auto fn) {
            auto* b = new QToolButton(this);
            b->setIcon(icon(iconName));
            b->setToolTip(tip);
            b->setAutoRaise(true);
            connect(b, &QToolButton::clicked, this, fn);
            row->addWidget(b);
            return b;
        };
        button("copy", "Duplicate: a saved copy you can edit", [this] { duplicate(); });
        mRename = button("pencil", "Rename", [this] { renameTheme(); });
        mDelete = button("trash", "Delete", [this] { deleteTheme(); });
        auto* share = button("share", "Import and export", [] {});
        auto* menu = new QMenu(share);
        menu->addAction(icon("file-import"), "Import File...", this, [this] { importFile(); });
        menu->addAction(icon("clipboard"), "Import Pasted Text...", this, [this] { importText(); });
        menu->addSeparator();
        menu->addAction(icon("file-export"), "Export File...", this, [this] { exportFile(); });
        menu->addAction(icon("clipboard-copy"), "Copy as Text", this, [this] { copyText(); });
        share->setMenu(menu);
        share->setPopupMode(QToolButton::InstantPopup);
        layout->addLayout(row);

        mInfo = new QLabel(this);
        mInfo->setWordWrap(true);
        mInfo->setForegroundRole(QPalette::PlaceholderText);
        layout->addWidget(mInfo);

        mTree = new QTreeWidget(this);
        mTree->setColumnCount(2);
        mTree->setHeaderLabels({"Colour", "Value"});
        mTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        mTree->setRootIsDecorated(true);
        mTree->setMinimumHeight(260);
        mTree->setContextMenuPolicy(Qt::CustomContextMenu);
        layout->addWidget(mTree);

        connect(mPick, &QComboBox::activated, this, [this](int i) { run("setTheme", {{"args", {mPick->itemText(i).toStdString()}}}); });
        connect(mTree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) { editToken(item); });
        connect(mTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) {
            QTreeWidgetItem* item = mTree->itemAt(at);
            if (!item || item->data(0, Qt::UserRole).toString().isEmpty() || builtIn())
                return;
            QMenu m;
            m.addAction("Edit...", [this, item] { editToken(item); });
            m.addAction("Reset to Base Theme", [this, item] {
                run("setThemeColor", {{"args", {name()}}, {"token", item->data(0, Qt::UserRole).toString().toStdString()}, {"color", ""}});
            });
            m.exec(mTree->viewport()->mapToGlobal(at));
        });

        mListener = PrefStore::get().onChanged([self = QPointer<ThemeEditor>(this)](const std::string& file) {
            if (self && file == "general")
                self->refresh();
        });
        refresh();
    }

    ~ThemeEditor() override { PrefStore::get().removeOnChanged(mListener); }

private:
    std::string name() const { return themes::currentName(); }
    bool builtIn() const { return themes::isBuiltIn(name()); }

    json run(const char* cmd, const json& call)
    {
        try
        {
            return ops::execute(cmd, call, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            QMessageBox::warning(this, "Theme", e.what());
            return nullptr;
        }
    }

    void refresh()
    {
        std::vector<std::string> names = themes::names();
        std::string cur = themes::current()->name;
        {
            QSignalBlocker block(mPick);
            mPick->clear();
            for (const std::string& n : names)
                mPick->addItem(qs(n));
            mPick->setCurrentIndex(mPick->findText(qs(cur)));
        }
        std::shared_ptr<const themes::Theme> t = themes::current();
        bool fixed = themes::isBuiltIn(t->name);
        mRename->setEnabled(!fixed);
        mDelete->setEnabled(!fixed);
        mInfo->setText(fixed ? qs(t->description) + "\nBuilt-in themes are read-only: Duplicate one to edit its colours."
                             : QString("Saved theme%1, based on %2. Double-click a colour to edit it; right-click to reset it.")
                                   .arg(t->author.empty() ? QString() : " by " + qs(t->author), t->base == "light" ? "Light" : "Monotone"));

        std::map<std::string, bool> open;
        for (int i = 0; i < mTree->topLevelItemCount(); ++i)
            open[mTree->topLevelItem(i)->text(0).toStdString()] = mTree->topLevelItem(i)->isExpanded();
        int scroll = mTree->verticalScrollBar() ? mTree->verticalScrollBar()->value() : 0;
        mTree->clear();
        std::map<std::string, QTreeWidgetItem*> groups;
        for (const themes::TokenInfo& info : themes::tokens())
        {
            QTreeWidgetItem*& g = groups[info.group];
            if (!g)
            {
                g = new QTreeWidgetItem(mTree, {qs(info.group)});
                g->setFlags(Qt::ItemIsEnabled);
                g->setExpanded(open.contains(info.group) ? open[info.group] : std::string(info.group) == "Interface");
            }
            themes::Rgba c = t->color(info.id);
            auto* item = new QTreeWidgetItem(g, {qs(info.label), qs(themes::formatColor(c))});
            item->setIcon(1, swatch(c));
            item->setData(0, Qt::UserRole, qs(info.id));
            item->setToolTip(0, qs(info.id));
            bool own = t->colors.contains(info.id);
            QFont f = item->font(1);
            f.setItalic(!own && !fixed);
            item->setFont(1, f);
            if (!own && !fixed)
                item->setToolTip(1, "From the base theme");
        }
        if (mTree->verticalScrollBar())
            mTree->verticalScrollBar()->setValue(scroll);
    }

    void editToken(QTreeWidgetItem* item)
    {
        std::string id = item->data(0, Qt::UserRole).toString().toStdString();
        if (id.empty())
            return;
        if (builtIn())
        {
            QMessageBox::information(this, "Theme", "Built-in themes are read-only. Duplicate this one to edit its colours.");
            return;
        }
        std::optional<themes::Theme> before = themes::find(name());
        if (!before)
            return;
        themes::Rgba c = before->color(id);
        QColorDialog dlg(QColor(c.r, c.g, c.b, c.a), this);
        dlg.setOption(QColorDialog::ShowAlphaChannel, c.a != 255);
        // Live preview: the theme is saved as the picker moves, and put back on cancel.
        connect(&dlg, &QColorDialog::currentColorChanged, this, [&](const QColor& q) {
            themes::Theme t = *before;
            t.colors[id] = {uint8_t(q.red()), uint8_t(q.green()), uint8_t(q.blue()), uint8_t(q.alpha())};
            themes::save(t);
        });
        if (dlg.exec() == QDialog::Accepted)
        {
            QColor q = dlg.selectedColor();
            themes::save(*before);
            run("setThemeColor", {{"args", {name()}},
                                  {"token", id},
                                  {"color", themes::formatColor({uint8_t(q.red()), uint8_t(q.green()), uint8_t(q.blue()), uint8_t(q.alpha())})}});
        }
        else
            themes::save(*before);
    }

    void duplicate()
    {
        bool ok = false;
        QString to = QInputDialog::getText(this, "Duplicate Theme", "Name:", QLineEdit::Normal, qs(name()) + " Copy", &ok).trimmed();
        if (!ok || to.isEmpty())
            return;
        json made = run("duplicateTheme", {{"args", {name()}}, {"to", to.toStdString()}});
        if (made.is_string())
            run("setTheme", {{"args", {made.get<std::string>()}}});
    }

    void renameTheme()
    {
        bool ok = false;
        QString to = QInputDialog::getText(this, "Rename Theme", "Name:", QLineEdit::Normal, qs(name()), &ok).trimmed();
        if (ok && !to.isEmpty() && to.toStdString() != name())
            run("renameTheme", {{"args", {name()}}, {"to", to.toStdString()}});
    }

    void deleteTheme()
    {
        if (QMessageBox::question(this, "Delete Theme", "Delete the theme \"" + qs(name()) + "\"?") == QMessageBox::Yes)
            run("deleteTheme", {{"args", {name()}}});
    }

    void importText(const std::string& text)
    {
        themes::ParseResult r = themes::parseText(text);
        if (!r.theme)
        {
            QString msg = "This is not a theme Lila can read:";
            for (const std::string& e : r.errors)
                msg += "\n  " + qs(e);
            QMessageBox::warning(this, "Import Theme", msg);
            return;
        }
        json call = {{"text", text}, {"apply", true}};
        if (themes::isBuiltIn(r.theme->name) || themes::find(r.theme->name))
        {
            bool ok = false;
            QString as = QInputDialog::getText(this, "Import Theme", "A theme named \"" + qs(r.theme->name) + "\" exists. Import as:",
                                               QLineEdit::Normal, qs(r.theme->name) + " 2", &ok)
                             .trimmed();
            if (!ok || as.isEmpty())
                return;
            call["name"] = as.toStdString();
            call["replace"] = true;
        }
        run("importTheme", call);
        if (!r.warnings.empty())
        {
            QString msg = "Imported, skipping:";
            for (const std::string& w : r.warnings)
                msg += "\n  " + qs(w);
            QMessageBox::information(this, "Import Theme", msg);
        }
    }

    void importFile()
    {
        QString file = ui::fileDialog::getOpenFileName(this, "Import Theme", QString(), "Lila themes (*.lilatheme *.json);;All files (*)");
        if (file.isEmpty())
            return;
        std::ifstream f(std::filesystem::path(file.toStdWString()), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        importText(ss.str());
    }

    void importText()
    {
        bool ok = false;
        QString text = QInputDialog::getMultiLineText(this, "Import Theme", "Paste a theme's text:", QApplication::clipboard()->text(), &ok);
        if (ok && !text.trimmed().isEmpty())
            importText(text.toStdString());
    }

    void exportFile()
    {
        QString file = ui::fileDialog::getSaveFileName(this, "Export Theme", qs(name()) + ".lilatheme", "Lila themes (*.lilatheme)");
        if (!file.isEmpty())
            run("exportTheme", {{"args", {name()}}, {"file", file.toStdString()}});
    }

    void copyText()
    {
        json text = run("exportTheme", {{"args", {name()}}});
        if (text.is_string())
            QApplication::clipboard()->setText(qs(text.get<std::string>()));
    }

    QComboBox* mPick = nullptr;
    QToolButton* mRename = nullptr;
    QToolButton* mDelete = nullptr;
    QLabel* mInfo = nullptr;
    QTreeWidget* mTree = nullptr;
    int mListener = 0;
};
} // namespace

QWidget* makeThemeEditor(QWidget* parent)
{
    return new ThemeEditor(parent);
}
} // namespace lila::ui
