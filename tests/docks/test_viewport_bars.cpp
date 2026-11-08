#include <doctest/doctest.h>

#include "core/PrefStore.h"
#include "ui/ViewportHeader.h"
#include "ui/ViewportHeaderConfig.h"
#include "ui/ViewportWidget.h"

#include <QMenu>
#include <QToolButton>

using namespace lila::ui;

namespace
{
QToolButton* chevronOf(QWidget& header)
{
    for (QToolButton* b : header.findChildren<QToolButton*>(Qt::FindDirectChildrenOnly))
        if (b->toolTip() == "More viewport options")
            return b;
    return nullptr;
}

QToolButton* buttonWithText(QWidget& header, const QString& prefix)
{
    for (QToolButton* b : header.findChildren<QToolButton*>())
        if (b->text().startsWith(prefix))
            return b;
    return nullptr;
}
} // namespace

TEST_CASE("viewport header drops labels, then folds items into the chevron as it narrows")
{
    ViewportWidget view;
    ViewportHeader header(&view);
    header.resize(1600, header.sizeHint().height());
    header.show();
    QToolButton* chevron = chevronOf(header);
    REQUIRE(chevron);
    QToolButton* snap = buttonWithText(header, "Snap");
    QToolButton* symmetry = buttonWithText(header, "Symmetry");
    REQUIRE(snap);
    REQUIRE(symmetry);
    CHECK_FALSE(chevron->isVisible());
    CHECK(snap->toolButtonStyle() == Qt::ToolButtonTextBesideIcon);

    header.resize(520, header.height());
    CHECK(snap->toolButtonStyle() == Qt::ToolButtonIconOnly);

    header.resize(260, header.height());
    CHECK(chevron->isVisible());
    Q_EMIT chevron->menu()->aboutToShow();
    CHECK_FALSE(chevron->menu()->actions().isEmpty());
}

TEST_CASE("a saved viewport header layout picks up entries added to the defaults, but not ones the user removed")
{
    using nlohmann::json;
    auto items = [](const json& c) {
        for (const json& e : c["left"])
            if (e.is_object() && e.value("menu", std::string()) == "Display")
                return e["items"];
        return json();
    };
    auto has = [](const json& list, const std::string& id) { return std::find(list.begin(), list.end(), json(id)) != list.end(); };

    // Saved before Joint Size existed: no known list, and the Display menu without it.
    json saved = viewportHeader::defaultConfig();
    for (json& e : saved["left"])
        if (e.is_object() && e.value("menu", std::string()) == "Display")
        {
            e["items"].erase(e["items"].end() - 1);
            e["items"].erase(e["items"].end() - 1);
        }
    saved["right"].erase(saved["right"].begin());
    lila::PrefStore::get().set("general", viewportHeader::kPref, saved);
    json merged = viewportHeader::config();
    CHECK(has(items(merged), "jointSize"));
    CHECK(items(merged).size() == items(viewportHeader::defaultConfig()).size());
    CHECK(merged["right"].front() == json("snap"));

    // Removed after the layout knows about it: stays removed.
    json edited = merged;
    edited.erase("known");
    for (json& e : edited["left"])
        if (e.is_object() && e.value("menu", std::string()) == "Display")
            e["items"].erase(e["items"].end() - 1);
    viewportHeader::setConfig(edited);
    CHECK_FALSE(has(items(viewportHeader::config()), "jointSize"));

    lila::PrefStore::get().remove("general", viewportHeader::kPref);
}
