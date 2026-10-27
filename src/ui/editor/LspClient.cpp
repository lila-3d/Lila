#include "ui/editor/LspClient.h"

#include "core/Profile.h"

#include <QCoreApplication>

namespace lila::ui
{
using json = nlohmann::json;

void LspFramer::feed(const QByteArray& bytes)
{
    mBuffer.append(bytes);
}

std::optional<QByteArray> LspFramer::next()
{
    qsizetype headerEnd = mBuffer.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return std::nullopt;
    qsizetype length = -1;
    for (const QByteArray& line : mBuffer.first(headerEnd).split('\n'))
    {
        qsizetype colon = line.indexOf(':');
        if (colon > 0 && line.first(colon).trimmed().compare("Content-Length", Qt::CaseInsensitive) == 0)
            length = line.sliced(colon + 1).trimmed().toLongLong();
    }
    qsizetype bodyStart = headerEnd + 4;
    if (length < 0)
    {
        // Malformed header: drop it rather than wedge the stream.
        mBuffer.remove(0, bodyStart);
        return std::nullopt;
    }
    if (mBuffer.size() - bodyStart < length)
        return std::nullopt;
    QByteArray body = mBuffer.mid(bodyStart, length);
    mBuffer.remove(0, bodyStart + length);
    return body;
}

QByteArray LspFramer::frame(const json& message)
{
    std::string body = message.dump(-1, ' ', false, json::error_handler_t::replace);
    QByteArray out = "Content-Length: " + QByteArray::number(qsizetype(body.size())) + "\r\n\r\n";
    out.append(body.data(), qsizetype(body.size()));
    return out;
}

QHash<int, std::vector<Token>> semanticTokensFromLsp(const json& result, const json& legend)
{
    LILA_ZONE();

    QHash<int, std::vector<Token>> out;
    const json data = result.is_object() ? result.value("data", json::array()) : json::array();
    if (!data.is_array())
        return out;
    std::vector<int> kinds;
    for (const json& t : legend.value("tokenTypes", json::array()))
    {
        std::string type = t.is_string() ? t.get<std::string>() : std::string();
        TokenKind k = TokenKind::Count;
        if (type == "namespace")
            k = TokenKind::Module;
        else if (type == "class" || type == "type" || type == "enum" || type == "typeParameter")
            k = TokenKind::ClassName;
        else if (type == "function" || type == "method")
            k = TokenKind::Function;
        else if (type == "parameter")
            k = TokenKind::Parameter;
        else if (type == "variable" || type == "property")
            k = TokenKind::Variable;
        else if (type == "enumMember")
            k = TokenKind::ConstantName;
        else if (type == "decorator")
            k = TokenKind::Decorator;
        else if (type == "selfParameter" || type == "clsParameter")
            k = TokenKind::Self;
        kinds.push_back(int(k));
    }
    int readonlyBit = -1;
    const json modifiers = legend.value("tokenModifiers", json::array());
    for (size_t i = 0; i < modifiers.size(); ++i)
        if (modifiers[i] == "readonly")
            readonlyBit = int(i);

    int line = 0;
    int col = 0;
    for (size_t i = 0; i + 4 < data.size(); i += 5)
    {
        int dLine = data[i].get<int>();
        int dCol = data[i + 1].get<int>();
        line += dLine;
        col = dLine == 0 ? col + dCol : dCol;
        int length = data[i + 2].get<int>();
        int type = data[i + 3].get<int>();
        int mods = data[i + 4].get<int>();
        if (type < 0 || type >= int(kinds.size()) || kinds[size_t(type)] == int(TokenKind::Count))
            continue;
        auto kind = TokenKind(kinds[size_t(type)]);
        if (kind == TokenKind::Variable && readonlyBit >= 0 && (mods >> readonlyBit) & 1)
            kind = TokenKind::ConstantName;
        out[line].push_back({col, length, kind});
    }
    return out;
}

Diagnostic diagnosticFromLsp(const json& d)
{
    Diagnostic out;
    const json& r = d.value("range", json::object());
    out.line = r.value("/start/line"_json_pointer, 0);
    out.col = r.value("/start/character"_json_pointer, 0);
    out.endLine = r.value("/end/line"_json_pointer, out.line);
    out.endCol = r.value("/end/character"_json_pointer, out.col);
    out.severity = Severity(std::clamp(d.value("severity", 2), 1, 4));
    out.message = QString::fromStdString(d.value("message", std::string()));
    out.source = QString::fromStdString(d.value("source", std::string()));
    if (auto c = d.find("code"); c != d.end())
        out.code = c->is_string() ? QString::fromStdString(c->get<std::string>()) : QString::fromStdString(c->dump());
    if (auto tags = d.find("tags"); tags != d.end() && tags->is_array())
        for (const json& t : *tags)
            if (t == 1)
                out.unnecessary = true;
    return out;
}

LspClient::LspClient(QObject* parent) : QObject(parent) {}

LspClient::~LspClient()
{
    stop();
}

void LspClient::start(const QString& program, const QStringList& args, const json& settings)
{
    LILA_ZONE();

    stop();
    mSettings = settings;
    mStopping = false;
    mProcess = new QProcess(this);
    mProcess->setProcessChannelMode(QProcess::SeparateChannels);
    connect(mProcess, &QProcess::readyReadStandardOutput, this, &LspClient::onReadyRead);
    connect(mProcess, &QProcess::readyReadStandardError, this,
            [this] { Q_EMIT logMessage(QString::fromUtf8(mProcess->readAllStandardError()).trimmed()); });
    connect(mProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!mStopping)
            Q_EMIT stopped(mProcess->errorString());
    });
    connect(mProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        mReady = false;
        if (!mStopping)
            Q_EMIT stopped(QString("language server exited (%1)").arg(code));
    });
    mProcess->start(program, args);

    json caps = {
        {"general", {{"positionEncodings", {"utf-16"}}}},
        {"workspace", {{"configuration", true}, {"didChangeConfiguration", {{"dynamicRegistration", true}}}, {"workspaceFolders", false}}},
        {"textDocument",
         {
             {"synchronization", {{"didSave", true}, {"dynamicRegistration", false}}},
             {"completion",
              {{"completionItem",
                {{"documentationFormat", {"plaintext"}},
                 {"snippetSupport", false},
                 {"resolveSupport", {{"properties", {"documentation", "detail"}}}}}},
               {"contextSupport", true}}},
             {"hover", {{"contentFormat", {"plaintext"}}}},
             {"signatureHelp",
              {{"signatureInformation",
                {{"documentationFormat", {"plaintext"}}, {"parameterInformation", {{"labelOffsetSupport", true}}}, {"activeParameterSupport", true}}}}},
             {"definition", {{"linkSupport", true}}},
             {"documentHighlight", json::object()},
             {"semanticTokens",
              {{"requests", {{"full", true}, {"range", false}}},
               {"tokenTypes", {"namespace", "type", "class", "enum", "typeParameter", "parameter", "variable", "property", "enumMember",
                               "function", "method", "decorator"}},
               {"tokenModifiers", {"declaration", "definition", "readonly", "static", "async", "defaultLibrary"}},
               {"formats", {"relative"}},
               {"overlappingTokenSupport", false},
               {"multilineTokenSupport", false}}},
             {"rename", {{"prepareSupport", false}}},
             {"publishDiagnostics", {{"relatedInformation", false}, {"tagSupport", {{"valueSet", {1, 2}}}}}},
         }},
    };
    json params = {
        {"processId", QCoreApplication::applicationPid()},
        {"clientInfo", {{"name", "Lila"}}},
        {"rootUri", nullptr},
        {"capabilities", caps},
        {"initializationOptions", json::object()},
    };

    int id = mNextId++;
    mPending.insert(id, [this](const json& result, const json& error) {
        if (!error.is_null())
        {
            Q_EMIT stopped("initialize failed: " + QString::fromStdString(error.dump()));
            return;
        }
        mCapabilities = result.value("capabilities", json::object());
        mReady = true;
        notify("initialized", json::object());
        notify("workspace/didChangeConfiguration", {{"settings", mSettings}});
        Q_EMIT ready();
    });
    send({{"jsonrpc", "2.0"}, {"id", id}, {"method", "initialize"}, {"params", params}});
}

void LspClient::stop()
{
    if (!mProcess)
        return;
    mStopping = true;
    if (mProcess->state() == QProcess::Running)
    {
        if (mReady)
        {
            send({{"jsonrpc", "2.0"}, {"id", mNextId++}, {"method", "shutdown"}, {"params", nullptr}});
            send({{"jsonrpc", "2.0"}, {"method", "exit"}, {"params", nullptr}});
        }
        mProcess->closeWriteChannel();
        if (!mProcess->waitForFinished(800))
        {
            mProcess->kill();
            mProcess->waitForFinished(500);
        }
    }
    mProcess->disconnect(this);
    mProcess->deleteLater();
    mProcess = nullptr;
    mReady = false;
    mPending.clear();
    mVersions.clear();
    mFramer = LspFramer();
}

int LspClient::request(const QString& method, json params, Handler handler)
{
    if (!mReady)
        return -1;
    int id = mNextId++;
    mPending.insert(id, std::move(handler));
    send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method.toStdString()}, {"params", std::move(params)}});
    return id;
}

void LspClient::cancel(int id)
{
    if (mPending.remove(id))
        notify("$/cancelRequest", {{"id", id}});
}

void LspClient::notify(const QString& method, json params)
{
    if (!mProcess)
        return;
    send({{"jsonrpc", "2.0"}, {"method", method.toStdString()}, {"params", std::move(params)}});
}

void LspClient::openDocument(const QString& uri, const QString& text)
{
    if (!mReady)
        return;
    if (mVersions.contains(uri))
        closeDocument(uri);
    mVersions.insert(uri, 1);
    notify("textDocument/didOpen",
           {{"textDocument", {{"uri", uri.toStdString()}, {"languageId", "python"}, {"version", 1}, {"text", text.toStdString()}}}});
}

void LspClient::changeDocument(const QString& uri, const QString& text)
{
    if (!mReady || !mVersions.contains(uri))
        return;
    int v = ++mVersions[uri];
    notify("textDocument/didChange", {{"textDocument", {{"uri", uri.toStdString()}, {"version", v}}},
                                      {"contentChanges", json::array({{{"text", text.toStdString()}}})}});
}

void LspClient::closeDocument(const QString& uri)
{
    if (!mVersions.remove(uri))
        return;
    notify("textDocument/didClose", {{"textDocument", {{"uri", uri.toStdString()}}}});
}

void LspClient::send(const json& msg)
{
    // While the process is still starting, QProcess buffers the write.
    if (mProcess)
        mProcess->write(LspFramer::frame(msg));
}

void LspClient::onReadyRead()
{
    LILA_ZONE();

    mFramer.feed(mProcess->readAllStandardOutput());
    while (auto body = mFramer.next())
    {
        json msg = json::parse(body->constBegin(), body->constEnd(), nullptr, false);
        if (!msg.is_discarded())
            handle(msg);
    }
}

void LspClient::handle(const json& msg)
{
    bool hasId = msg.contains("id") && !msg["id"].is_null();
    if (msg.contains("method"))
    {
        if (hasId)
        {
            handleServerRequest(msg);
            return;
        }
        std::string method = msg["method"].get<std::string>();
        const json& params = msg.value("params", json::object());
        if (method == "textDocument/publishDiagnostics")
        {
            QList<Diagnostic> list;
            for (const json& d : params.value("diagnostics", json::array()))
                list.append(diagnosticFromLsp(d));
            Q_EMIT diagnosticsPublished(QString::fromStdString(params.value("uri", std::string())), list);
        }
        else if (method == "window/logMessage" || method == "window/showMessage")
            Q_EMIT logMessage(QString::fromStdString(params.value("message", std::string())));
        return;
    }
    if (!hasId || !msg["id"].is_number_integer())
        return;
    int id = msg["id"].get<int>();
    auto it = mPending.find(id);
    if (it == mPending.end())
        return;
    Handler h = std::move(it.value());
    mPending.erase(it);
    if (h)
        h(msg.value("result", json()), msg.value("error", json()));
}

void LspClient::handleServerRequest(const json& msg)
{
    std::string method = msg["method"].get<std::string>();
    json result = nullptr;
    if (method == "workspace/configuration")
    {
        result = json::array();
        for (const json& item : msg.value("/params/items"_json_pointer, json::array()))
            result.push_back(configurationFor(item.value("section", std::string())));
    }
    send({{"jsonrpc", "2.0"}, {"id", msg["id"]}, {"result", result}});
}

json LspClient::configurationFor(const std::string& section) const
{
    if (section.empty())
        return mSettings;
    std::string pointer = "/" + section;
    for (char& c : pointer)
        if (c == '.')
            c = '/';
    json::json_pointer p(pointer);
    return mSettings.contains(p) ? mSettings[p] : json(nullptr);
}
} // namespace lila::ui
