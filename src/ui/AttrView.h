#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include "scene/Node.h"
#include "scene/Plug.h"

#include <QString>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class QWidget;

/**
 * What the Attribute Editor and Channel Box share: the name mode, the lead object, multi-object editing behind Alt, the
 * display precision and namespaces, saved views and per-type help pages. Prefs live in the "attributes" file.
 */
namespace lila::ui::attrView
{
enum class NameMode : uint8_t
{
    Nice,
    Long,
    Short,
};
LILA_UI_API NameMode nameMode();
LILA_UI_API void setNameMode(NameMode m);
LILA_UI_API const char* nameModeName(NameMode m);
/** An attr's label in the current mode; component >= 0 labels one of a Vec3's children. */
LILA_UI_API QString label(const scene::AttrDef& def, int component = -1);
/** "maxHealth" as "Max Health". */
LILA_UI_API QString niceName(const std::string& name);

/** The object edits go to: the one set with setLead while it stays selected, else the last selected. */
LILA_UI_API scene::NodeId lead();
/** Makes a selected object the lead without changing the selection; kNoNode clears it. */
LILA_UI_API void setLead(scene::NodeId id);
/** The selection with the lead first: the order the header chips and multi-object edits use. */
LILA_UI_API std::vector<scene::NodeId> selectionLeadFirst();

/** True while Alt is held: edits then go to every selected object that has the attribute. */
LILA_UI_API bool multiEditHeld();
/** Tests and scripts arm multi-object editing without a key: held true until set false. */
LILA_UI_API void setMultiEditForced(bool on);
/** Watches Alt for the panels (call once with the QApplication up). Swallows the release after a multi-object edit. */
LILA_UI_API void installKeyWatch();
/** Records that Alt was used for an edit, so its release doesn't focus the menu bar. */
LILA_UI_API void noteMultiEditUsed();
/** Channel Box edits go to every selected object that has the channel, as in Maya (default); off edits the lead only
 * and Alt edits all. The Attribute Editor always edits the lead. */
LILA_UI_API bool channelBoxEditsAll();
LILA_UI_API void setChannelBoxEditsAll(bool on);
/** The "Changed X only" hint after a lead-only edit with several objects selected. */
LILA_UI_API bool hintEnabled();
LILA_UI_API void setHintEnabled(bool on);

/** Decimal places shown in numeric fields (default 3). */
LILA_UI_API int precision();
LILA_UI_API void setPrecision(int places);
/** Show namespaces in object names (default false). */
LILA_UI_API bool showNamespaces();
LILA_UI_API void setShowNamespaces(bool on);
/** A number as the panels show it: precision places, trailing zeros dropped. */
LILA_UI_API QString formatNumber(double v);

/** The Channel Box's highlighted channels, and whether the Graph Editor shows only those. */
LILA_UI_API const std::vector<scene::PlugRef>& highlightedChannels();
LILA_UI_API void setHighlightedChannels(std::vector<scene::PlugRef> plugs);
LILA_UI_API bool syncGraphEditor();
LILA_UI_API void setSyncGraphEditor(bool on);

/** Channel Box scrubbing: a speed multiplier, hyperbolic (faster drags go further), and when a middle drag in a viewport
 * scrubs the highlighted channels: "always", "select" (only with the Select tool, never over a manipulator) or "off". */
LILA_UI_API double scrubSpeed();
LILA_UI_API void setScrubSpeed(double s);
LILA_UI_API bool hyperbolicScrub();
LILA_UI_API void setHyperbolicScrub(bool on);
LILA_UI_API std::string viewportScrub();
LILA_UI_API void setViewportScrub(const std::string& mode);
/** A scrub step for a drag of dx pixels at base step size, with the speed and hyperbolic settings applied. */
LILA_UI_API double scrubStep(double base, int dx);

/** Called after any setting here or the lead changes. Returns an id for removeListener. */
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);

/** A set of Attribute Editor sections hidden per node type: {type: [section titles]}. */
struct View
{
    std::string name;
    /** "lila", "user" or "studio". */
    std::string source;
    std::map<std::string, std::set<std::string>> hidden;
    /** Built-in views show only sections whose title contains one of these words, when set. */
    std::vector<std::string> onlyContaining;
};
/** Default, Lookdev, Rigging, then the user's and the studio's saved ones. */
LILA_UI_API std::vector<View> views();
LILA_UI_API std::string currentView();
LILA_UI_API void setCurrentView(const std::string& name);
/**
 * Whether the current view (and the per-type toggles) shows a section of a type. An added-attribute section (added) is
 * hidden only by its own toggle, never by a built-in view's word list.
 */
LILA_UI_API bool sectionShown(const std::string& typeName, const QString& title, bool added = false);
/** The per-type toggle, kept in the current view when it is the user's, else in the user's own "Custom" view. */
LILA_UI_API void setSectionShown(const std::string& typeName, const QString& title, bool shown);
/** Saves the current view's hidden sections under a name, in the user's prefs or the first studio folder. */
LILA_UI_API bool saveView(const std::string& name, bool studio);

/** The docs page a node type's help opens; registered per type, else the Attributes page at the type's heading. */
LILA_UI_API Registry<std::string>& helpPages();
LILA_UI_API std::string helpPageFor(const scene::NodeType& type);
} // namespace lila::ui::attrView
