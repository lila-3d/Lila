#pragma once

#include <string_view>

/**
 * When each Steam achievement unlocks. Most follow from the commands the user runs (never an agent's); the rest are
 * reported by the UI that sees them, through the functions below. All of it is a no-op unless Lila runs under Steam.
 */
namespace lila::ui::achievementRules
{
void start();
void stop();

void scriptEditorRan();
void cheatSheetShown();
void commandSearchRan();
void sceneSearchSelected();
void primitivePlaced();
void hotboxPicked();
void lastSceneReopened();
void recoveredSceneOpened();
void presenterModeOn();
void todoListTicked(int items, int ticked);
void outlinerFilter(std::string_view text);
void texturesDropped(int assigned);
} // namespace lila::ui::achievementRules
