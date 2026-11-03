#include "batch/BatchCommands.h"

#include "batch/BatchJob.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "io/FileTranslators.h"
#include "ops/Command.h"
#include "python/Python.h"
#include "vcs/SourceControl.h"

#include <chrono>
#include <fstream>
#include <sstream>

namespace lila::batch
{
namespace
{
json readJson(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return json::parse(ss.str());
}

void writeJson(const fs::path& p, const json& j)
{
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << j.dump(1);
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
}

/** Makes out writable per the spec: a source control checkout first, then clearing the flag when allowed. */
bool prepareWrite(const Spec& spec, const fs::path& out, std::string& why)
{
    if (!isReadOnly(out))
        return true;
    if (spec.checkout == "auto")
    {
        if (vcs::SourceControl::get().requestWrite(out, &why, true) == vcs::WriteAccess::Writable && !isReadOnly(out))
        {
            log::info("batch: checked out {}", paths::toUtf8(out));
            return true;
        }
    }
    else
        why = paths::toUtf8(out) + " is read-only";
    if (spec.makeWritable)
    {
        std::error_code ec;
        fs::permissions(out, fs::perms::owner_write, fs::perm_options::add, ec);
        if (!ec && !isReadOnly(out))
        {
            log::info("batch: made {} writable", paths::toUtf8(out));
            return true;
        }
    }
    return false;
}

/** The script's half: lila.batch runs it with ctx in its globals and writes what happened to outcomePath. */
json runScript(const json& context, const fs::path& dir, size_t index)
{
    const fs::path ctxPath = dir / (std::to_string(index) + ".ctx.json");
    const fs::path outcomePath = dir / (std::to_string(index) + ".outcome.json");
    writeJson(ctxPath, context);
    std::error_code ec;
    fs::remove(outcomePath, ec);
    const std::string code = "import lila.batch\nlila.batch._run_worker(" + json(paths::toUtf8(ctxPath)).dump() + ", " +
                             json(paths::toUtf8(outcomePath)).dump() + ")\n";
    python::run(code, "<batch worker>");
    if (!fs::exists(outcomePath, ec))
        return {{"ok", false}, {"message", "the script runner did not report back; see the log"}};
    return readJson(outcomePath);
}
} // namespace

int runWorker(const fs::path& jobFile, size_t index, bool pythonOk)
{
    LILA_ZONE();

    const auto started = std::chrono::steady_clock::now();
    json job;
    try
    {
        job = readJson(jobFile);
    }
    catch (const std::exception& e)
    {
        log::error("batch worker: cannot read {}: {}", paths::toUtf8(jobFile), e.what());
        return 3;
    }
    const fs::path dir = jobFile.parent_path();
    const json& items = job["items"];
    if (!items.is_array() || index >= items.size())
    {
        log::error("batch worker: no file {} in {}", index, paths::toUtf8(jobFile));
        return 3;
    }
    Spec spec;
    try
    {
        spec = specFromJson(job["spec"]);
    }
    catch (const std::exception& e)
    {
        log::error("batch worker: {}", e.what());
        return 3;
    }
    const fs::path file = paths::fromUtf8(items[index].value("path", std::string()));
    const fs::path out = paths::fromUtf8(items[index].value("output", std::string()));

    FileResult r;
    r.status = Status::Failed;
    auto finish = [&]() {
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        writeJson(dir / (std::to_string(index) + ".result.json"), toJson(r));
        return 0;
    };

    log::info("batch: {} ({}/{})", paths::toUtf8(file), index + 1, items.size());
    if (!spec.workspace.empty() && fs::is_directory(paths::fromUtf8(spec.workspace)))
        Workspace::get().open(paths::fromUtf8(spec.workspace));
    else if (auto ws = Workspace::detect(file))
        Workspace::get().open(*ws);

    if (!pythonOk)
    {
        r.message = "Python did not start in the worker";
        return finish();
    }
    try
    {
        io::translators::run(io::translators::Op::Open, file);
    }
    catch (const std::exception& e)
    {
        r.message = std::string("cannot open: ") + e.what();
        return finish();
    }

    const json context = {{"file", paths::toUtf8(file)},
                          {"output", paths::toUtf8(out)},
                          {"outputDir", spec.outputDir},
                          {"index", index},
                          {"count", items.size()},
                          {"runId", job.value("runId", 0)},
                          {"args", spec.args},
                          {"script", spec.script},
                          {"scriptFile", spec.scriptFile}};
    json outcome = runScript(context, dir, index);
    if (outcome.contains("results") && outcome["results"].is_object())
        r.results = outcome["results"];
    r.message = outcome.value("message", std::string());
    r.errorFile = outcome.value("errorFile", std::string());
    r.errorLine = outcome.value("errorLine", 0);
    r.traceback = outcome.value("traceback", std::string());
    if (!outcome.value("ok", false))
        return finish();
    if (outcome.value("skipped", false))
    {
        r.status = Status::Skipped;
        return finish();
    }

    if (spec.save != SaveMode::None && !out.empty() && outcome.value("save", true))
    {
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        std::string why;
        if (!prepareWrite(spec, out, why))
        {
            r.message = "cannot save: " + (why.empty() ? paths::toUtf8(out) + " is read-only" : why);
            return finish();
        }
        try
        {
            ops::execute("saveScene", {{"args", {paths::toUtf8(out)}}});
            r.saved = true;
            log::info("batch: saved {}", paths::toUtf8(out));
        }
        catch (const std::exception& e)
        {
            r.message = std::string("cannot save: ") + e.what();
            return finish();
        }
        if (spec.checkout == "auto")
            vcs::markForAddIfNew(out);
    }
    r.status = Status::Done;
    return finish();
}
} // namespace lila::batch
