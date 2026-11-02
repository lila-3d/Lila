#include "ui/ChoiceTips.h"

#include "ops/CommandOptions.h"
#include "scene/Plug.h"

#include <QComboBox>
#include <QEvent>

namespace lila::ui::choiceTips
{
namespace
{
constexpr const char* kOwnTip = "lilaChoiceOwnTip";
constexpr const char* kShownTip = "lilaChoiceShownTip";

/** The tip the combo's owner set, as opposed to the choice doc shown in its place. */
QString ownTip(QComboBox* combo)
{
    const QString tip = combo->toolTip();
    if (tip != combo->property(kShownTip).toString())
        combo->setProperty(kOwnTip, tip);
    return combo->property(kOwnTip).toString();
}

void showCurrent(QComboBox* combo)
{
    const QString own = ownTip(combo);
    const QString doc = combo->currentIndex() >= 0 ? combo->itemData(combo->currentIndex(), Qt::ToolTipRole).toString() : QString();
    const QString tip = doc.isEmpty() ? own : doc;
    combo->setProperty(kShownTip, tip);
    combo->setToolTip(tip);
}

/** Owners refresh the current index with signals blocked, so the tip catches up when the pointer arrives. */
class CurrentTip : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() == QEvent::Enter)
            showCurrent(static_cast<QComboBox*>(o));
        return false;
    }
};
} // namespace

std::vector<std::string> forAttr(const scene::AttrDef& def)
{
    std::vector<std::string> docs = ops::options::choiceDocs(def.doc, def.enumNames);
    for (size_t i = 0; i < docs.size() && i < def.enumDocs.size(); ++i)
        if (!def.enumDocs[i].empty())
            docs[i] = def.enumDocs[i];
    return docs;
}

void apply(QComboBox* combo, const std::vector<std::string>& docs)
{
    for (int i = 0; i < combo->count(); ++i)
        combo->setItemData(i, size_t(i) < docs.size() && !docs[size_t(i)].empty() ? QVariant(QString::fromStdString(docs[size_t(i)])) : QVariant(),
                           Qt::ToolTipRole);
    if (!combo->property(kShownTip).isValid())
    {
        combo->setProperty(kShownTip, QString());
        combo->installEventFilter(new CurrentTip(combo));
        QObject::connect(combo, &QComboBox::currentIndexChanged, combo, [combo] { showCurrent(combo); });
    }
    showCurrent(combo);
}

void refresh(QComboBox* combo)
{
    if (combo->property(kShownTip).isValid())
        showCurrent(combo);
}
} // namespace lila::ui::choiceTips
