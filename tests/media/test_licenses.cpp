#include <doctest/doctest.h>

#include "ui/LicensesDialog.h"

#include <QFile>

using namespace lila::ui;

TEST_CASE("licenses: the notices file parses into components with their section notes")
{
    QFile f(QString::fromUtf8(LILA_SOURCE_DIR) + "/THIRD-PARTY-NOTICES.md");
    REQUIRE(f.open(QIODevice::ReadOnly));
    const std::vector<licenses::Component> all = licenses::parse(QString::fromUtf8(f.readAll()));
    REQUIRE(all.size() > 30);
    auto find = [&](const QString& prefix) -> const licenses::Component* {
        for (const licenses::Component& c : all)
            if (c.name.startsWith(prefix))
                return &c;
        return nullptr;
    };
    const licenses::Component* ffmpeg = find("FFmpeg");
    REQUIRE(ffmpeg);
    CHECK(ffmpeg->license.startsWith("LGPL-2.1"));
    CHECK(ffmpeg->section.startsWith("Copyleft"));
    CHECK(ffmpeg->notes.contains("source code"));
    const licenses::Component* mini = find("miniaudio");
    REQUIRE(mini);
    CHECK(mini->section == "Permissive");
    // Header and rule rows are not components.
    CHECK_FALSE(find("Component"));
    CHECK_FALSE(find("---"));
}

TEST_CASE("licenses: a table row's cells, pipes trimmed")
{
    const auto c = licenses::parse("## Permissive\n\n| Component | License | Copyright | Source |\n|---|---|---|---|\n| A | MIT | Me | https://a |\n\nNote line.\n");
    REQUIRE(c.size() == 1);
    CHECK(c[0].name == "A");
    CHECK(c[0].license == "MIT");
    CHECK(c[0].source == "https://a");
    CHECK(c[0].notes == "Note line.");
}
