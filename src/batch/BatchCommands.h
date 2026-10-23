#pragma once

#include "core/Export.h"

#include <filesystem>
#include <string>
#include <vector>

namespace lila::batch
{
/** batchRun, batchRunStatus, batchRunCancel, batchRunRetry, batchRunPreset, batchRunFileList. */
LILA_SCENE_API void registerCommands();

/**
 * `lilabatch --batch-run <inputs...> --scriptFile fix.py --save overwrite ...`: batchRun's flags as --name value (or
 * --name=value, kebab-case accepted, --no-name for false, -short names too), progress per file on stdout. 0 when nothing failed, 1 when
 * some file did, 2 for a bad call.
 */
LILA_SCENE_API int runCli(const std::vector<std::string>& args);

/** lilabatch --batch-run-worker <job.json> <index>: processes one file of a run and writes its result. */
LILA_SCENE_API int runWorker(const std::filesystem::path& jobFile, size_t index, bool pythonOk);
} // namespace lila::batch
