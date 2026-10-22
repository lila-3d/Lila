// Runs real children on a pseudo console through PtyProcess and checks what reaches the screen model. No window.

#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "ui/Terminal.h"
#include "ui/terminal/VtScreen.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>

#include <string>
#include <vector>

using lila::term::VtScreen;
using lila::ui::PtyProcess;

namespace
{
/** Runs commandLine to completion, feeding its output to screen. */
bool run(VtScreen& screen, const QString& commandLine, int cols, int rows)
{
    PtyProcess pty;
    bool finished = false;
    QObject::connect(&pty, &PtyProcess::output, [&](const QByteArray& data) {
        screen.feed(std::string_view(data.constData(), size_t(data.size())));
        if (std::string reply = screen.takeResponses(); !reply.empty())
            pty.write(QByteArray::fromStdString(reply));
    });
    QObject::connect(&pty, &PtyProcess::finished, [&](int) { finished = true; });
    QString error;
    if (!pty.start(commandLine, QDir::tempPath(), QProcessEnvironment::systemEnvironment(), cols, rows, &error))
    {
        MESSAGE(error.toStdString());
        return false;
    }
    QElapsedTimer t;
    t.start();
    while (!finished && t.elapsed() < 20000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    // The console's last frame lands after the exit.
    QElapsedTimer tail;
    tail.start();
    while (tail.elapsed() < 750)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return finished;
}

std::vector<std::string> allLines(const VtScreen& s)
{
    std::vector<std::string> out;
    for (int i = -s.scrollbackSize(); i < s.rows(); ++i)
        out.push_back(s.lineText(i));
    return out;
}
} // namespace

#ifdef _WIN32
TEST_CASE("ConPTY output that scrolls off the top lands in the scrollback")
{
    VtScreen screen(80, 24, 10000);
    REQUIRE(run(screen, "cmd.exe /d /c \"for /l %i in (1,1,300) do @echo line %i\"", 80, 24));
    std::vector<std::string> lines = allLines(screen);
    CHECK(screen.scrollbackSize() >= 300 - 24);

    // Every line exactly once, in order.
    int next = 1;
    for (const std::string& l : lines)
        if (l == "line " + std::to_string(next))
            ++next;
        else
            CHECK_MESSAGE(l.rfind("line ", 0) != 0, "out of order: " << l);
    CHECK(next == 301);
}

TEST_CASE("cls through ConPTY clears the scrollback")
{
    VtScreen screen(80, 24, 10000);
    REQUIRE(run(screen, "cmd.exe /d /c \"for /l %i in (1,1,100) do @echo line %i\" & cls & echo after", 80, 24));
    for (const std::string& l : allLines(screen))
        CHECK_MESSAGE(l.rfind("line ", 0) != 0, "survived cls: " << l);
    CHECK(screen.lineText(0) == "after");
}
#endif

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
