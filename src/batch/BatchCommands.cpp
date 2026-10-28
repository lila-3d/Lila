#include "batch/BatchCommands.h"

#include "batch/BatchRunner.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "ops/Command.h"
#include "scene/SceneScripts.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace lila::batch
{
namespace
{
constexpr const char* kPrefs = "batchRun";

fs::path resolvePath(const std::string& s)
{
    fs::path p = paths::fromUtf8(s);
    if (p.is_relative())
        p = (Workspace::get().isOpen() ? Workspace::get().root() : fs::current_path()) / p;
    return p.lexically_normal();
}

json presets() { return PrefStore::get().value(kPrefs, "/presets", json::object()); }

json namedEntry(const char* cmd, const char* key, const Args& a, const char* valueFlag)
{
    json all = PrefStore::get().value(kPrefs, key, json::object());
    if (!all.is_object())
        all = json::object();
    if (a.positional.empty())
        return all;
    const std::string name = a.positional[0].get<std::string>();
    if (name.empty())
        throw CommandError(std::string(cmd) + ": the name is empty");
    if (a.get<bool>("delete"))
    {
        if (!all.erase(name))
            throw CommandError(std::string(cmd) + ": no '" + name + "'");
        PrefStore::get().set(kPrefs, key, all);
        return all;
    }
    if (a.has(valueFlag))
    {
        all[name] = a.flags[valueFlag];
        PrefStore::get().set(kPrefs, key, all);
        return all[name];
    }
    if (!all.contains(name))
        throw CommandError(std::string(cmd) + ": no '" + name + "'");
    return all[name];
}

/** The spec a batchRun call describes: its preset, then every flag it was given. */
Spec specFromCall(const Args& a)
{
    json j = json::object();
    if (a.has("preset"))
    {
        const std::string name = a.get<std::string>("preset");
        json all = presets();
        if (!all.is_object() || !all.contains(name))
            throw CommandError("batchRun: no preset '" + name + "'");
        j = all[name];
    }
    for (const char* k : {"script", "extensions", "include", "exclude", "excludePrefix", "excludeSuffix", "excludeContains", "save",
                          "outputDir", "suffix", "format", "keepTree", "checkout", "makeWritable", "workers", "stopOnError", "timeout",
                          "dryRun", "csv", "depth"})
        if (a.has(k))
            j[k] = a.flags[k];
    if (a.has("scriptArgs"))
        j["args"] = a.flags["scriptArgs"];
    if (a.has("recursive") && !a.has("depth"))
        j["depth"] = a.get<bool>("recursive") ? -1 : 0;
    if (a.has("scriptFile"))
    {
        j["scriptFile"] = paths::toUtf8(resolvePath(a.get<std::string>("scriptFile")));
        if (!a.has("script"))
            j.erase("script");
    }
    if (a.has("sceneScript"))
    {
        const std::string name = a.get<std::string>("sceneScript");
        const scene::scripts::Script* s = scene::scripts::find(name);
        if (!s)
            throw CommandError("batchRun: the scene has no script '" + name + "'");
        j["script"] = s->text;
        j["scriptFile"] = "";
        j["scriptLabel"] = "scene script: " + name;
    }
    json inputs = json::array();
    for (const json& v : a.positional)
    {
        if (v.is_string())
            inputs.push_back(v);
        else if (v.is_array())
            for (const json& x : v)
                if (x.is_string())
                    inputs.push_back(x);
    }
    if (a.has("fileList"))
    {
        json lists = PrefStore::get().value(kPrefs, "/fileLists", json::object());
        const std::string name = a.get<std::string>("fileList");
        if (!lists.is_object() || !lists.contains(name) || !lists[name].is_array())
            throw CommandError("batchRun: no file list '" + name + "'");
        for (const json& x : lists[name])
            inputs.push_back(x);
    }
    if (!inputs.empty())
        j["inputs"] = inputs;
    if (a.has("csv"))
        j["csv"] = paths::toUtf8(resolvePath(a.get<std::string>("csv")));
    if (!j.contains("workspace") || !j["workspace"].is_string() || j["workspace"].get<std::string>().empty())
        j["workspace"] = Workspace::get().isOpen() ? paths::toUtf8(Workspace::get().root()) : std::string();
    try
    {
        return specFromJson(j);
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string("batchRun: ") + e.what());
    }
}

std::shared_ptr<Run> runArg(const char* cmd, const Args& a)
{
    int id = a.positional.empty() ? 0 : a.positional[0].get<int>();
    std::shared_ptr<Run> r = findRun(id);
    if (!r)
        throw CommandError(std::string(cmd) + (id ? ": no run " + std::to_string(id) : std::string(": no batch run yet")));
    return r;
}

json afterStart(const std::shared_ptr<Run>& run, const Args& a, const std::vector<std::string>& warnings)
{
    const bool wait = a.has("wait") ? a.get<bool>("wait") : !interactive();
    if (wait)
        run->wait();
    json out = run->summary(run->isFinished());
    if (!warnings.empty())
        out["warnings"] = warnings;
    return out;
}

std::vector<FlagDef> runFlags()
{
    using T = ArgType;
    return {
        {"script", "s", T::String, nullptr, "Python source to run in every file. ctx (lila.batch.BatchContext) is in its globals."},
        {"scriptFile", "sf", T::String, nullptr, "A .py file to run instead; tracebacks point into it."},
        {"sceneScript", "ss", T::String, nullptr, "Run a script saved in the open scene, by name."},
        {"recursive", "r", T::Bool, nullptr, "Walk folders all the way down. Default: only the folder itself."},
        {"depth", "d", T::Int, nullptr, "How many folder levels to walk below each input folder; -1 for all.", {}, {}, -1.0},
        {"extensions", "ext", T::List, nullptr, "File types folders are searched for (\"ma\", \".lila\"). Default: every type Lila opens."},
        {"include", "inc", T::List, nullptr, "Globs a file must match (name, or path below its folder when it has a slash)."},
        {"exclude", "exc", T::List, nullptr, "Globs that leave a file out."},
        {"excludePrefix", "xp", T::String, nullptr, "Leave out files whose name starts with this."},
        {"excludeSuffix", "xs", T::String, nullptr, "Leave out files whose name (without extension) ends with this."},
        {"excludeContains", "xc", T::String, nullptr, "Leave out files whose name contains this."},
        {"fileList", "fl", T::String, nullptr, "Add the files of a saved file list (batchRunFileList)."},
        {"save", "sv", T::String, nullptr, "none (default), overwrite (in place; other formats save as .lila beside them) or saveAs.",
         {"none", "overwrite", "saveAs"}},
        {"outputDir", "o", T::String, nullptr, "saveAs: the folder to write to. Default: beside each source."},
        {"suffix", "sx", T::String, nullptr, "saveAs: added to each file name (\"_fixed\")."},
        {"format", "fmt", T::String, nullptr, "saveAs: lila or lilaa. Default: the source's, .lila for other formats.", {"", "lila", "lilaa"}},
        {"keepTree", "kt", T::Bool, nullptr, "saveAs: mirror the folders below each input folder under outputDir. Default on."},
        {"checkout", "co", T::String, nullptr, "auto (default) checks read-only files out through source control; never leaves them.",
         {"auto", "never"}},
        {"makeWritable", "mw", T::Bool, nullptr, "Clear the read-only flag of files source control does not take."},
        {"workers", "w", T::Int, nullptr, "Files processed at once, each in its own lilabatch. Default 0: half the cores, at most 8.", {}, {}, 0.0},
        {"stopOnError", "soe", T::Bool, nullptr, "Start no more files after one fails. Default: carry on."},
        {"timeout", "to", T::Float, nullptr, "Seconds a file may take before its worker is ended; 0 for no limit.", {}, {}, 0.0},
        {"dryRun", "dry", T::Bool, nullptr, "Only report the plan: each file, where it saves and whether it can be written."},
        {"scriptArgs", "sa", T::Any, nullptr, "A dict handed to the script as ctx.args."},
        {"csv", "", T::String, nullptr, "Write the summary table here when the run finishes."},
        {"preset", "p", T::String, nullptr, "Start from a saved preset (batchRunPreset); flags given here override it."},
        {"wait", "", T::Bool, nullptr, "Block until the run finishes. Default: wait in batch mode, return at once with a UI."},
    };
}
} // namespace

void registerCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "batchRun",
        .doc = "Run a Python script over many scene files, each opened in its own lilabatch worker, several at once. The "
               "script sees ctx: ctx.file, ctx.output, ctx.index, ctx.args (scriptArgs), ctx.results (a dict collected into the summary "
               "and CSV), ctx.save = False to leave a file unsaved, ctx.skip(reason). Returns the run's summary (with "
               "files once finished): {id, state, total, counts, seconds, dir, csv, files}.",
        .positionalDoc = "files, folders or globs",
        .flags = runFlags(),
        .undoable = false,
        .run = [](const Args& a) -> json {
            Spec spec = specFromCall(a);
            std::vector<std::string> warnings;
            try
            {
                return afterStart(startRun(spec, &warnings), a, warnings);
            }
            catch (const std::invalid_argument& e)
            {
                std::string msg = std::string("batchRun: ") + e.what();
                for (const std::string& w : warnings)
                    msg += "; " + w;
                throw CommandError(msg);
            }
        },
    });

    ops::registerCommand({
        .name = "batchRunStatus",
        .doc = "A batch run's summary: {id, state, total, counts, seconds, dir, csv, files: [{index, file, output, access, "
               "status, seconds, saved, message, errorFile, errorLine, traceback, results, log}]}. With list, every kept run "
               "without its files.",
        .positionalDoc = "[run id, default the newest]",
        .flags = {{"files", "f", T::Bool, true, "Include the per-file rows."}, {"list", "l", T::Bool, false, "Summaries of every kept run."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("list"))
            {
                json out = json::array();
                for (const auto& r : runs())
                    out.push_back(r->summary(false));
                return out;
            }
            return runArg("batchRunStatus", a)->summary(a.get<bool>("files"));
        },
    });

    ops::registerCommand({
        .name = "batchRunCancel",
        .doc = "Cancel a batch run: its workers are ended and the files not started yet are left cancelled.",
        .positionalDoc = "[run id, default the newest]",
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto r = runArg("batchRunCancel", a);
            r->cancel();
            return r->id();
        },
    });

    ops::registerCommand({
        .name = "batchRunRetry",
        .doc = "Run the files of an earlier batch run again as a new run: the failed and cancelled ones, or those given by "
               "index. Returns the new run's summary.",
        .positionalDoc = "[run id, default the newest]",
        .flags = {{"indices", "i", T::List, nullptr, "File indices of the earlier run to retry."},
                  {"wait", "", T::Bool, nullptr, "Block until the run finishes. Default: wait in batch mode."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto old = runArg("batchRunRetry", a);
            std::vector<size_t> indices;
            if (a.has("indices"))
                for (const json& v : a.flags["indices"])
                    indices.push_back(v.is_number() ? v.get<size_t>() : size_t(std::stoul(v.get<std::string>())));
            try
            {
                return afterStart(retryRun(*old, indices), a, {});
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("batchRunRetry: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "batchRunPreset",
        .doc = "Saved batch run settings, in the batchRun prefs. With no name, every preset; with spec, saves it under name "
               "(batchRun's flags as a dict, plus inputs); with delete, removes it; else returns it.",
        .positionalDoc = "[name]",
        .flags = {{"spec", "s", T::Any, nullptr, "The settings to save."}, {"delete", "del", T::Bool, false, "Remove the preset."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.has("spec") && !a.flags["spec"].is_object())
                throw CommandError("batchRunPreset: spec must be a dict");
            return namedEntry("batchRunPreset", "/presets", a, "spec");
        },
    });

    ops::registerCommand({
        .name = "batchRunFileList",
        .doc = "Saved lists of files, folders and globs for batchRun's fileList flag. With no name, every list; with files, "
               "saves them under name; with delete, removes it; else returns it.",
        .positionalDoc = "[name]",
        .flags = {{"files", "f", T::List, nullptr, "The files, folders and globs to save."},
                  {"delete", "del", T::Bool, false, "Remove the list."}},
        .undoable = false,
        .run = [](const Args& a) -> json { return namedEntry("batchRunFileList", "/fileLists", a, "files"); },
    });
}

namespace
{
std::string camel(std::string s)
{
    for (size_t i = 0; (i = s.find('-', i)) != std::string::npos;)
    {
        s.erase(i, 1);
        if (i < s.size())
            s[i] = char(std::toupper(static_cast<unsigned char>(s[i])));
    }
    return s;
}

void usage(const CommandDef& def)
{
    std::printf("lilabatch --batch-run <files, folders or globs...> [--flag value ...]\n\n%s\n\n", def.doc.c_str());
    for (const FlagDef& f : def.flags)
        if (f.name != "wait")
            std::printf("  --%-16s %s\n", f.name.c_str(), f.doc.c_str());
    std::fflush(stdout);
}
} // namespace

int runCli(const std::vector<std::string>& args)
{
    const CommandDef* def = ops::find("batchRun");
    if (!def)
        return 2;
    json call = {{"args", json::array()}};
    try
    {
        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::string& a = args[i];
            if (a == "--help" || a == "-h")
            {
                usage(*def);
                return 0;
            }
            // -r and the like are short names; a lone "-" or a negative number is not a flag.
            const bool shortFlag = !a.starts_with("--") && a.size() > 1 && a[0] == '-' && !std::isdigit(static_cast<unsigned char>(a[1]));
            if (!a.starts_with("--") && !shortFlag)
            {
                call["args"].push_back(a);
                continue;
            }
            std::string name = a.substr(shortFlag ? 1 : 2);
            std::string value;
            const size_t eq = name.find('=');
            const bool inlineValue = eq != std::string::npos;
            if (inlineValue)
            {
                value = name.substr(eq + 1);
                name.resize(eq);
            }
            name = camel(name);
            bool negated = false;
            auto flag = std::find_if(def->flags.begin(), def->flags.end(), [&](const FlagDef& f) { return f.name == name || f.shortName == name; });
            if (flag == def->flags.end() && name.starts_with("no") && name.size() > 2)
            {
                std::string positive = name.substr(2);
                positive[0] = char(std::tolower(static_cast<unsigned char>(positive[0])));
                flag = std::find_if(def->flags.begin(), def->flags.end(), [&](const FlagDef& f) { return f.name == positive; });
                negated = flag != def->flags.end() && flag->type == ArgType::Bool;
                if (!negated)
                    flag = def->flags.end();
            }
            if (flag == def->flags.end())
                throw std::invalid_argument("unknown flag " + a + " (see --batch-run --help)");
            if (flag->type == ArgType::Bool)
            {
                call[flag->name] = negated ? false : !inlineValue || (value != "0" && value != "false" && value != "no" && value != "off");
                continue;
            }
            if (!inlineValue)
            {
                if (i + 1 >= args.size())
                    throw std::invalid_argument(a + " needs a value");
                value = args[++i];
            }
            switch (flag->type)
            {
            case ArgType::Int:
                call[flag->name] = std::stoi(value);
                break;
            case ArgType::Float:
                call[flag->name] = std::stod(value);
                break;
            case ArgType::List: {
                json& list = call[flag->name];
                if (!list.is_array())
                    list = json::array();
                size_t start = 0;
                while (start <= value.size())
                {
                    size_t end = value.find(',', start);
                    std::string part = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    if (!part.empty())
                        list.push_back(part);
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
                break;
            }
            case ArgType::Any:
                call[flag->name] = json::parse(value, nullptr, false).is_discarded() ? json(value) : json::parse(value);
                break;
            default:
                call[flag->name] = value;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "batch-run: %s\n", e.what());
        return 2;
    }

    call["wait"] = false;
    json started;
    try
    {
        started = ops::execute("batchRun", call);
    }
    catch (const CommandError& e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
    std::shared_ptr<Run> run = findRun(started["id"].get<int>());
    if (!run)
        return 2;
    if (started.contains("warnings"))
        for (const json& w : started["warnings"])
            std::fprintf(stderr, "warning: %s\n", w.get<std::string>().c_str());

    // A line per file as it finishes, then the totals.
    std::vector<bool> shown(run->items().size(), false);
    size_t finishedCount = 0;
    auto flush = [&] {
        std::vector<FileResult> rs = run->results();
        for (size_t i = 0; i < rs.size(); ++i)
        {
            if (shown[i] || !finished(rs[i].status))
                continue;
            shown[i] = true;
            ++finishedCount;
            std::string line = "[" + std::to_string(finishedCount) + "/" + std::to_string(rs.size()) + "] " + statusName(rs[i].status) + " " +
                               paths::toUtf8(run->items()[i].path);
            if (!rs[i].message.empty())
                line += ": " + rs[i].message;
            if (!rs[i].errorFile.empty())
                line += " (" + rs[i].errorFile + ":" + std::to_string(rs[i].errorLine) + ")";
            std::printf("%s\n", line.c_str());
        }
        std::fflush(stdout);
    };
    while (!run->wait(0.1))
        flush();
    flush();
    std::printf("\n%s", run->report(false).c_str());
    std::fflush(stdout);
    json s = run->summary(false);
    return s["counts"]["failed"].get<int>() > 0 || s["counts"]["cancelled"].get<int>() > 0 ? 1 : 0;
}
} // namespace lila::batch
