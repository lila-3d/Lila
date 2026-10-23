#include "ui/SmartJoinUi.h"

#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "ui/FlagFields.h"
#include "ui/OptionBox.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <map>

namespace lila::ui
{
namespace
{
namespace opt = ops::options;
constexpr const char* kCommand = "smartJoin";

QWidget* mainWindow()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w->inherits("QMainWindow") && w->isVisible())
            return w;
    return nullptr;
}

class SmartJoinDialog : public QDialog
{
public:
    explicit SmartJoinDialog(QWidget* parent) : QDialog(parent)
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowTitle("Smart Join Options");
        auto* root = new QVBoxLayout(this);
        root->setContentsMargins(10, 8, 10, 8);
        root->setSpacing(6);

        auto* doc = new QLabel("Select the limb and the body, then Join. Changes re-run the join on the same meshes.", this);
        doc->setWordWrap(true);
        doc->setStyleSheet("color: #9a9a9a;");
        root->addWidget(doc);

        auto* grid = new QGridLayout();
        grid->setHorizontalSpacing(8);
        grid->setVerticalSpacing(4);
        grid->setColumnStretch(1, 1);
        const json values = opt::values(kCommand);
        std::string section = "\x01";
        int row = 0;
        for (const opt::Field& f : opt::fields(kCommand))
        {
            if (f.section != section)
            {
                section = f.section;
                if (!section.empty())
                {
                    auto* header = new QLabel(QString::fromStdString(section), this);
                    QFont hf = header->font();
                    hf.setWeight(QFont::DemiBold);
                    header->setFont(hf);
                    header->setContentsMargins(0, row ? 6 : 0, 0, 2);
                    grid->addWidget(header, row++, 0, 1, 2);
                }
            }
            const std::string flag = f.flag;
            FlagFieldWidgets w = makeFlagField(f, values.value(flag, f.defaultValue), [this, flag](const json& v) {
                opt::setValue(kCommand, flag, v);
                if (mLive->isChecked())
                    mRerun->start();
            }, this);
            mSetters[flag] = w.set;
            if (w.label)
                grid->addWidget(w.label, row, 0, Qt::AlignRight | Qt::AlignVCenter);
            grid->addWidget(w.field, row, 1);
            ++row;
        }
        root->addLayout(grid);

        mConfidence = new QLabel("--", this);
        QFont big = mConfidence->font();
        big.setPointSizeF(big.pointSizeF() * 1.8);
        big.setWeight(QFont::DemiBold);
        mConfidence->setFont(big);
        mConfidence->setToolTip("How sure Smart Join is that the result is clean topology");
        mReasons = new QLabel("No join yet.", this);
        mReasons->setWordWrap(true);
        mReasons->setTextInteractionFlags(Qt::TextSelectableByMouse);
        mReasons->setStyleSheet("color: #b0b0b0;");
        auto* readout = new QHBoxLayout();
        readout->addWidget(mConfidence, 0, Qt::AlignTop);
        readout->addWidget(mReasons, 1);
        root->addLayout(readout);

        mLive = new QCheckBox("Live", this);
        mLive->setChecked(true);
        mLive->setToolTip("Re-run the join on its original meshes whenever a setting changes");
        auto* buttons = new QHBoxLayout();
        auto* join = new QPushButton("Join", this);
        join->setDefault(true);
        join->setToolTip("Join the two selected meshes (smartJoin)");
        auto* reset = new QPushButton("Reset", this);
        reset->setToolTip("Back to the default settings");
        auto* closeButton = new QPushButton("Close", this);
        buttons->addWidget(mLive);
        buttons->addStretch(1);
        for (QPushButton* b : {join, reset, closeButton})
            buttons->addWidget(b);
        root->addLayout(buttons);

        mRerun = new QTimer(this);
        mRerun->setSingleShot(true);
        mRerun->setInterval(80);
        connect(mRerun, &QTimer::timeout, this, [this] { rerun(); });
        connect(join, &QPushButton::clicked, this, [this] { joinSelection(); });
        connect(reset, &QPushButton::clicked, this, [this] {
            opt::reset(kCommand);
            const json v = opt::values(kCommand);
            for (auto& [flag, set] : mSetters)
                if (set && v.contains(flag))
                    set(v[flag]);
            if (mLive->isChecked())
                mRerun->start();
        });
        connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
        resize(400, sizeHint().height());
    }

private:
    /** The join this dialog ran is still the latest undo step. */
    bool live() const
    {
        const UndoStack& u = UndoStack::get();
        return !mTargets.empty() && u.serial() == mSerial && u.undoLabel() == kCommand;
    }

    void showResult(const json& r)
    {
        mConfidence->setText(QString("%1%").arg(r.value("confidence", 0)));
        QStringList lines;
        for (const json& why : r.value("reasons", json::array()))
            lines << QString::fromStdString(why.get<std::string>());
        lines << QString("%1-point loop, %2-point hole, %3 segments, %4 support loops, %5 triangles")
                     .arg(r.value("limbLoopSize", 0))
                     .arg(r.value("holeSize", 0))
                     .arg(r.value("segments", 0))
                     .arg(r.value("supportLoops", 0))
                     .arg(r.value("triangles", 0));
        mReasons->setText(lines.join("\n"));
    }

    void showError(const char* what)
    {
        mConfidence->setText("0%");
        mReasons->setText(QString::fromUtf8(what));
    }

    void joinSelection()
    {
        try
        {
            const json r = ops::execute(kCommand, opt::callWith(kCommand), CallSource::UI);
            mTargets = {r.value("limb", std::string()), r.value("body", std::string())};
            mSerial = UndoStack::get().serial();
            showResult(r);
        }
        catch (const CommandError& e)
        {
            mTargets.clear();
            showError(e.what());
        }
    }

    void rerun()
    {
        if (!live())
            return;
        UndoStack& u = UndoStack::get();
        u.undo();
        const uint64_t before = u.serial();
        json call = opt::callWith(kCommand);
        // Undo brought the inputs back under their names.
        call["args"] = mTargets;
        try
        {
            showResult(ops::execute(kCommand, call, CallSource::Script));
            mSerial = u.serial();
        }
        catch (const CommandError& e)
        {
            if (u.serial() == before)
                u.redo();
            mSerial = u.serial();
            showError(e.what());
        }
    }

    QLabel* mConfidence = nullptr;
    QLabel* mReasons = nullptr;
    QCheckBox* mLive = nullptr;
    QTimer* mRerun = nullptr;
    std::map<std::string, std::function<void(const json&)>> mSetters;
    json mTargets = json::array();
    uint64_t mSerial = 0;
};

void openDialog()
{
    static QPointer<SmartJoinDialog> dialog;
    if (!dialog)
        dialog = new SmartJoinDialog(mainWindow());
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
} // namespace

void registerSmartJoinUi()
{
    opt::Override o;
    o.order = {"segments", "tension", "twist", "relax", "holeScale", "offsetU", "offsetV", "limbLoop", "deleteBuried", "trimMargin", "flip",
               "keepOriginals"};
    o.hidden = {"name"};
    for (const char* f : {"segments", "tension", "twist", "relax"})
        o.sections[f] = "Bridge";
    for (const char* f : {"holeScale", "offsetU", "offsetV"})
        o.sections[f] = "Hole";
    for (const char* f : {"limbLoop", "deleteBuried", "trimMargin", "flip"})
        o.sections[f] = "Limb";
    o.sections["keepOriginals"] = "Result";
    o.labels = {{"offsetU", "Offset U"}, {"offsetV", "Offset V"}, {"limbLoop", "Limb Loop"}, {"flip", "Swap Limb and Body"}};
    o.ranges = {{"segments", {0, 32, 1, ""}},       {"tension", {0, 4, 0.05, ""}},      {"twist", {-32, 32, 1, ""}},
                {"relax", {0, 50, 1, ""}},          {"holeScale", {0.1, 4, 0.05, ""}},  {"offsetU", {-500, 500, 0.5, " cm"}},
                {"offsetV", {-500, 500, 0.5, " cm"}}, {"limbLoop", {-1, 32, 1, ""}},    {"trimMargin", {0, 2, 0.05, ""}}};
    opt::setOverride(kCommand, std::move(o));
    optionBox::addCustom("mesh.smartJoin", [] { openDialog(); });
}
} // namespace lila::ui
