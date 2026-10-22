// Throwaway harness: the API reference and terminal panels with Python up, grabbed to PNGs.
// Usage: lila_panels_demo <outdir>

#include "cmds/Command.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "io/SceneFile.h"
#include "python/Python.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/ApiReference.h"
#include "ui/Terminal.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>

using namespace lila;

namespace
{
void save(QWidget* w, const QString& path)
{
    w->grab().save(path);
    std::printf("saved %s\n", path.toUtf8().constData());
}

void dumpScreen(const ui::TerminalView* view)
{
    const term::VtScreen& s = view->screen();
    std::printf("--- screen %dx%d cursor %d,%d history %d\n", s.cols(), s.rows(), s.cursorX(), s.cursorY(), s.scrollbackSize());
    for (int y = 0; y < s.rows(); ++y)
        if (std::string line = s.lineText(y); !line.empty())
            std::printf("|%s\n", line.c_str());
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto cfg = std::filesystem::temp_directory_path() / "lila_panels_demo";
    std::filesystem::create_directories(cfg);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", cfg.string().c_str());
#else
    setenv("LILA_CONFIG_DIR", cfg.string().c_str(), 1);
#endif

    mainq::init();
    PrefStore::get().start();
    cmds::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    io::registerCommands();
    bool pyOk = python::init({"lila"});
    std::printf("python ok %d\n", pyOk);

    QApplication app(argc, argv);
    ui::applyTheme(app);
    mainq::setWakeup([&app] { QMetaObject::invokeMethod(&app, [] { mainq::drain(); }, Qt::QueuedConnection); });
    QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString(".");
    QDir().mkpath(out);

    auto* api = new ui::ApiReference;
    api->resize(1000, 720);
    api->show();
    QObject::connect(api, &ui::ApiReference::insertCode, [](const QString& code) { std::printf("insertCode: %s", code.toUtf8().constData()); });

    auto* terminal = new ui::TerminalPanel;
    terminal->resize(900, 520);

    struct Step
    {
        int delayMs;
        std::function<void()> fn;
    };
    std::vector<Step> steps;
    steps.push_back({500, [&] { save(api, out + "/api_1_tree.png"); }});
    for (const char* sym : {"parent", "lila.math.Vector.get_safe_normal", "Node.translate", "lila.nodes.Node", "lila.math", "nonexistent.thing"})
    {
        steps.push_back({100, [&, sym] {
                             bool found = api->showSymbol(sym);
                             std::printf("showSymbol(%s) -> %d\n", sym, found);
                             save(api, out + "/api_sym_" + QString(sym).replace('.', '_') + ".png");
                         }});
    }
    steps.push_back({100, [&] {
                         api->findChild<QLineEdit*>()->setText("vecnorm");
                         save(api, out + "/api_search_vecnorm.png");
                         api->findChild<QLineEdit*>()->setText("");
                         api->showSymbol("parent");
                         for (QPushButton* b : api->findChildren<QPushButton*>())
                             if (b->text() == "Insert Example")
                                 b->click();
                     }});

    steps.push_back({100, [&] { terminal->show(); }});
    steps.push_back({4000, [&] {
                         dumpScreen(terminal->currentTerminal());
                         terminal->currentTerminal()->sendText("dir\r");
                     }});
    steps.push_back({2000, [&] {
                         terminal->currentTerminal()->sendText("python --version; python -c \"import sys; print(sys.path[1])\"\r");
                     }});
    steps.push_back({2500, [&] {
                         dumpScreen(terminal->currentTerminal());
                         terminal->currentTerminal()->setFocus();
                         save(terminal, out + "/term_1.png");
                         terminal->newTerminal();
                     }});
    steps.push_back({3000, [&] {
                         terminal->currentTerminal()->sendText(
                             "$e=[char]27; Write-Host \"$e[1mbold$e[0m $e[2mdim$e[0m $e[4munder$e[0m $e[7minverse$e[0m $e[31mred$e[32mgreen$e[34mblue$e[0m "
                             "$e[48;5;250;30m light bg $e[0m\"\r");
                     }});
    steps.push_back({1500, [&] {
                         dumpScreen(terminal->currentTerminal());
                         save(terminal, out + "/term_2.png");
                         terminal->resize(700, 400);
                     }});
    steps.push_back({1500, [&] {
                         dumpScreen(terminal->currentTerminal());
                         save(terminal, out + "/term_3_resized.png");
                         terminal->currentTerminal()->sendText("exit\r");
                     }});
    steps.push_back({1500, [&] {
                         dumpScreen(terminal->currentTerminal());
                         save(terminal, out + "/term_4_exited.png");
                         app.quit();
                     }});

    std::function<void(size_t)> runStep = [&](size_t i) {
        if (i >= steps.size())
            return;
        QTimer::singleShot(steps[i].delayMs, [&, i] {
            steps[i].fn();
            runStep(i + 1);
        });
    };
    runStep(0);

    int rc = app.exec();
    delete terminal;
    delete api;
    mainq::setWakeup(nullptr);
    python::shutdown();
    PrefStore::get().stop();
    return rc;
}
