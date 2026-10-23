#include "ui/SourceControlPanel.h"

#include "ops/Command.h"
#include "core/Log.h"
#include "vcs/SourceControl.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace lila::ui
{
using json = nlohmann::json;

SourceControlPanel::SourceControlPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    mProvider = new QLabel(this);
    mProvider->setStyleSheet("color: #9a9a9a;");
    v->addWidget(mProvider);

    auto* buttons = new QHBoxLayout;
    auto button = [&](const char* label, std::function<void()> fn) {
        auto* b = new QPushButton(label, this);
        connect(b, &QPushButton::clicked, this, fn);
        buttons->addWidget(b);
    };
    button("Refresh", [this] { refresh(); });
    button("Check Out", [this] {
        auto files = selectedFiles();
        if (files.empty())
            return;
        run("scCheckout", {{"args", files}});
    });
    button("Revert", [this] {
        auto files = selectedFiles();
        if (!files.empty())
            run("scRevert", {{"args", files}});
    });
    button("Submit...", [this] { submit(); });
    button("Get Latest", [this] { run("scSync", json::object()); });
    buttons->addStretch(1);
    v->addLayout(buttons);

    mTree = new QTreeWidget(this);
    mTree->setHeaderLabels({"Change / File", "Description"});
    mTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    mTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    v->addWidget(mTree, 1);
}

void SourceControlPanel::run(const char* command, const json& call)
{
    try
    {
        ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
    refresh();
}

void SourceControlPanel::refresh()
{
    mTree->clear();
    try
    {
        json info = ops::execute("scProvider", json::object());
        mProvider->setText(QString::fromStdString("Provider: " + info.value("provider", std::string("none"))));
        json lists = ops::execute("scChangelists", json::object());
        for (const json& cl : lists.is_array() ? lists : json::array())
        {
            auto* top = new QTreeWidgetItem(mTree);
            std::string id = cl.value("id", "");
            top->setText(0, QString::fromStdString(id == "default" ? "Default" : "Change " + id));
            top->setText(1, QString::fromStdString(cl.value("description", "")));
            top->setData(0, Qt::UserRole, QString::fromStdString(id));
            for (const json& f : cl.value("files", json::array()))
            {
                auto* item = new QTreeWidgetItem(top);
                item->setText(0, QString::fromStdString(f.get<std::string>()));
                item->setData(0, Qt::UserRole + 1, QString::fromStdString(f.get<std::string>()));
            }
            top->setExpanded(true);
        }
    }
    catch (const CommandError& e)
    {
        mProvider->setText(QString("Source control: ") + e.what());
    }
}

std::vector<std::string> SourceControlPanel::selectedFiles() const
{
    std::vector<std::string> out;
    for (QTreeWidgetItem* item : mTree->selectedItems())
    {
        QString file = item->data(0, Qt::UserRole + 1).toString();
        if (!file.isEmpty())
            out.push_back(file.toStdString());
    }
    return out;
}

std::string SourceControlPanel::selectedChange() const
{
    for (QTreeWidgetItem* item : mTree->selectedItems())
    {
        QTreeWidgetItem* top = item->parent() ? item->parent() : item;
        return top->data(0, Qt::UserRole).toString().toStdString();
    }
    return "default";
}

void SourceControlPanel::submit()
{
    QDialog dlg(this);
    dlg.setWindowTitle("Submit");
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel(QString::fromStdString("Submit change " + selectedChange()), &dlg));
    auto* desc = new QPlainTextEdit(&dlg);
    desc->setPlaceholderText("Description");
    v->addWidget(desc);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted || desc->toPlainText().trimmed().isEmpty())
        return;
    json call = {{"description", desc->toPlainText().trimmed().toStdString()}};
    if (selectedChange() != "default")
        call["changelist"] = selectedChange();
    if (auto files = selectedFiles(); !files.empty())
        call["args"] = files;
    run("scSubmit", call);
}

void sceneSourceStatus(const std::filesystem::path& scene, std::function<void(QString)> done)
{
    if (scene.empty())
    {
        done({});
        return;
    }
    vcs::SourceControl::get().statusAsync({scene}, [done](std::vector<vcs::FileStatus> st, std::string error) {
        if (!error.empty() || st.empty())
        {
            done({});
            return;
        }
        const vcs::FileStatus& s = st.front();
        QString text;
        if (s.state == vcs::FileState::NotTracked)
            text = "not in source control";
        else if (s.lockedByOther || !s.otherUsers.empty())
            text = "checked out by " + QString::fromStdString(s.otherUsers.empty() ? std::string("someone else") : s.otherUsers.front());
        else if (s.checkedOutByMe)
            text = "checked out";
        else if (s.outOfDate)
            text = "out of date";
        else
            text = "read-only (not checked out)";
        done(text);
    });
}
} // namespace lila::ui
