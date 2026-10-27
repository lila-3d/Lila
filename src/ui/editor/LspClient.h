#pragma once

#include "core/Export.h"

#include "ui/editor/EditorTypes.h"
#include "ui/editor/PythonHighlighter.h"

#include <nlohmann/json.hpp>

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QProcess>

#include <functional>
#include <optional>

namespace lila::ui
{
/** Splits a byte stream into JSON-RPC messages framed by `Content-Length` headers. */
class LILA_UI_API LspFramer
{
public:
    void feed(const QByteArray& bytes);
    /** Next complete message body, if one has arrived. */
    std::optional<QByteArray> next();

    static QByteArray frame(const nlohmann::json& message);

private:
    QByteArray mBuffer;
};

/** Converts an LSP `Diagnostic` object. */
LILA_UI_API Diagnostic diagnosticFromLsp(const nlohmann::json& d);

/**
 * Decodes a `textDocument/semanticTokens/full` result against the server's legend into highlighter tokens by line.
 * Token types the highlighter has no kind for are left out, so the lexical colour stays.
 */
LILA_UI_API QHash<int, std::vector<Token>> semanticTokensFromLsp(const nlohmann::json& result, const nlohmann::json& legend);

/**
 * A language server over stdio. Requests made before the server is initialized are dropped (the callback never
 * runs), so callers can ask freely and simply get nothing until it is ready.
 */
class LILA_UI_API LspClient : public QObject
{
    Q_OBJECT

public:
    using json = nlohmann::json;
    using Handler = std::function<void(const json& result, const json& error)>;

    explicit LspClient(QObject* parent = nullptr);
    ~LspClient() override;

    /** `settings` answers workspace/configuration by section, e.g. {"basedpyright": {"analysis": {...}}}. */
    void start(const QString& program, const QStringList& args, const json& settings);
    void stop();
    bool isReady() const { return mReady; }
    const json& capabilities() const { return mCapabilities; }

    /** Returns the request id, or -1 if the server is not ready. */
    int request(const QString& method, json params, Handler handler);
    void cancel(int id);
    void notify(const QString& method, json params);

    void openDocument(const QString& uri, const QString& text);
    void changeDocument(const QString& uri, const QString& text);
    void closeDocument(const QString& uri);
    bool isOpen(const QString& uri) const { return mVersions.contains(uri); }

Q_SIGNALS:
    void ready();
    void stopped(const QString& reason);
    void diagnosticsPublished(const QString& uri, const QList<lila::ui::Diagnostic>& diagnostics);
    void logMessage(const QString& text);

private:
    void onReadyRead();
    void handle(const json& msg);
    void handleServerRequest(const json& msg);
    void send(const json& msg);
    json configurationFor(const std::string& section) const;

    QProcess* mProcess = nullptr;
    LspFramer mFramer;
    json mSettings;
    json mCapabilities;
    QHash<int, Handler> mPending;
    QHash<QString, int> mVersions;
    int mNextId = 1;
    bool mReady = false;
    bool mStopping = false;
};
} // namespace lila::ui
