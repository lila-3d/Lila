// Throwaway harness: the ScriptEditor with Python up, driven by a few scripted steps that save screenshots and
// print what the language features returned. Usage: lila_editor_demo <outdir>

#include "ops/Command.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "io/SceneFile.h"
#include "python/Python.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/editor/CodeEditor.h"
#include "ui/editor/CompletionPopup.h"
#include "ui/editor/ScriptEditor.h"
#include "ui/editor/Tools.h"

#include "core/Paths.h"

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QStyleFactory>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QTimer>
#include <QToolTip>

#include <cstdlib>
#include <filesystem>
#include <cstdio>
#include <functional>

using namespace lila;

namespace
{
const char* kSample = R"(# %% Setup
import os
import lila
from lila import ops

GRID = 0x10  # spacing


@staticmethod
def build(count: int, name: str = "cube") -> list[str]:
    """Make `count` cubes in a row."""
    made = []
    for i in range(count):
        node = ops.polyCube(name=f"{name}_{i:02d}")
        made.append(node)
    return made


# %% Use it
class Rig(object):
    def __init__(self, root=None):
        self.root = root or "rig"

    def report(self):
        print(f"root is {self.root!r}", undefined_name)


cubes = build(3)
lila.ops.
)";

void typeText(QWidget* w, const QString& text)
{
    for (QChar c : text)
    {
        QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
        QApplication::sendEvent(w, &press);
    }
}

void key(QWidget* w, int k, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent press(QEvent::KeyPress, k, mods);
    QApplication::sendEvent(w, &press);
}

void save(QWidget* w, const QString& path)
{
    w->grab().save(path);
    std::printf("saved %s\n", path.toUtf8().constData());
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // Keep the demo out of the real user prefs.
    auto cfg = std::filesystem::temp_directory_path() / "lila_editor_demo";
    std::filesystem::create_directories(cfg);
#ifdef _WIN32
    _putenv_s("LILA_CONFIG_DIR", cfg.string().c_str());
#else
    setenv("LILA_CONFIG_DIR", cfg.string().c_str(), 1);
#endif
    mainq::init();
    PrefStore::get().start();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    io::registerCommands();
    std::printf("python init\n");
    bool pyOk = python::init({"lila"});
    std::printf("python ok %d\n", pyOk);

    QApplication app(argc, argv);
    std::printf("app created\n");
    app.setStyle(QStyleFactory::create("Fusion"));
    for (const QFileInfo& f : QDir(QString::fromStdWString((paths::resourcesDir() / "fonts").wstring())).entryInfoList({"*.ttf"}))
        QFontDatabase::addApplicationFont(f.absoluteFilePath());
    {
        auto gray = [](int v) { return QColor(v, v, v); };
        QPalette p;
        p.setColor(QPalette::Window, gray(48));
        p.setColor(QPalette::WindowText, gray(212));
        p.setColor(QPalette::Base, gray(36));
        p.setColor(QPalette::Text, gray(212));
        p.setColor(QPalette::Button, gray(58));
        p.setColor(QPalette::ButtonText, gray(212));
        p.setColor(QPalette::ToolTipBase, gray(28));
        p.setColor(QPalette::ToolTipText, gray(220));
        p.setColor(QPalette::Highlight, gray(105));
        std::printf("fonts loaded\n");
        app.setPalette(p);
        QFont ui("Inter");
        ui.setPointSizeF(9.0);
        app.setFont(ui);
    }
    mainq::setWakeup([&app] { QMetaObject::invokeMethod(&app, [] { mainq::drain(); }, Qt::QueuedConnection); });
    QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString(".");

    std::printf("creating editor\n");
    auto* editor = new ui::ScriptEditor;
    std::printf("editor created\n");
    QObject::connect(editor, &ui::ScriptEditor::showDocs, [](const QString& s) { std::printf("showDocs(%s)\n", s.toUtf8().constData()); });
    editor->resize(1100, 820);
    editor->show();
    editor->activateWindow();

    ui::CodeEditor* code = editor->newTab();
    code->setPlainText(kSample);
    code->setFocus();

    // Steps run one after another, each after its delay.
    struct Step
    {
        int delayMs;
        std::function<void()> fn;
    };
    std::vector<Step> steps;
    steps.push_back({9000, [&] {
                         std::printf("tools state %d: %s\n", int(ui::Tools::get().state()), ui::Tools::get().message().toUtf8().constData());
                         for (const ui::Diagnostic& d : code->diagnostics())
                             std::printf("diag %d:%d [%s %s] %s\n", d.line + 1, d.col + 1, d.source.toUtf8().constData(),
                                         d.code.toUtf8().constData(), d.message.toUtf8().constData());
                         save(editor, out + "/1_highlight.png");
                     }});
    steps.push_back({100, [&] {
                         QTextCursor c(code->document()->findBlockByNumber(code->blockCount() - 2));
                         c.movePosition(QTextCursor::EndOfBlock);
                         code->setTextCursor(c);
                         code->setFocus();
                         typeText(code, "sp");
                     }});
    steps.push_back({2500, [&] {
                         auto* popup = code->findChild<ui::CompletionPopup*>();
                         std::printf("popup visible %d entries %lld\n", popup->isVisible(), (long long)popup->entries().size());
                         int shown = 0;
                         for (const auto& e : popup->entries())
                             if (e.label.startsWith("sp") && shown++ < 12)
                                 std::printf("  %s (%s) interp=%d\n", e.label.toUtf8().constData(), e.detail.left(60).toUtf8().constData(),
                                             e.fromInterpreter);
                         save(popup, out + "/2_popup.png");
                         save(editor, out + "/2_editor.png");
                     }});
    steps.push_back({100, [&] {
                         key(code, Qt::Key_Escape);
                         QTextCursor c = code->textCursor();
                         c.select(QTextCursor::BlockUnderCursor);
                         c.movePosition(QTextCursor::StartOfBlock);
                         c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
                         c.insertText("os.path.join");
                         code->setTextCursor(c);
                         typeText(code, "(");
                     }});
    steps.push_back({2500, [&] {
                         QLabel* sig = nullptr;
                         for (QLabel* l : code->findChildren<QLabel*>())
                             if (l->windowFlags() & Qt::ToolTip)
                                 sig = l;
                         std::printf("signature visible %d: %s\n", sig && sig->isVisible(), sig ? sig->text().left(200).toUtf8().constData() : "");
                         if (sig)
                             save(sig, out + "/3_signature.png");
                         typeText(code, "\"a\"");
                     }});
    steps.push_back({500, [&] {
                         std::printf("last line: %s\n", code->document()->lastBlock().previous().text().toUtf8().constData());
                         // Comment toggle, duplicate, move line.
                         QTextCursor c(code->document()->findBlockByNumber(1));
                         code->setTextCursor(c);
                         key(code, Qt::Key_Slash, Qt::ControlModifier);
                         std::printf("toggled: %s\n", code->document()->findBlockByNumber(1).text().toUtf8().constData());
                         key(code, Qt::Key_Up);
                         key(code, Qt::Key_Slash, Qt::ControlModifier);
                         std::printf("untoggled: %s\n", code->document()->findBlockByNumber(1).text().toUtf8().constData());
                     }});
    steps.push_back({100, [&] {
                         QTextBlock b = code->document()->findBlockByNumber(27);
                         QTextCursor c(b);
                         c.setPosition(b.position() + 10);
                         code->setTextCursor(c);
                         key(code, Qt::Key_F12);
                     }});
    steps.push_back({1500, [&] {
                         std::printf("after F12 on build: line %d\n", code->textCursor().blockNumber() + 1);
                         save(editor, out + "/3_after_def.png");
                     }});
    steps.push_back({100, [&] {
                         QTextBlock b = code->document()->findBlockByNumber(12);
                         QTextCursor c(b);
                         c.setPosition(b.position() + 18);
                         QPoint p = code->cursorRect(c).center();
                         QCursor::setPos(code->viewport()->mapToGlobal(p));
                         QHelpEvent he(QEvent::ToolTip, p, code->viewport()->mapToGlobal(p));
                         QApplication::sendEvent(code->viewport(), &he);
                     }});
    steps.push_back({1500, [&] { std::printf("hover tooltip: %s\n", QToolTip::text().left(300).toUtf8().constData()); }});
    steps.push_back({300, [&] {
                         // Run a tab that raises, to see the clickable traceback.
                         ui::CodeEditor* t = editor->newTab();
                         t->setPlainText("x = 21 * 2\nprint('x is', x)\n\ndef boom():\n    return 1 / 0\n\nboom()\n");
                         editor->runCurrent(false);
                         t->setPlainText("import lila\nlila.ops.ls()\n");
                         editor->runCurrent(false);
                     }});
    steps.push_back({800, [&] { save(editor, out + "/4_run.png"); }});
    steps.push_back({100, [&] {
                         ui::CodeEditor* t = editor->currentEditor();
                         t->setPlainText("import os\nx = {  'a':1 }\ndef  f( a ):\n  return a\n");
                         t->formatDocument();
                         std::printf("formatted:\n%s\n", t->toPlainText().toUtf8().constData());
                         QTextCursor c(t->document()->findBlockByNumber(0));
                         c.movePosition(QTextCursor::EndOfBlock);
                         t->setTextCursor(c);
                         key(t, Qt::Key_F1);
                     }});
    steps.push_back({300, [&] { app.quit(); }});

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
    delete editor;
    mainq::setWakeup(nullptr);
    python::shutdown();
    PrefStore::get().stop();
    return rc;
}
