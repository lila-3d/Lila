#include <doctest/doctest.h>

#include "ui/Credits.h"

#include <fstream>

using namespace lila::ui;
using json = nlohmann::json;

TEST_CASE("credits: links are https profiles on allowed platforms, corporate any https site")
{
    CHECK(credits::linkAllowed("https://github.com/someone", false));
    CHECK(credits::linkAllowed("https://www.artstation.com/someone/", false));
    CHECK_FALSE(credits::linkAllowed("https://github.com/", false));
    CHECK_FALSE(credits::linkAllowed("http://github.com/someone", false));
    CHECK_FALSE(credits::linkAllowed("https://github.com.evil.example/someone", false));
    CHECK_FALSE(credits::linkAllowed("https://user@github.com/someone", false));
    CHECK_FALSE(credits::linkAllowed("https://github.com:8443/someone", false));
    CHECK_FALSE(credits::linkAllowed("javascript:alert(1)", false));
    CHECK(credits::linkAllowed("https://studio.example", true));
    CHECK_FALSE(credits::linkAllowed("http://studio.example", true));
    CHECK(credits::linkHost("https://www.github.com/a") == "github.com");
}

TEST_CASE("credits: parse orders tiers, keeps Eternal links only and drops bad entries")
{
    const json file = json::parse(R"({
        "corporate": [{"name": "Studio", "logo": "logos/studio.png", "url": "https://studio.example"}, {"name": ""}],
        "pastSponsors": [{"name": "Old Co", "url": "https://old.example"}],
        "supporters": [
            {"name": "Sam", "tier": "supporter", "founder": true},
            {"name": "Eve", "tier": "eternal", "url": "https://github.com/eve"},
            {"name": "Leo", "tier": "legendary", "number": 2},
            {"name": "Lia", "tier": "legendary", "number": 1},
            {"name": "Pat", "tier": "patron", "url": "https://github.com/pat"},
            {"name": "Bad", "tier": "eternal", "url": "https://evil.example/x"},
            {"name": "Ben", "tier": "backer"},
            {"name": "  ", "tier": "patron"},
            "junk"
        ],
        "contributors": [{"name": "Kai", "role": "Engineering review and testing", "areas": ["OpenUSD", "Hydra", 3], "url": "https://github.com/kai"}],
        "testers": [{"name": "Zed"}, {"name": "Lu", "founding": true}]
    })");
    const credits::Credits c = credits::parse(file, true, "https://w.example/credits.json");

    REQUIRE(c.corporate.size() == 1);
    CHECK(c.corporate[0].url == "https://studio.example");
    CHECK(c.corporate[0].logo == "https://w.example/logos/studio.png");
    REQUIRE(c.pastSponsors.size() == 1);
    CHECK(c.pastSponsors[0].url.empty());

    REQUIRE(c.supporters.size() == 6);
    CHECK(c.supporters[0].tier == credits::Tier::Eternal);
    CHECK(c.supporters[1].tier == credits::Tier::Eternal);
    for (const credits::Supporter& s : c.supporters)
    {
        if (s.name == "Eve")
            CHECK(s.url == "https://github.com/eve");
        else
            CHECK(s.url.empty());
    }
    CHECK(c.supporters[2].name == "Lia");
    CHECK(c.supporters[3].name == "Leo");
    CHECK(c.supporters[4].name == "Pat");
    CHECK(c.supporters[5].name == "Sam");
    CHECK(c.supporters[5].founder);

    REQUIRE(c.contributors.size() == 1);
    CHECK(c.contributors[0].areas == std::vector<std::string>{"OpenUSD", "Hydra"});
    CHECK(c.contributors[0].url == "https://github.com/kai");

    REQUIRE(c.testers.size() == 2);
    CHECK(c.testers[0].name == "Lu");
    CHECK(c.testers[0].founding);
}

TEST_CASE("credits: the bundled snapshot never carries links")
{
    const json file = json::parse(R"({
        "corporate": [{"name": "Studio", "url": "https://studio.example"}],
        "supporters": [{"name": "Eve", "tier": "eternal", "url": "https://github.com/eve"}],
        "contributors": [{"name": "Kai", "url": "https://github.com/kai"}]
    })");
    const credits::Credits c = credits::parse(file, false);
    CHECK(c.corporate[0].url.empty());
    CHECK(c.supporters[0].url.empty());
    CHECK(c.contributors[0].url.empty());
}

TEST_CASE("credits: made possible by names Eternal and Mythic, then one Legendary in turn")
{
    const json file = json::parse(R"({"supporters": [
        {"name": "Eve", "tier": "eternal"}, {"name": "My", "tier": "mythic"},
        {"name": "L1", "tier": "legendary", "number": 1}, {"name": "L2", "tier": "legendary", "number": 2},
        {"name": "Pat", "tier": "patron"}]})");
    const credits::Credits c = credits::parse(file, false);
    CHECK(credits::madePossibleBy(c, 0) == std::vector<std::string>{"Eve", "My", "L1"});
    CHECK(credits::madePossibleBy(c, 1) == std::vector<std::string>{"Eve", "My", "L2"});
    CHECK(credits::madePossibleBy(c, 2) == std::vector<std::string>{"Eve", "My", "L1"});
    CHECK(credits::madePossibleBy(credits::parse(json::object(), false), 0).empty());
}

TEST_CASE("credits: the bundled snapshot parses")
{
    std::ifstream f(credits::bundledFile());
    REQUIRE(f.good());
    const json file = json::parse(f, nullptr, false);
    REQUIRE(file.is_object());
    CHECK(file.value("version", 0) == 1);
    for (const json& s : file.value("supporters", json::array()))
        CHECK_FALSE(s.contains("url"));
}
