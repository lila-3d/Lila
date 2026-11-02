// Components: applied API schemas (fixtures/schemas, a codeless HealthAPI on PXR_PLUGINPATH_NAME) and custom attributes
// as addAttr attributes, through import, export and live stages.

#include "UsdTestUtil.h"

using namespace lila;
using namespace lila::scene;
using namespace lila::usdtest;

namespace
{
int attrOf(const std::string& object, const std::string& name)
{
    const int a = node(object).type().attrIndex(name);
    INFO(object, ".", name);
    REQUIRE(a >= 0);
    return a;
}

const AttrDef& defOf(const std::string& object, const std::string& name) { return node(object).type().attrs[size_t(attrOf(object, name))]; }

std::vector<std::string> schemasOf(const std::string& object)
{
    std::vector<std::string> out;
    for (const auto& [i, v] : node(object).elements(attrOf(object, "usdApiSchemas")))
        out.push_back(std::get<std::string>(v));
    return out;
}

void addHealth(const std::string& object)
{
    run("addAttr", {{"args", {object}}, {"longName", "usdApiSchemas"}, {"dataType", "stringArray"}});
    graph().setElements({node(object).id(), attrOf(object, "usdApiSchemas"), -1}, {PlugValue(std::string("HealthAPI"))});
}

bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

size_t count(const std::string& text, const std::string& part)
{
    size_t n = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + 1))
        ++n;
    return n;
}
} // namespace

TEST_CASE("usd components: a schema's attributes and custom ones import as attributes headed by the schema")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    run("importUsd", {{"args", {fixture("components.usda").generic_string()}}});

    CHECK(schemasOf("Crate") == std::vector<std::string>{"HealthAPI"});
    const AttrDef& max = defOf("Crate", "health:max");
    CHECK(max.type == PlugType::Float);
    CHECK(max.category == "HealthAPI");
    CHECK(max.niceName == "Max Health");
    CHECK(max.doc == "Most hit points.");
    CHECK(node("Crate").as<double>(attrOf("Crate", "health:max")) == doctest::Approx(50.0));

    const AttrDef& team = defOf("Crate", "health:team");
    CHECK(team.type == PlugType::Enum);
    CHECK(team.enumNames == std::vector<std::string>{"red", "blue"});
    CHECK(node("Crate").as<int64_t>(attrOf("Crate", "health:team")) == 1);

    CHECK((defOf("Crate", "health:tint").flags & AttrColor) != 0);
    CHECK(near(node("Crate").as<glm::dvec3>(attrOf("Crate", "health:tint")), {1, 0, 0}, 1e-6));
    CHECK(node("Crate").elements(attrOf("Crate", "health:stages")).size() == 2);

    CHECK(defOf("Crate", "studio:assetId").category.empty());
    CHECK(node("Crate").as<std::string>(attrOf("Crate", "studio:assetId")) == "CR_001");
    CHECK(near(node("Crate").as<glm::dvec3>(attrOf("Crate", "studio:offset")), {1, 2, 3}, 1e-9));

    CHECK(node("Barrel").type().attrIndex("usdApiSchemas") < 0);
}

TEST_CASE("usd components: export applies the schemas and writes the attributes, and they import again")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    run("importUsd", {{"args", {fixture("components.usda").generic_string()}}});
    graph().setValue({node("Crate").id(), attrOf("Crate", "health:max"), -1}, 75.0);
    addHealth("Barrel");
    run("addAttr", {{"args", {"Barrel"}}, {"longName", "studio:note"}, {"dataType", "string"}});
    graph().setValue({node("Barrel").id(), attrOf("Barrel", "studio:note"), -1}, std::string("dented"));

    const fs::path file = tempDir() / "components_out.usda";
    run("exportUsd", {{"args", {file.generic_string(), "World"}}});
    const std::string text = readAll(file);
    INFO(text);
    CHECK(count(text, "prepend apiSchemas = [\"HealthAPI\"]") == 2);
    CHECK(has(text, "float health:max = 75"));
    CHECK(has(text, "token health:team = \"blue\""));
    CHECK(has(text, "custom string studio:assetId = \"CR_001\""));
    CHECK(has(text, "custom string studio:note = \"dented\""));
    CHECK(!has(text, "usdApiSchemas"));
    // Fallback values stay unauthored.
    CHECK(!has(text, "health:tint"));

    fresh();
    run("importUsd", {{"args", {file.generic_string()}}});
    CHECK(node("Crate").as<double>(attrOf("Crate", "health:max")) == doctest::Approx(75.0));
    CHECK(schemasOf("Barrel") == std::vector<std::string>{"HealthAPI"});
    CHECK(node("Barrel").as<double>(attrOf("Barrel", "health:max")) == doctest::Approx(100.0));
    CHECK(node("Barrel").as<std::string>(attrOf("Barrel", "studio:note")) == "dented");
}

TEST_CASE("usd components: live stage edits write changed, added and removed components to the edit layer")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = tempDir() / "components_live";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::copy_file(fixture("components.usda"), dir / "components.usda");
    const std::string stage = run("usdStageOpen", {{"args", {(dir / "components.usda").generic_string()}}, {"report", true}})["stage"];
    auto edits = [&] { return run("usdStageInfo", {{"args", {stage}}, {"edits", true}})["edits"].get<std::string>(); };

    run("saveScene", {{"args", {(dir / "scene.lila").generic_string()}}});
    CHECK(!has(edits(), "over"));

    graph().setValue({node("Crate").id(), attrOf("Crate", "health:max"), -1}, 75.0);
    run("deleteAttr", {{"args", {"Crate.studio:assetId"}}});
    addHealth("Barrel");
    run("saveScene", {{"args", {(dir / "scene.lila").generic_string()}}});
    const std::string text = edits();
    INFO(text);
    CHECK(has(text, "float health:max = 75"));
    CHECK(has(text, "studio:assetId = None"));
    CHECK(has(text, "prepend apiSchemas = [\"HealthAPI\"]"));
    CHECK(!has(text, "studio:offset"));

    // Taking the schema back off deletes it from the prim.
    graph().setElements({node("Barrel").id(), attrOf("Barrel", "usdApiSchemas"), -1}, std::vector<PlugValue>{});
    run("saveScene", {{"args", {(dir / "scene.lila").generic_string()}}});
    CHECK(has(edits(), "delete apiSchemas = [\"HealthAPI\"]"));
}
