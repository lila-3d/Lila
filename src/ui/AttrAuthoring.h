#pragma once

#include "core/Export.h"

#include "scene/Node.h"
#include "scene/Plug.h"

#include <QFrame>
#include <QString>

#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QWidget;

/**
 * Authoring attributes in place: the Add popover (new attributes, components, copy from another object) and the Edit
 * popover on an added attribute. Both act on every node they were opened for, run commands (so they undo and echo), and
 * close on commit. Child widgets carry object names for tests.
 */
namespace lila::ui::attrAuthoring
{
using json = nlohmann::json;

/** The type names the popovers offer, in menu order. */
LILA_UI_API const std::vector<std::string>& typeNames();
/** The addAttr flags (attributeType or dataType, multi, usedAs...) for one of typeNames. */
LILA_UI_API json addAttrFlags(const std::string& type, bool array);
/** Typed text as a value of the type: numbers, "x y z" or "x, y, z", true/false/on/off, enum labels. Null when it doesn't read. */
LILA_UI_API json parseValue(const std::string& type, const QString& text, const std::vector<std::string>& enumNames = {});
/** A value shown as parseValue reads it back. */
LILA_UI_API QString valueText(const json& v);
/** Whether an attribute matches the Attribute Editor's search: nice, long and short names and its doc, case blind. */
LILA_UI_API bool matches(const scene::AttrDef& def, const QString& query);
/** The heading added attributes go under when none is given, and where the Attribute Editor shows uncategorised ones. */
inline constexpr const char* kCustomCategory = "Custom";

/** The headings (categories) the nodes' added attributes use, in first-use order. */
LILA_UI_API std::vector<std::string> categories(const std::vector<scene::NodeId>& nodes);

class LILA_UI_API AddPopover : public QFrame
{
    Q_OBJECT

public:
    /** category presets the new attribute's heading (the + at the end of a component). */
    explicit AddPopover(std::vector<scene::NodeId> nodes, std::string category = {}, QWidget* parent = nullptr);

    /** Runs the list row: a new attribute, a component, or copying from an object. True when it did something. */
    bool activate(QListWidgetItem* item);
    /** Creates the new attribute the search names with the form's settings. False, with the reason in the status line, on error. */
    bool commitNew();

Q_SIGNALS:
    void committed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refill();
    void showForm(bool on);
    void updateFormForType();

    std::vector<scene::NodeId> mNodes;
    QLineEdit* mSearch = nullptr;
    QListWidget* mList = nullptr;
    QWidget* mForm = nullptr;
    QComboBox* mType = nullptr;
    QCheckBox* mArray = nullptr;
    QLineEdit* mDefault = nullptr;
    QLineEdit* mMin = nullptr;
    QLineEdit* mMax = nullptr;
    QLineEdit* mEnum = nullptr;
    QComboBox* mCategory = nullptr;
    QLabel* mStatus = nullptr;
};

class LILA_UI_API EditPopover : public QFrame
{
    Q_OBJECT

public:
    EditPopover(std::vector<scene::NodeId> nodes, std::string attr, QWidget* parent = nullptr);

    /** Runs one editAttr with every changed field. False, with the reason in the status line, on error. */
    bool apply();

Q_SIGNALS:
    void committed();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    std::vector<scene::NodeId> mNodes;
    std::string mAttr;
    json mBefore;
    QLineEdit* mName = nullptr;
    QLineEdit* mNice = nullptr;
    QComboBox* mType = nullptr;
    QCheckBox* mArray = nullptr;
    QLineEdit* mDefault = nullptr;
    QLineEdit* mMin = nullptr;
    QLineEdit* mMax = nullptr;
    QLineEdit* mSoftMin = nullptr;
    QLineEdit* mSoftMax = nullptr;
    QListWidget* mEnum = nullptr;
    QCheckBox* mKeyable = nullptr;
    QCheckBox* mChannelBox = nullptr;
    QCheckBox* mHidden = nullptr;
    QLineEdit* mDoc = nullptr;
    QComboBox* mCategory = nullptr;
    QLabel* mStatus = nullptr;
};

/** Opens the Add popover below anchor. It deletes itself on close. */
LILA_UI_API AddPopover* showAdd(std::vector<scene::NodeId> nodes, QWidget* anchor, std::string category = {});
/** Opens the Edit popover for an added attribute below anchor. It deletes itself on close. */
LILA_UI_API EditPopover* showEdit(std::vector<scene::NodeId> nodes, const std::string& attr, QWidget* anchor);
} // namespace lila::ui::attrAuthoring
