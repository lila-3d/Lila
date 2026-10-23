#include <doctest/doctest.h>

#include "ops/Command.h"
#include "scene/Modifiers.h"
#include "scene/Node.h"

using namespace lila;
using json = nlohmann::json;

TEST_CASE("every modifier kind says what it does")
{
    std::vector<std::string> kinds = scene::modifiers::kinds();
    REQUIRE_FALSE(kinds.empty());
    for (const std::string& kind : kinds)
    {
        CAPTURE(kind);
        CHECK_FALSE(scene::modifiers::kindDoc(kind).empty());
    }
}

TEST_CASE("every modifier parameter has a doc")
{
    for (const std::string& kind : scene::modifiers::kinds())
        for (const scene::AttrDef& a : scene::types::find(kind + "Modifier")->attrs)
        {
            if (a.flags & scene::AttrHidden)
                continue;
            CAPTURE(kind);
            CAPTURE(a.name);
            CHECK_FALSE(a.doc.empty());
        }
}

TEST_CASE("addModifier and modifierKinds document the kinds")
{
    const std::string& doc = ops::find("addModifier")->doc;
    for (const std::string& kind : {"bevel", "mirror", "autoBevel", "decimate"})
    {
        CAPTURE(kind);
        CHECK(doc.find("- " + std::string(kind) + ": " + scene::modifiers::kindDoc(kind)) != std::string::npos);
    }
    json docs = ops::execute("modifierKinds", {{"docs", true}});
    REQUIRE(docs.is_object());
    CHECK(docs["solidify"].get<std::string>() == scene::modifiers::kindDoc("solidify"));
    CHECK(ops::execute("modifierKinds", json::object()).is_array());
}
