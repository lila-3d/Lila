#include "ui/editor/LspClient.h"
#include "ui/editor/Ruff.h"

#include <doctest/doctest.h>

using namespace lila::ui;
using json = nlohmann::json;

TEST_CASE("framing round-trips and splits a stream")
{
    json a = {{"jsonrpc", "2.0"}, {"id", 1}, {"result", "h\xC3\xA9llo"}};
    json b = {{"jsonrpc", "2.0"}, {"method", "x"}};
    QByteArray stream = LspFramer::frame(a) + LspFramer::frame(b);

    LspFramer f;
    // Deliver byte by byte to exercise partial headers and bodies.
    std::vector<json> got;
    for (char c : stream)
    {
        f.feed(QByteArray(1, c));
        while (auto body = f.next())
            got.push_back(json::parse(body->toStdString()));
    }
    REQUIRE(got.size() == 2);
    CHECK(got[0] == a);
    CHECK(got[1] == b);
}

TEST_CASE("Content-Length counts bytes, not characters")
{
    QByteArray framed = LspFramer::frame(json("\xC3\xA9"));
    CHECK(framed.startsWith("Content-Length: 4\r\n\r\n"));
}

TEST_CASE("headers other than Content-Length are ignored")
{
    LspFramer f;
    f.feed("Content-Type: application/vscode-jsonrpc; charset=utf-8\r\ncontent-length: 2\r\n\r\n{}");
    auto body = f.next();
    REQUIRE(body);
    CHECK(*body == "{}");
    CHECK(!f.next());
}

TEST_CASE("LSP diagnostics convert")
{
    json d = json::parse(R"({"range":{"start":{"line":2,"character":4},"end":{"line":2,"character":9}},
        "message":"\"foo\" is not defined","severity":1,"code":"reportUndefinedVariable","source":"basedpyright","tags":[1]})");
    Diagnostic out = diagnosticFromLsp(d);
    CHECK(out.line == 2);
    CHECK(out.col == 4);
    CHECK(out.endCol == 9);
    CHECK(out.severity == Severity::Error);
    CHECK(out.code == "reportUndefinedVariable");
    CHECK(out.unnecessary);
}

TEST_CASE("ruff check output converts to UTF-16 columns")
{
    QByteArray out = R"([
        {"code":"F401","message":"`os` imported but unused","location":{"row":2,"column":8},"end_location":{"row":2,"column":10}},
        {"code":null,"message":"SyntaxError","location":{"row":1,"column":1},"end_location":{"row":1,"column":2}}
    ])";
    QStringList lines{QString::fromUtf8("s = '\xF0\x9F\x98\x80'"), "import os"};
    auto list = ruff::parseCheck(out, lines);
    REQUIRE(list.size() == 1);
    CHECK(list[0].line == 1);
    CHECK(list[0].col == 7);
    CHECK(list[0].endCol == 9);
    CHECK(list[0].source == "ruff");
    CHECK(list[0].unnecessary);

    QByteArray wide = R"([{"code":"E501","message":"long","location":{"row":1,"column":7},"end_location":{"row":1,"column":8}}])";
    auto w = ruff::parseCheck(wide, lines);
    REQUIRE(w.size() == 1);
    // Column 7 (1-based code points) is the closing quote, after a surrogate pair.
    CHECK(w[0].col == 7);
}
