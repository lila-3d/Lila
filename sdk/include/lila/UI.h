#pragma once

/** Every UI extension point. Each lives in lila::ui; register through the extension's ext::Context so unloading removes it. */
#include "lila/Actions.h"
#include "lila/AttributeEditor.h"
#include "lila/Delete.h"
#include "lila/GraphBackends.h"
#include "lila/Hotkeys.h"
#include "lila/Layouts.h"
#include "lila/MarkingMenus.h"
#include "lila/Menus.h"
#include "lila/Modes.h"
#include "lila/OptionBox.h"
#include "lila/Panels.h"
#include "lila/PreferencePages.h"
#include "lila/Shelves.h"
#include "ui/ColorPopup.h"
