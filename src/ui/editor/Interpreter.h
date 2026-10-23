#pragma once

#include "core/Export.h"

#include "ui/editor/EditorTypes.h"

#include <QList>
#include <QString>

/** What the editor asks of the live embedded interpreter. Everything here is a no-op until Python is up. */
namespace lila::ui::interp
{
/** Attributes of a dotted name in __main__ (or __main__'s globals for an empty base). Never calls anything. */
LILA_UI_API QList<CompletionEntry> complete(const QString& base);

/** Docstring and signature for a dotted name, or empty. */
LILA_UI_API QString describe(const QString& dotted);

/** "ops.polyCube" -> "lila.ops.polyCube" via __module__ and __qualname__; empty if the name does not resolve. */
LILA_UI_API QString qualifiedName(const QString& dotted);

/** True if __main__ or builtins defines `name`, like the commands Lila exposes as bare names. */
LILA_UI_API bool hasGlobal(const QString& name);

/** Lets tracebacks for unsaved tabs print their source lines. */
LILA_UI_API void registerSource(const QString& filename, const QString& text);

/** The interpreter executable that owns the embedded runtime's stdlib, for pip. Empty if unknown. */
LILA_UI_API QString basePython();
} // namespace lila::ui::interp
