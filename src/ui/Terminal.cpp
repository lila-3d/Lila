#include "ui/Terminal.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "mcp/AgentIntegration.h"
#include "ui/AgentSetup.h"
#include "ui/PythonBridge.h"
#include "ui/Theme.h"
#include "ui/editor/EditorPalette.h"
#include "ui/terminal/VtInput.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <QWinEventNotifier>
#else
#include <QFile>

#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

namespace lila::ui
{
namespace
{
constexpr int kDefaultFg = 204;
constexpr int kDefaultBg = 28;
constexpr int kSelectionBg = 105;
constexpr int kWheelLine = 40; // angle delta per scrolled line: three lines per notch
constexpr int kWheelNotch = 120;
// A program that never ends its synchronized frame still gets painted.
constexpr int kSyncTimeoutMs = 150;

constexpr term::Rgb kAnsiDark[16] = {
    {0x00, 0x00, 0x00}, {0xcd, 0x31, 0x31}, {0x0d, 0xbc, 0x79}, {0xe5, 0xe5, 0x10}, {0x24, 0x72, 0xc8}, {0xbc, 0x3f, 0xbc},
    {0x11, 0xa8, 0xcd}, {0xe5, 0xe5, 0xe5}, {0x66, 0x66, 0x66}, {0xf1, 0x4c, 0x4c}, {0x23, 0xd1, 0x8b}, {0xf5, 0xf5, 0x43},
    {0x3b, 0x8e, 0xea}, {0xd6, 0x70, 0xd6}, {0x29, 0xb8, 0xdb}, {0xe5, 0xe5, 0xe5},
};
constexpr term::Rgb kAnsiLight[16] = {
    {0x00, 0x00, 0x00}, {0xcd, 0x31, 0x31}, {0x10, 0x7c, 0x10}, {0x94, 0x98, 0x00}, {0x04, 0x51, 0xa5}, {0xbc, 0x05, 0xbc},
    {0x05, 0x98, 0xbc}, {0x55, 0x55, 0x55}, {0x66, 0x66, 0x66}, {0xcd, 0x31, 0x31}, {0x14, 0xce, 0x14}, {0xb5, 0xba, 0x00},
    {0x04, 0x51, 0xa5}, {0xbc, 0x05, 0xbc}, {0x05, 0x98, 0xbc}, {0xa5, 0xa5, 0xa5},
};

bool pref(const char* key, bool fallback)
{
    return PrefStore::get().value<bool>("general", key, fallback);
}

#ifdef _WIN32
// Resolved at runtime so the build doesn't depend on the SDK's NTDDI level, and old Windows fails cleanly.
using HPCON_ = void*;
using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON_*);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(HPCON_, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(HPCON_);
constexpr DWORD_PTR kAttrPseudoConsole = 0x00020016; // PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE

struct ConPty
{
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;
};

const ConPty& conPty()
{
    static ConPty api = [] {
        ConPty a;
        if (HMODULE k = GetModuleHandleW(L"kernel32.dll"))
        {
            a.create = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(k, "CreatePseudoConsole"));
            a.resize = reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(k, "ResizePseudoConsole"));
            a.close = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(k, "ClosePseudoConsole"));
        }
        return a;
    }();
    return api;
}

COORD coord(int cols, int rows)
{
    return COORD{SHORT(std::clamp(cols, 1, 32767)), SHORT(std::clamp(rows, 1, 32767))};
}
#endif

QString native(const std::filesystem::path& p)
{
    return QDir::toNativeSeparators(QString::fromStdWString(p.wstring()));
}

bool isWordChar(char32_t c)
{
    static const std::u32string kBreaks = U" \t()[]{}<>'\"`|;,";
    return c != 0 && kBreaks.find(c) == std::u32string::npos;
}
} // namespace

// PtyProcess

#ifdef _WIN32
struct PtyProcess::Impl
{
    PtyProcess* q = nullptr;
    HPCON_ pc = nullptr;
    HANDLE inWrite = nullptr;
    HANDLE outRead = nullptr;
    PROCESS_INFORMATION pi{};
    std::thread reader;
    QWinEventNotifier* notifier = nullptr;
    bool running = false;

    void closeConsole()
    {
        if (pc)
        {
            conPty().close(pc);
            pc = nullptr;
        }
    }

    void stop()
    {
        delete notifier;
        notifier = nullptr;
        closeConsole();
        if (inWrite)
        {
            CloseHandle(inWrite);
            inWrite = nullptr;
        }
        if (reader.joinable())
        {
            // Closing the console breaks the pipe, which ends the read; this covers a read already in flight.
            CancelSynchronousIo(HANDLE(reader.native_handle()));
            reader.join();
        }
        if (outRead)
        {
            CloseHandle(outRead);
            outRead = nullptr;
        }
        if (pi.hProcess)
        {
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT)
                TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            pi = {};
        }
        running = false;
    }
};

bool PtyProcess::start(const QString& commandLine, const QString& workingDir, const QProcessEnvironment& env, int cols, int rows,
                       QString* error)
{
    LILA_ZONE();

    auto fail = [error](const QString& msg) {
        if (error)
            *error = msg;
        return false;
    };
    m->stop();
    const ConPty& api = conPty();
    if (!api.create)
        return fail("ConPTY needs Windows 10 1809 or later");

    HANDLE inRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &m->inWrite, nullptr, 0) || !CreatePipe(&m->outRead, &outWrite, nullptr, 0))
        return fail("CreatePipe failed");
    HRESULT hr = api.create(coord(cols, rows), inRead, outWrite, 0, &m->pc);
    // The console holds its own references to these ends.
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (FAILED(hr))
    {
        m->stop();
        return fail(QString("CreatePseudoConsole failed (0x%1)").arg(quint32(hr), 8, 16, QChar('0')));
    }

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrs, 0, kAttrPseudoConsole, m->pc, sizeof(m->pc), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;
    // Null std handles: otherwise a parent with redirected stdio leaks those into the child instead of the console.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;

    QStringList vars = env.toStringList();
    std::sort(vars.begin(), vars.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    std::wstring block;
    for (const QString& v : vars)
    {
        block += v.toStdWString();
        block += L'\0';
    }
    block += L'\0';

    std::wstring cmd = commandLine.toStdWString();
    std::wstring cwd = QDir::toNativeSeparators(workingDir).toStdWString();
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                             block.data(), cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &m->pi);
    DWORD err = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    if (!ok)
    {
        m->stop();
        return fail(QString("could not run %1 (error %2)").arg(commandLine).arg(err));
    }
    m->running = true;

    m->reader = std::thread([q = this, h = m->outRead] {
        LILA_THREAD_NAME("Terminal reader");
        char buf[1 << 16];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0)
        {
            QByteArray data(buf, int(n));
            QMetaObject::invokeMethod(q, [q, data] { emit q->output(data); }, Qt::QueuedConnection);
        }
    });

    m->notifier = new QWinEventNotifier(m->pi.hProcess, this);
    connect(m->notifier, &QWinEventNotifier::activated, this, [this] {
        DWORD code = 0;
        GetExitCodeProcess(m->pi.hProcess, &code);
        m->running = false;
        m->notifier->setEnabled(false);
        // Lets the console flush its last frame and end the reader with EOF.
        m->closeConsole();
        emit finished(int(code));
    });
    return true;
}

void PtyProcess::write(const QByteArray& data)
{
    if (!m->inWrite || data.isEmpty())
        return;
    DWORD written = 0;
    WriteFile(m->inWrite, data.constData(), DWORD(data.size()), &written, nullptr);
}

void PtyProcess::resize(int cols, int rows)
{
    if (m->pc)
        conPty().resize(m->pc, coord(cols, rows));
}

#else
// forkpty sketch for Linux/macOS: same shape as the ConPTY path, untested.
struct PtyProcess::Impl
{
    PtyProcess* q = nullptr;
    int master = -1;
    pid_t pid = -1;
    std::thread reader;
    std::thread waiter;
    bool running = false;

    void stop()
    {
        if (pid > 0 && running)
            ::kill(pid, SIGHUP);
        if (master >= 0)
        {
            ::close(master);
            master = -1;
        }
        if (reader.joinable())
            reader.join();
        if (waiter.joinable())
            waiter.join();
        pid = -1;
        running = false;
    }
};

bool PtyProcess::start(const QString& commandLine, const QString& workingDir, const QProcessEnvironment& env, int cols, int rows,
                       QString* error)
{
    LILA_ZONE();

    m->stop();
    winsize ws{};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    QByteArray cmd = commandLine.toUtf8();
    QByteArray cwd = QFile::encodeName(workingDir);
    QList<QByteArray> vars;
    for (const QString& v : env.toStringList())
        vars.push_back(v.toUtf8());

    pid_t pid = forkpty(&m->master, nullptr, nullptr, &ws);
    if (pid < 0)
    {
        if (error)
            *error = "forkpty failed";
        return false;
    }
    if (pid == 0)
    {
        if (!cwd.isEmpty())
            (void)chdir(cwd.constData());
        std::vector<char*> envp;
        for (QByteArray& v : vars)
            envp.push_back(v.data());
        envp.push_back(nullptr);
        const char* argv[] = {"/bin/sh", "-c", cmd.constData(), nullptr};
        execve("/bin/sh", const_cast<char**>(argv), envp.data());
        _exit(127);
    }
    m->pid = pid;
    m->running = true;
    m->reader = std::thread([q = this, fd = m->master] {
        char buf[1 << 16];
        for (;;)
        {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            QByteArray data(buf, int(n));
            QMetaObject::invokeMethod(q, [q, data] { emit q->output(data); }, Qt::QueuedConnection);
        }
    });
    m->waiter = std::thread([q = this, pid] {
        int status = 0;
        waitpid(pid, &status, 0);
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        QMetaObject::invokeMethod(q, [q, code] {
            q->m->running = false;
            emit q->finished(code);
        }, Qt::QueuedConnection);
    });
    return true;
}

void PtyProcess::write(const QByteArray& data)
{
    if (m->master >= 0)
        (void)::write(m->master, data.constData(), size_t(data.size()));
}

void PtyProcess::resize(int cols, int rows)
{
    if (m->master < 0)
        return;
    winsize ws{};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ioctl(m->master, TIOCSWINSZ, &ws);
}
#endif

PtyProcess::PtyProcess(QObject* parent) : QObject(parent), m(std::make_unique<Impl>())
{
    m->q = this;
}

PtyProcess::~PtyProcess()
{
    m->stop();
}

bool PtyProcess::running() const
{
    return m->running;
}

// TerminalView

TerminalView::TerminalView(QWidget* parent)
    : QAbstractScrollArea(parent), mScreen(80, 24, PrefStore::get().value<int>("general", "/terminal/scrollback", 10000))
{
    setFocusPolicy(Qt::StrongFocus);
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setAttribute(Qt::WA_InputMethodEnabled);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    // Motion with no button held, for programs in any-event mouse tracking (1003).
    viewport()->setMouseTracking(true);
    mSyncTimeout = new QTimer(this);
    mSyncTimeout->setSingleShot(true);
    mSyncTimeout->setInterval(kSyncTimeoutMs);
    connect(mSyncTimeout, &QTimer::timeout, viewport(), qOverload<>(&QWidget::update));
    // Every key belongs to the shell, not to app hotkeys.
    setProperty("hotkeysOff", true);
    viewport()->setProperty("hotkeysOff", true);
    updateFontMetrics();
    updateScrollBar();
    palette::onLookChanged(this, [this] { viewport()->update(); });
}

TerminalView::~TerminalView() = default;

QString TerminalView::shellCommand()
{
    std::string pref = PrefStore::get().value<std::string>("general", "/terminal/shell", "");
    if (!pref.empty())
        return QString::fromStdString(pref);
#ifdef _WIN32
    for (const char* exe : {"pwsh.exe", "powershell.exe"})
    {
        QString path = QStandardPaths::findExecutable(exe);
        if (!path.isEmpty())
            return "\"" + QDir::toNativeSeparators(path) + "\" -NoLogo";
    }
    return qEnvironmentVariable("COMSPEC", "cmd.exe");
#else
    return qEnvironmentVariable("SHELL", "/bin/sh");
#endif
}

QProcessEnvironment TerminalView::shellEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
#ifdef _WIN32
    const QString sep = ";";
#else
    const QString sep = ":";
#endif
    QStringList path;
    if (std::filesystem::path home = pybridge::pythonHome(); !home.empty())
    {
#ifdef _WIN32
        path << native(home) << native(home / "Scripts");
#else
        path << native(home / "bin");
#endif
    }
    // lilabatch sits next to lila, so scripts and agents in this shell can run headless Lila.
    path << native(paths::executableDir());
    if (QString old = env.value("PATH"); !old.isEmpty())
        path << old;
    env.insert("PATH", path.join(sep));
    for (auto& [name, value] : mcp::agents::shellVariables())
        env.insert(QString::fromStdString(name), QString::fromUtf8(value.c_str()));

    QString pyPath = native(paths::pythonDir());
    if (QString old = env.value("PYTHONPATH"); !old.isEmpty())
        pyPath += sep + old;
    env.insert("PYTHONPATH", pyPath);
    env.insert("TERM", "xterm-256color");
    return env;
}

bool TerminalView::startShell()
{
    LILA_ZONE();

    delete mPty;
    mPty = new PtyProcess(this);
    connect(mPty, &PtyProcess::output, this, &TerminalView::onOutput);
    connect(mPty, &PtyProcess::finished, this, &TerminalView::onFinished);

    QString cmd = shellCommand();
    QString first = cmd.startsWith('"') ? cmd.mid(1, cmd.indexOf('"', 1) - 1) : cmd.section(' ', 0, 0);
    mShellName = QFileInfo(first).completeBaseName();
    if (mShellName.isEmpty())
        mShellName = "shell";

    Workspace& ws = Workspace::get();
    QString cwd = ws.isOpen() ? native(ws.root()) : QDir::homePath();
    QString error;
    mExited = false;
    if (!mPty->start(cmd, cwd, shellEnvironment(), mScreen.cols(), mScreen.rows(), &error))
    {
        mExited = true;
        log::error("terminal: {}", error.toStdString());
        mScreen.feed(("\x1b[1m" + error + "\x1b[0m\r\n").toUtf8().toStdString());
        viewport()->update();
        return false;
    }
    agentSetup::maybePrompt(this);
    return true;
}

bool TerminalView::running() const
{
    return mPty && mPty->running();
}

void TerminalView::onOutput(const QByteArray& bytes)
{
    LILA_ZONE();

    int64_t before = mScreen.droppedLines() + mScreen.scrollbackSize();
    mScreen.feed(std::string_view(bytes.constData(), size_t(bytes.size())));
    if (std::string reply = mScreen.takeResponses(); !reply.empty())
        send(QByteArray::fromStdString(reply));
    if (pref("/terminal/scrollOnOutput", false))
        mViewOffset = 0;
    // Hold the view still while the user is reading history.
    if (mViewOffset > 0)
    {
        int64_t added = mScreen.droppedLines() + mScreen.scrollbackSize() - before;
        mViewOffset = int(std::clamp<int64_t>(mViewOffset + added, 0, mScreen.scrollbackSize()));
    }
    updateScrollBar();
    repaintScreen();
}

void TerminalView::repaintScreen()
{
    if (mScreen.synchronizedOutput())
    {
        if (!mSyncTimeout->isActive())
            mSyncTimeout->start();
        return;
    }
    mSyncTimeout->stop();
    viewport()->update();
}

void TerminalView::onFinished(int exitCode)
{
    mExited = true;
    // A program that died mid-session can't turn its input modes back off.
    std::string msg = "\x1b[?1049l\x1b[?9l\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1004l\x1b[?1006l\x1b[?2026l\x1b[?2004l\x1b[?1l"
                      "\r\n\x1b[2m[process exited with code " +
                      std::to_string(exitCode) + "; press Enter to restart]\x1b[0m\r\n";
    // Let the console's final output land first.
    QTimer::singleShot(50, this, [this, msg] {
        mScreen.feed(msg);
        updateScrollBar();
        viewport()->update();
    });
    emit exited(exitCode);
}

void TerminalView::send(const QByteArray& bytes)
{
    if (mPty)
        mPty->write(bytes);
}

void TerminalView::sendText(const QString& text)
{
    send(text.toUtf8());
}

void TerminalView::updateFontMetrics()
{
    mFont = monoFont();
    mFont.setStyleStrategy(QFont::PreferAntialias);
    mBold = mFont;
    mBold.setBold(true);
    mItalic = mFont;
    mItalic.setItalic(true);
    mBoldItalic = mBold;
    mBoldItalic.setItalic(true);
    QFontMetricsF fm(mFont);
    mCellW = fm.horizontalAdvance(QLatin1Char('M'));
    mCellH = int(std::ceil(fm.height()));
    mAscent = int(std::ceil(fm.ascent()));
}

void TerminalView::updateGrid()
{
    QSize vp = viewport()->size();
    int cols = std::max(2, int((vp.width() - 2 * mPad) / mCellW));
    int rows = std::max(1, (vp.height() - 2 * mPad) / mCellH);
    if (cols == mScreen.cols() && rows == mScreen.rows())
        return;
    mScreen.resize(cols, rows);
    if (mPty)
        mPty->resize(cols, rows);
    updateScrollBar();
}

void TerminalView::updateScrollBar()
{
    int history = mScreen.altScreen() ? 0 : mScreen.scrollbackSize();
    mViewOffset = std::clamp(mViewOffset, 0, history);
    mUpdatingBar = true;
    QScrollBar* bar = verticalScrollBar();
    bar->setRange(0, history);
    bar->setPageStep(mScreen.rows());
    bar->setSingleStep(1);
    bar->setValue(history - mViewOffset);
    mUpdatingBar = false;
}

void TerminalView::scrollContentsBy(int, int)
{
    if (mUpdatingBar)
        return;
    mViewOffset = verticalScrollBar()->maximum() - verticalScrollBar()->value();
    viewport()->update();
}

void TerminalView::scrollLines(int lines)
{
    QScrollBar* bar = verticalScrollBar();
    bar->setValue(bar->value() + lines);
}

void TerminalView::scrollToBottom()
{
    if (mViewOffset == 0)
        return;
    mViewOffset = 0;
    updateScrollBar();
    viewport()->update();
}

void TerminalView::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    updateGrid();
    if (mFindBar)
        mFindBar->move(std::max(0, viewport()->width() - mFindBar->width() - 8), 6);
}

int TerminalView::firstVisibleLine() const
{
    return mScreen.altScreen() ? 0 : -mViewOffset;
}

int64_t TerminalView::absLine(int historyIndex) const
{
    return mScreen.droppedLines() + mScreen.scrollbackSize() + historyIndex;
}

int TerminalView::historyIndex(int64_t line) const
{
    return int(line - mScreen.droppedLines() - mScreen.scrollbackSize());
}

TerminalView::Pos TerminalView::cellAt(const QPoint& p) const
{
    int row = std::clamp((p.y() - mPad) / mCellH, 0, mScreen.rows() - 1);
    int col = std::clamp(int(std::lround((p.x() - mPad) / mCellW)), 0, mScreen.cols());
    return {absLine(firstVisibleLine() + row), col};
}

bool TerminalView::selected(int64_t line, int col) const
{
    Pos a = std::min(mSelAnchor, mSelEnd), b = std::max(mSelAnchor, mSelEnd);
    Pos p{line, col};
    return p >= a && p < b;
}

void TerminalView::paintEvent(QPaintEvent*)
{
    LILA_ZONE();

    QPainter p(viewport());

    // Monotone: explicit colors are compressed into the readable range and kept clear of their background.
    auto foreground = [](int fg, int bg) {
        int g = 96 + fg * (255 - 96) / 255;
        if (std::abs(g - bg) < 90)
            g = bg > 127 ? std::max(0, bg - 130) : std::min(255, bg + 130);
        return g;
    };
    const palette::Look look = palette::currentLook();
    std::array<QColor, 256> shades;
    for (int v = 0; v < 256; ++v)
        shades[v] = themeShade(v);
    const QColor defaultFg = shades[kDefaultFg], defaultBg = shades[kDefaultBg];
    p.fillRect(viewport()->rect(), defaultBg);
    const term::Rgb* ansi = look.light ? kAnsiLight : kAnsiDark;
    auto rgb = [ansi](term::Color c) {
        term::Rgb v = term::rgbFromColor(c, ansi);
        return QColor(v.r, v.g, v.b);
    };

    struct Style
    {
        QColor fg, bg;
        bool bold, italic, underline, strike;
        bool operator==(const Style&) const = default;
    };

    bool showCursor = mScreen.cursorVisible();
    int first = firstVisibleLine();
    bool hasSel = hasSelection();
    for (int r = 0; r < mScreen.rows(); ++r)
    {
        int idx = first + r;
        if (idx < -mScreen.scrollbackSize() || idx >= mScreen.rows())
            continue;
        const term::Line& line = mScreen.historyLine(idx);
        int64_t abs = absLine(idx);
        int y = mPad + r * mCellH;
        bool cursorRow = showCursor && idx == mScreen.cursorY() && mFocused && !mExited;

        auto monoStyle = [&](const term::Cell& c, bool sel, bool cursor) {
            int bg = c.bg == term::kDefaultColor ? kDefaultBg : term::grayFromColor(c.bg);
            int fg = c.fg == term::kDefaultColor ? kDefaultFg : foreground(term::grayFromColor(c.fg), bg);
            if (c.attrs & term::Bold)
                fg = std::min(255, fg + 30);
            if (c.attrs & term::Dim)
                fg = (fg + bg) / 2;
            if (c.attrs & term::Inverse)
                std::swap(fg, bg);
            if (c.attrs & term::Hidden)
                fg = bg;
            if (sel)
            {
                bg = kSelectionBg;
                fg = 255;
            }
            if (cursor)
            {
                bg = c.attrs & term::Inverse ? kDefaultBg : kDefaultFg;
                fg = kDefaultBg;
            }
            return std::pair{shades[fg], shades[bg]};
        };
        auto colourStyle = [&](const term::Cell& c, bool sel, bool cursor) {
            QColor bg = c.bg == term::kDefaultColor ? defaultBg : rgb(c.bg);
            term::Color fgc = c.fg;
            // Bold brightens the eight basic colors, as most terminals do.
            if ((c.attrs & term::Bold) && fgc >= 0 && fgc < 8)
                fgc += 8;
            QColor fg = fgc == term::kDefaultColor ? defaultFg : rgb(fgc);
            if (c.attrs & term::Dim)
                fg = QColor((fg.red() + bg.red()) / 2, (fg.green() + bg.green()) / 2, (fg.blue() + bg.blue()) / 2);
            if (c.attrs & term::Inverse)
                std::swap(fg, bg);
            if (c.attrs & term::Hidden)
                fg = bg;
            if (sel)
                bg = palette::selection;
            if (cursor)
            {
                bg = c.attrs & term::Inverse ? defaultBg : defaultFg;
                fg = defaultBg;
            }
            return std::pair{fg, bg};
        };
        auto styleAt = [&](int x) {
            const term::Cell& c = line[x];
            bool sel = hasSel && selected(abs, x);
            bool cursor = cursorRow && x == mScreen.cursorX();
            auto [fg, bg] = look.monotone ? monoStyle(c, sel, cursor) : colourStyle(c, sel, cursor);
            return Style{fg, bg, bool(c.attrs & term::Bold), bool(c.attrs & term::Italic), bool(c.attrs & term::Underline),
                         bool(c.attrs & term::Strike)};
        };

        int cols = std::min<int>(int(line.size()), mScreen.cols());
        int x = 0;
        while (x < cols)
        {
            const term::Cell& c = line[x];
            if (c.width == 0)
            {
                ++x;
                continue;
            }
            Style st = styleAt(x);
            int span = c.width;
            bool batch = c.width == 1 && c.ch < 0x80;
            QString text = batch ? QString(QChar(char16_t(c.ch < 0x20 ? U' ' : c.ch))) : QString::fromUcs4(&c.ch, 1);
            // Runs of plain ASCII in one style draw as one string; anything else draws on its own.
            while (batch && x + span < cols)
            {
                const term::Cell& n = line[x + span];
                if (n.width != 1 || n.ch >= 0x80 || !(styleAt(x + span) == st))
                    break;
                text += QChar(char16_t(n.ch < 0x20 ? U' ' : n.ch));
                ++span;
            }

            QRectF rect(mPad + x * mCellW, y, span * mCellW, mCellH);
            if (st.bg != defaultBg)
                p.fillRect(rect, st.bg);
            if (!text.trimmed().isEmpty())
            {
                p.setFont(st.bold ? (st.italic ? mBoldItalic : mBold) : (st.italic ? mItalic : mFont));
                p.setPen(st.fg);
                p.drawText(QPointF(rect.left(), y + mAscent), text);
            }
            if (st.underline || st.strike)
            {
                p.setPen(st.fg);
                qreal ly = st.underline ? y + mAscent + 1.5 : y + mCellH * 0.55;
                p.drawLine(QPointF(rect.left(), ly), QPointF(rect.right(), ly));
            }
            x += span;
        }

        // Cursor past the end of a short history line, or drawn hollow when unfocused.
        if (showCursor && idx == mScreen.cursorY() && !mExited)
        {
            QRectF cell(mPad + mScreen.cursorX() * mCellW, y, mCellW, mCellH);
            if (!mFocused)
            {
                p.setPen(defaultFg);
                p.setBrush(Qt::NoBrush);
                p.drawRect(cell.adjusted(0.5, 0.5, -0.5, -0.5));
            }
            else if (mScreen.cursorX() >= cols)
                p.fillRect(cell, defaultFg);
        }
    }
}

QByteArray TerminalView::keyToBytes(QKeyEvent* e) const
{
    Qt::KeyboardModifiers mods = e->modifiers();
    bool shift = mods & Qt::ShiftModifier;
    bool ctrl = mods & Qt::ControlModifier;
    bool alt = mods & Qt::AltModifier;
    int modParam = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    bool app = mScreen.appCursorKeys();

    // CSI <letter> keys: plain, SS3 in application mode, CSI 1;<mod> with modifiers.
    auto letter = [&](char c) -> QByteArray {
        if (modParam > 1)
            return QByteArray("\x1b[1;") + QByteArray::number(modParam) + c;
        return QByteArray(app ? "\x1bO" : "\x1b[") + c;
    };
    auto tilde = [&](int n) -> QByteArray {
        QByteArray s = "\x1b[" + QByteArray::number(n);
        if (modParam > 1)
            s += ";" + QByteArray::number(modParam);
        return s + "~";
    };
    auto ss3 = [&](char c) -> QByteArray {
        if (modParam > 1)
            return QByteArray("\x1b[1;") + QByteArray::number(modParam) + c;
        return QByteArray("\x1bO") + c;
    };

    switch (e->key())
    {
    case Qt::Key_Up:
        return letter('A');
    case Qt::Key_Down:
        return letter('B');
    case Qt::Key_Right:
        return letter('C');
    case Qt::Key_Left:
        return letter('D');
    case Qt::Key_Home:
        return letter('H');
    case Qt::Key_End:
        return letter('F');
    case Qt::Key_Insert:
        return tilde(2);
    case Qt::Key_Delete:
        return tilde(3);
    case Qt::Key_PageUp:
        return tilde(5);
    case Qt::Key_PageDown:
        return tilde(6);
    case Qt::Key_F1:
        return ss3('P');
    case Qt::Key_F2:
        return ss3('Q');
    case Qt::Key_F3:
        return ss3('R');
    case Qt::Key_F4:
        return ss3('S');
    case Qt::Key_F5:
        return tilde(15);
    case Qt::Key_F6:
        return tilde(17);
    case Qt::Key_F7:
        return tilde(18);
    case Qt::Key_F8:
        return tilde(19);
    case Qt::Key_F9:
        return tilde(20);
    case Qt::Key_F10:
        return tilde(21);
    case Qt::Key_F11:
        return tilde(23);
    case Qt::Key_F12:
        return tilde(24);
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return alt ? "\x1b\r" : "\r";
    case Qt::Key_Backspace:
        return ctrl ? "\x08" : alt ? "\x1b\x7f" : "\x7f";
    case Qt::Key_Tab:
        return "\t";
    case Qt::Key_Backtab:
        return "\x1b[Z";
    case Qt::Key_Escape:
        return "\x1b";
    default:
        break;
    }

    QString text = e->text();
    // AltGr arrives as Ctrl+Alt with the composed character in text().
    if (ctrl && !alt)
    {
        int k = e->key();
        if (k >= Qt::Key_A && k <= Qt::Key_Z)
            return QByteArray(1, char(k - Qt::Key_A + 1));
        switch (k)
        {
        case Qt::Key_Space:
        case Qt::Key_At:
        case Qt::Key_2:
            return QByteArray(1, '\0');
        case Qt::Key_BracketLeft:
            return "\x1b";
        case Qt::Key_Backslash:
            return "\x1c";
        case Qt::Key_BracketRight:
            return "\x1d";
        case Qt::Key_AsciiCircum:
        case Qt::Key_6:
            return "\x1e";
        case Qt::Key_Slash:
        case Qt::Key_Underscore:
        case Qt::Key_Minus:
            return "\x1f";
        default:
            break;
        }
    }
    if (text.isEmpty())
        return {};
    QByteArray bytes = text.toUtf8();
    if (alt && !ctrl)
        bytes.prepend('\x1b');
    return bytes;
}

bool TerminalView::event(QEvent* e)
{
    // Claim every shortcut while focused, so menu accelerators don't steal keys from the shell.
    if (e->type() == QEvent::ShortcutOverride)
    {
        e->accept();
        return true;
    }
    return QAbstractScrollArea::event(e);
}

void TerminalView::keyPressEvent(QKeyEvent* e)
{
    Qt::KeyboardModifiers mods = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier);
    int key = e->key();
    if (mods == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_C)
        return copySelection();
    if ((mods == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_V) || (mods == Qt::ShiftModifier && key == Qt::Key_Insert))
        return paste();
    if (mods == Qt::ControlModifier && key == Qt::Key_C && hasSelection())
    {
        copySelection();
        mSelAnchor = mSelEnd = {};
        viewport()->update();
        return;
    }
    int page = std::max(1, mScreen.rows() - 1);
    bool ctrlShift = mods == (Qt::ControlModifier | Qt::ShiftModifier);
    if ((mods == Qt::ShiftModifier || ctrlShift) && (key == Qt::Key_PageUp || key == Qt::Key_PageDown))
        return scrollLines(key == Qt::Key_PageUp ? -page : page);
    if (ctrlShift && (key == Qt::Key_Up || key == Qt::Key_Down))
        return scrollLines(key == Qt::Key_Up ? -1 : 1);
    if (ctrlShift && key == Qt::Key_Home)
        return scrollLines(-verticalScrollBar()->maximum());
    if (ctrlShift && key == Qt::Key_End)
        return scrollToBottom();
    if (ctrlShift && key == Qt::Key_F)
        return showFind();
    if (mExited)
    {
        if (key == Qt::Key_Return || key == Qt::Key_Enter)
            startShell();
        return;
    }

    QByteArray bytes = keyToBytes(e);
    if (bytes.isEmpty())
        return QAbstractScrollArea::keyPressEvent(e);
    if (hasSelection())
    {
        mSelAnchor = mSelEnd = {};
        viewport()->update();
    }
    if (pref("/terminal/scrollOnInput", true))
        scrollToBottom();
    send(bytes);
}

void TerminalView::copySelection()
{
    if (!hasSelection())
        return;
    Pos a = std::min(mSelAnchor, mSelEnd), b = std::max(mSelAnchor, mSelEnd);
    std::string text = mScreen.text(historyIndex(a.line), a.col, historyIndex(b.line), b.col);
    QApplication::clipboard()->setText(QString::fromStdString(text));
}

void TerminalView::paste()
{
    QString text = QApplication::clipboard()->text();
    if (text.isEmpty() || mExited)
        return;
    text.replace("\r\n", "\r").replace('\n', '\r');
    QByteArray bytes = text.toUtf8();
    if (mScreen.bracketedPaste())
    {
        bytes.replace("\x1b[201~", "");
        bytes = "\x1b[200~" + bytes + "\x1b[201~";
    }
    mViewOffset = 0;
    updateScrollBar();
    send(bytes);
}

void TerminalView::clearScrollback()
{
    mScreen.feed("\x1b[3J");
    mSelAnchor = mSelEnd = {};
    updateScrollBar();
    viewport()->update();
}

bool TerminalView::forwardsMouse(Qt::KeyboardModifiers mods) const
{
    return mScreen.mouseTracking() != term::MouseTracking::Off && !(mods & Qt::ShiftModifier) && !mExited;
}

bool TerminalView::reportMouse(QMouseEvent* e, int type)
{
    if (!forwardsMouse(e->modifiers()))
    {
        mHeldButton = -1;
        return false;
    }
    auto t = term::MouseEventType(type);
    term::MouseButton button = term::MouseButton::None;
    if (t == term::MouseEventType::Move)
        button = mHeldButton >= 0 ? term::MouseButton(mHeldButton) : term::MouseButton::None;
    else if (e->button() == Qt::LeftButton)
        button = term::MouseButton::Left;
    else if (e->button() == Qt::MiddleButton)
        button = term::MouseButton::Middle;
    else if (e->button() == Qt::RightButton)
        button = term::MouseButton::Right;
    else
        return true;

    QPoint p = e->position().toPoint();
    QPoint cell(std::clamp(int((p.x() - mPad) / mCellW), 0, mScreen.cols() - 1), std::clamp((p.y() - mPad) / mCellH, 0, mScreen.rows() - 1));
    if (t == term::MouseEventType::Move && cell == mLastMouseCell)
        return true;
    mLastMouseCell = cell;
    if (t == term::MouseEventType::Press)
        mHeldButton = int(button);
    else if (t == term::MouseEventType::Release)
        mHeldButton = -1;

    Qt::KeyboardModifiers mods = e->modifiers();
    std::string bytes = term::encodeMouse(mScreen, t, button, cell.x(), cell.y(),
                                          {false, bool(mods & Qt::AltModifier), bool(mods & Qt::ControlModifier)});
    if (!bytes.empty())
        send(QByteArray::fromStdString(bytes));
    return true;
}

void TerminalView::mousePressEvent(QMouseEvent* e)
{
    setFocus(Qt::MouseFocusReason);
    if (reportMouse(e, int(term::MouseEventType::Press)))
        return;
    if (e->button() == Qt::LeftButton)
    {
        Pos at = cellAt(e->position().toPoint());
        if (e->modifiers() & Qt::ShiftModifier && hasSelection())
            mSelEnd = at;
        else
            mSelAnchor = mSelEnd = at;
        mSelecting = true;
        viewport()->update();
    }
    else if (e->button() == Qt::MiddleButton)
        paste();
}

void TerminalView::mouseMoveEvent(QMouseEvent* e)
{
    if (!mSelecting)
    {
        reportMouse(e, int(term::MouseEventType::Move));
        return;
    }
    QPoint pos = e->position().toPoint();
    // Dragging past the edges scrolls the history.
    if (pos.y() < 0)
        verticalScrollBar()->setValue(verticalScrollBar()->value() - 1);
    else if (pos.y() > viewport()->height())
        verticalScrollBar()->setValue(verticalScrollBar()->value() + 1);
    mSelEnd = cellAt(pos);
    viewport()->update();
}

void TerminalView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton && mSelecting)
    {
        mSelecting = false;
        return;
    }
    reportMouse(e, int(term::MouseEventType::Release));
}

void TerminalView::selectWord(const Pos& at)
{
    int idx = historyIndex(at.line);
    if (idx < -mScreen.scrollbackSize() || idx >= mScreen.rows())
        return;
    const term::Line& line = mScreen.historyLine(idx);
    int col = std::min(at.col, int(line.size()) - 1);
    if (col < 0 || !isWordChar(line[col].ch))
        return;
    int a = col, b = col + 1;
    while (a > 0 && isWordChar(line[a - 1].ch))
        --a;
    while (b < int(line.size()) && isWordChar(line[b].ch))
        ++b;
    mSelAnchor = {at.line, a};
    mSelEnd = {at.line, b};
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent* e)
{
    // Qt sends this in place of the second press.
    if (reportMouse(e, int(term::MouseEventType::Press)) || e->button() != Qt::LeftButton)
        return;
    QPoint pos = e->position().toPoint();
    Pos at = cellAt(pos);
    at.col = std::clamp(int((pos.x() - mPad) / mCellW), 0, mScreen.cols() - 1);
    selectWord(at);
    mSelecting = false;
    viewport()->update();
}

void TerminalView::wheelEvent(QWheelEvent* e)
{
    e->accept();
    bool shift = e->modifiers() & Qt::ShiftModifier;
    int delta = e->angleDelta().y();
    // Some platforms turn Shift+wheel into horizontal scrolling.
    if (delta == 0 && shift)
        delta = e->angleDelta().x();
    if (delta == 0)
        return;
    if ((mWheelAccum > 0) != (delta > 0))
        mWheelAccum = 0;
    mWheelAccum += delta;

    auto take = [this](int unit) {
        int n = mWheelAccum / unit;
        mWheelAccum -= n * unit;
        return n;
    };
    switch (term::wheelAction(mScreen, shift))
    {
    case term::WheelAction::ScrollHistory:
        scrollLines(-take(kWheelLine));
        break;
    case term::WheelAction::SendKeys:
    {
        int lines = take(kWheelLine);
        QByteArray key = QByteArray::fromStdString(term::wheelKeys(mScreen, lines > 0));
        for (int i = 0; i < std::abs(lines); ++i)
            send(key);
        break;
    }
    case term::WheelAction::SendMouse:
    {
        int notches = take(kWheelNotch);
        QPoint p = e->position().toPoint();
        int col = std::clamp(int((p.x() - mPad) / mCellW), 0, mScreen.cols() - 1);
        int row = std::clamp((p.y() - mPad) / mCellH, 0, mScreen.rows() - 1);
        Qt::KeyboardModifiers mods = e->modifiers();
        term::MouseButton button = notches > 0 ? term::MouseButton::WheelUp : term::MouseButton::WheelDown;
        std::string bytes = term::encodeMouse(mScreen, term::MouseEventType::Press, button, col, row,
                                              {false, bool(mods & Qt::AltModifier), bool(mods & Qt::ControlModifier)});
        for (int i = 0; i < std::abs(notches); ++i)
            send(QByteArray::fromStdString(bytes));
        break;
    }
    case term::WheelAction::Ignore:
        mWheelAccum = 0;
        break;
    }
}

void TerminalView::contextMenuEvent(QContextMenuEvent* e)
{
    // The program already got the right button.
    if (e->reason() == QContextMenuEvent::Mouse && forwardsMouse(e->modifiers()))
        return;
    QMenu menu(this);
    QAction* copy = menu.addAction("Copy\tCtrl+Shift+C", this, &TerminalView::copySelection);
    copy->setEnabled(hasSelection());
    menu.addAction("Paste\tCtrl+Shift+V", this, &TerminalView::paste);
    menu.addAction("Find...\tCtrl+Shift+F", this, &TerminalView::showFind);
    menu.addSeparator();
    menu.addAction("Clear Scrollback", this, &TerminalView::clearScrollback);
    if (mExited)
        menu.addAction("Restart Shell", this, [this] { startShell(); });
    menu.exec(e->globalPos());
}

void TerminalView::focusInEvent(QFocusEvent* e)
{
    mFocused = true;
    if (mScreen.focusReporting())
        send("\x1b[I");
    viewport()->update();
    QAbstractScrollArea::focusInEvent(e);
}

void TerminalView::focusOutEvent(QFocusEvent* e)
{
    mFocused = false;
    if (mScreen.focusReporting())
        send("\x1b[O");
    viewport()->update();
    QAbstractScrollArea::focusOutEvent(e);
}

void TerminalView::showFind()
{
    if (!mFindBar)
    {
        mFindBar = new QLineEdit(this);
        mFindBar->setPlaceholderText("Find  (Enter older, Shift+Enter newer)");
        mFindBar->setClearButtonEnabled(true);
        mFindBar->setFixedWidth(260);
        mFindBar->setProperty("hotkeysOff", true);
        mFindBar->installEventFilter(this);
        connect(mFindBar, &QLineEdit::textEdited, this, [this] {
            mSelAnchor = mSelEnd = {};
            findNext(true);
        });
        mFindBar->move(std::max(0, viewport()->width() - mFindBar->width() - 8), 6);
    }
    if (hasSelection() && !mFindBar->isVisible())
    {
        Pos a = std::min(mSelAnchor, mSelEnd), b = std::max(mSelAnchor, mSelEnd);
        if (a.line == b.line)
            mFindBar->setText(QString::fromStdString(mScreen.text(historyIndex(a.line), a.col, historyIndex(b.line), b.col)));
    }
    mFindBar->show();
    mFindBar->raise();
    mFindBar->selectAll();
    mFindBar->setFocus(Qt::ShortcutFocusReason);
}

void TerminalView::findNext(bool backward)
{
    std::string needle = mFindBar ? mFindBar->text().toStdString() : std::string();
    if (needle.empty())
    {
        viewport()->update();
        return;
    }
    int top = mScreen.altScreen() ? 0 : -mScreen.scrollbackSize();
    // Start beside the current match, else from the end the search walks away from.
    int line = backward ? mScreen.rows() : top - 1, col = 0;
    if (hasSelection())
    {
        Pos a = std::min(mSelAnchor, mSelEnd);
        line = historyIndex(a.line);
        col = a.col;
    }
    auto match = mScreen.find(needle, line, col, backward);
    if (!match)
        match = mScreen.find(needle, backward ? mScreen.rows() : top - 1, 0, backward);
    mFindBar->setStyleSheet(match ? QString() : QString("QLineEdit { color: #8a8a8a; }"));
    if (!match)
        return;

    mSelAnchor = {absLine(match->line), match->col};
    mSelEnd = {absLine(match->line), match->col + match->length};
    int first = firstVisibleLine();
    if (!mScreen.altScreen() && (match->line < first || match->line >= first + mScreen.rows()))
    {
        mViewOffset = std::clamp(mScreen.rows() / 2 - match->line, 0, mScreen.scrollbackSize());
        updateScrollBar();
    }
    viewport()->update();
}

bool TerminalView::eventFilter(QObject* watched, QEvent* e)
{
    if (watched == mFindBar && e->type() == QEvent::KeyPress)
    {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Escape)
        {
            mFindBar->hide();
            setFocus(Qt::OtherFocusReason);
            return true;
        }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter)
        {
            findNext(!(k->modifiers() & Qt::ShiftModifier));
            return true;
        }
    }
    // Keeps app hotkeys out of the find field too.
    if (watched == mFindBar && e->type() == QEvent::ShortcutOverride)
    {
        e->accept();
        return false;
    }
    return QAbstractScrollArea::eventFilter(watched, e);
}

// TerminalPanel

TerminalPanel::TerminalPanel(QWidget* parent) : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    mTabs = new QTabWidget(this);
    mTabs->setDocumentMode(true);
    mTabs->setMovable(true);
    layout->addWidget(mTabs);

    auto* add = new QToolButton(mTabs);
    add->setText("+");
    add->setToolTip("New terminal");
    add->setAutoRaise(true);
    mTabs->setCornerWidget(add, Qt::TopRightCorner);
    connect(add, &QToolButton::clicked, this, [this] { newTerminal(); });
}

TerminalView* TerminalPanel::newTerminal()
{
    auto* view = new TerminalView(mTabs);
    int index = mTabs->addTab(view, "shell");
    // Our own close button: the style's default is tinted, and the app stays grayscale.
    auto* close = new QToolButton(mTabs->tabBar());
    close->setText(QString(QChar(0x00D7)));
    close->setAutoRaise(true);
    close->setFocusPolicy(Qt::NoFocus);
    close->setFixedSize(16, 16);
    close->setToolTip("Close");
    close->setStyleSheet("QToolButton { border: none; color: #8a8a8a; padding: 0; }"
                         "QToolButton:hover { color: #f0f0f0; background: #5a5a5a; border-radius: 2px; }");
    connect(close, &QToolButton::clicked, this, [this, view] { closeTab(mTabs->indexOf(view)); });
    mTabs->tabBar()->setTabButton(index, QTabBar::RightSide, close);
    mTabs->setCurrentIndex(index);
    view->startShell();
    mTabs->setTabText(index, view->shellName());
    connect(view, &TerminalView::exited, this, [this, view] {
        int i = mTabs->indexOf(view);
        if (i >= 0)
            mTabs->setTabText(i, view->shellName() + " (exited)");
    });
    view->setFocus();
    return view;
}

TerminalView* TerminalPanel::currentTerminal() const
{
    return qobject_cast<TerminalView*>(mTabs->currentWidget());
}

void TerminalPanel::closeTab(int index)
{
    QWidget* w = mTabs->widget(index);
    mTabs->removeTab(index);
    delete w;
}

void TerminalPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (mTabs->count() == 0)
        newTerminal();
}
} // namespace lila::ui
