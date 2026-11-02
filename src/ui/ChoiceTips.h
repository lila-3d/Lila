#pragma once

#include "core/Export.h"

#include <string>
#include <vector>

class QComboBox;

namespace lila::scene
{
struct AttrDef;
}

namespace lila::ui::choiceTips
{
/** Each enum name's doc, by index: def.enumDocs where given, else what def.doc says about that name. */
LILA_UI_API std::vector<std::string> forAttr(const scene::AttrDef& def);

/**
 * Gives combo item i the hover tip docs[i], and keeps the closed combo's own tip on the current item's doc. A current
 * item without one shows the tip the combo's owner set. Call again after refilling the items.
 */
LILA_UI_API void apply(QComboBox* combo, const std::vector<std::string>& docs);

/** Puts the current item's doc on the closed combo now, after a change made with its signals blocked. */
LILA_UI_API void refresh(QComboBox* combo);
} // namespace lila::ui::choiceTips
