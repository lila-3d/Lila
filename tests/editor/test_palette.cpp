#include "ui/editor/EditorPalette.h"

#include <doctest/doctest.h>

using lila::ui::palette::resolveLook;

TEST_CASE("script editor look: own pref, app Monotone override and light base")
{
    for (bool light : {false, true})
    {
        // editorMonotone, followAppMonotone, appMonotone -> grey
        CHECK(resolveLook(false, false, false, light).monotone == false);
        CHECK(resolveLook(false, false, true, light).monotone == false);
        CHECK(resolveLook(false, true, false, light).monotone == false);
        CHECK(resolveLook(false, true, true, light).monotone == true);
        CHECK(resolveLook(true, false, false, light).monotone == true);
        CHECK(resolveLook(true, false, true, light).monotone == true);
        CHECK(resolveLook(true, true, false, light).monotone == true);
        CHECK(resolveLook(true, true, true, light).monotone == true);
        for (bool a : {false, true})
            for (bool b : {false, true})
                for (bool c : {false, true})
                    CHECK(resolveLook(a, b, c, light).light == light);
    }
}
