#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

/**
 * Batch script execution: one Python script run over many scene files, each in its own lilabatch worker. This half is
 * the plan (which files, where each one saves, whether it can be written) and its reporting, with no processes.
 */
namespace lila::batch
{
using json = nlohmann::json;
namespace fs = std::filesystem;

enum class SaveMode : uint8_t
{
    None,      // scripts that only read or export
    Overwrite, // in place; a scene Lila cannot write (.ma, .fbx) saves as .lila beside it
    SaveAs,    // to outputDir (or beside the source) with suffix and format
};

struct Spec
{
    /** The script's source. scriptFile names where it came from, for tracebacks; with no source it is read from there. */
    std::string script;
    std::string scriptFile;
    std::string scriptLabel; // "fix.py", "scene script: tidy", "snippet"

    /** Files, folders and globs ("D:/art/*.ma", "props/**"). */
    std::vector<std::string> inputs;
    /** How deep folders are walked: 0 the folder itself, -1 everything below. */
    int depth = 0;
    /** Lowercase with the dot. Empty: every format Lila opens. Explicitly named files are never filtered by it. */
    std::vector<std::string> extensions;
    /** Globs on the file name, or on the path below its input folder when they contain a slash. Any include must match. */
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    /** Skips files whose name without extension starts with, ends with or contains these. */
    std::string excludePrefix;
    std::string excludeSuffix;
    std::string excludeContains;

    SaveMode save = SaveMode::None;
    std::string outputDir;
    std::string suffix;
    std::string format;   // "lila", "lilaa" or empty to keep the source's
    bool keepTree = true; // under outputDir, mirror the folders below each input folder
    /** "auto" checks read-only files out through source control (per its autoCheckout pref), "never" leaves them. */
    std::string checkout = "auto";
    /** Read-only files that source control does not take are made writable instead of failing. */
    bool makeWritable = false;

    int workers = 0; // 0 = half the cores, at most 8
    bool stopOnError = false;
    double timeout = 0.0; // seconds per file, 0 = none
    bool dryRun = false;
    json args = json::object(); // handed to the script as ctx.args
    std::string csv;            // written when the run finishes
    std::string workspace;      // opened by each worker so prefs and source control match the caller's
};

LILA_SCENE_API const char* saveModeName(SaveMode m);
/** Throws std::invalid_argument for an unknown name. */
LILA_SCENE_API SaveMode saveModeFromName(std::string_view name);

LILA_SCENE_API json toJson(const Spec& s);
/** Missing keys keep their defaults. Throws std::invalid_argument on bad values. */
LILA_SCENE_API Spec specFromJson(const json& j);

/** What a planned file's target is like before the run. */
enum class Access : uint8_t
{
    None,      // not saving
    New,       // output does not exist yet
    Replace,   // output exists and is writable
    ReadOnly,  // output exists read-only: checked out or made writable at save time, else the file fails
    Missing,   // the source itself is gone
    Collision, // another file in the run saves to the same output
};
LILA_SCENE_API const char* accessName(Access a);

struct Item
{
    fs::path path;
    fs::path root; // the input folder it was found under, for keepTree
    fs::path output;
    Access access = Access::None;
};

/**
 * `*` and `?` stay within a path segment, `**` crosses them, `[abc]` and `[!a-z]` are sets. ASCII case-blind, and `\`
 * counts as `/` in both.
 */
LILA_SCENE_API bool globMatch(std::string_view pattern, std::string_view text);

/** Lowercase, with a leading dot: "MA" and ".ma" both give ".ma". */
LILA_SCENE_API std::string normalizeExtension(std::string_view ext);

/** The extensions every registered open translator reads, or .lila/.lilaa when none are registered. */
LILA_SCENE_API std::vector<std::string> openableExtensions();

/** The files the spec names, filtered, de-duplicated, folders sorted by path. Problems go to warnings. */
LILA_SCENE_API std::vector<Item> gather(const Spec& s, std::vector<std::string>* warnings = nullptr);

/** Where item saves under spec, empty when it does not. */
LILA_SCENE_API fs::path outputFor(const Spec& s, const Item& item);

/** True when p exists and its owner write permission is clear. */
LILA_SCENE_API bool isReadOnly(const fs::path& p);

/** Fills every item's output and access, marking later items that would overwrite an earlier one's output. */
LILA_SCENE_API void plan(const Spec& s, std::vector<Item>& items);

enum class Status : uint8_t
{
    Pending,
    Running,
    Done,
    Failed,
    Skipped,
    Cancelled,
    Planned, // a dry run's files
};
LILA_SCENE_API const char* statusName(Status s);
inline bool finished(Status s) { return s != Status::Pending && s != Status::Running; }

/** One file's outcome, as a worker writes it and the summary reports it. */
struct FileResult
{
    Status status = Status::Pending;
    double seconds = 0.0;
    bool saved = false;
    std::string message; // the error or skip reason
    std::string errorFile;
    int errorLine = 0;
    std::string traceback;
    json results = json::object(); // what the script put in ctx.results
    std::string log;               // the worker's captured output
};

LILA_SCENE_API json toJson(const FileResult& r);
LILA_SCENE_API FileResult resultFromJson(const json& j);

/** A row per file: index, file, status, seconds, output, saved, message, then one column per result key, first seen first. */
LILA_SCENE_API std::string toCsv(const std::vector<Item>& items, const std::vector<FileResult>& results);
} // namespace lila::batch
