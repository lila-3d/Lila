#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>

namespace lila::mcp
{
using json = nlohmann::json;

struct Tool
{
    std::string name;
    std::string description;
    json inputSchema;
    /** Runs on the main thread. Return text for the model; throw to report an error. */
    std::function<std::string(const json& args)> run;
    /** Instead of run: returns an MCP content array (text, image blocks). Runs on the main thread. */
    std::function<json(const json& args)> runContent;
};

/** A read-only document clients can list and fetch, such as an agent guide. */
struct Resource
{
    std::string uri; // "lila://guide/modeling"
    std::string name;
    std::string description;
    std::string mimeType = "text/markdown";
    /** Runs on the main thread. */
    std::function<std::string()> read;
};

/** A prompt template; clients show these as slash commands. */
struct Prompt
{
    std::string name;
    std::string description;
    json arguments = json::array(); // [{"name", "description", "required"}]
    /** Returns the prompt text for the given arguments. Runs on the main thread. */
    std::function<std::string(const json& args)> get;
};

/**
 * MCP over streamable HTTP (JSON responses, no SSE) on localhost. Requests arrive on the server thread and tool
 * calls hop to the main thread. Writes <userConfigDir>/mcp.json with url + token so clients can be pointed at it.
 */
class Server
{
public:
    static Server& get();

    void addTool(Tool tool);
    void addResource(Resource resource);
    void addPrompt(Prompt prompt);
    /** Sent to clients on initialize; agents read it as standing instructions for the server. */
    void setInstructions(std::string text);

    bool start(int port, const std::string& token);
    void stop();
    bool running() const;
    int port() const;
    std::string url() const;
    std::string token() const;

    /** One JSON-RPC request as the HTTP endpoint would answer it (null for notifications). Not on the main thread. */
    json handle(const json& request);

    ~Server();

private:
    Server();
    struct Impl;
    std::unique_ptr<Impl> m;
};

/** Registers the built-in tools and starts the server if prefs say so (general.json /mcp/enabled). */
void startFromPrefs();

/** Registers the built-in tools, resources and prompts without starting anything (batch mode, tests). Idempotent. */
void registerBuiltins();

/** The token from prefs, created on first use. */
std::string tokenFromPrefs();
int portFromPrefs();
} // namespace lila::mcp
