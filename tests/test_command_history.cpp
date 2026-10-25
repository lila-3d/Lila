#include <doctest/doctest.h>

#include "ui/CommandHistory.h"

using lila::ui::CommandHistory;

TEST_CASE("command history: dedupes, keeps newest last and caps")
{
    CommandHistory h(3);
    h.add("a");
    h.add("b");
    h.add("a");
    h.add("  ");
    CHECK(h.entries() == QStringList{"b", "a"});
    CHECK(h.last() == "a");
    h.add("c");
    h.add("d");
    CHECK(h.entries() == QStringList{"a", "c", "d"});

    h.setEntries({"x", "", "y", "x", "z", "w"});
    CHECK(h.entries() == QStringList{"x", "z", "w"});
}

TEST_CASE("command history: walk starts on the newest")
{
    CommandHistory h;
    CHECK(h.last().isEmpty());
    CHECK(h.older().isEmpty());
    CHECK(h.newer().isEmpty());

    h.setEntries({"one", "two", "three"});
    CHECK(h.older() == "two");
    CHECK(h.older() == "one");
    CHECK(h.older() == "one");
    CHECK(h.newer() == "two");
    CHECK(h.newer() == "three");
    CHECK(h.newer().isEmpty());
    CHECK(h.newer().isEmpty());
    CHECK(h.older() == "three");

    h.add("two");
    CHECK(h.entries() == QStringList{"one", "three", "two"});
    CHECK(h.older() == "three");
}
