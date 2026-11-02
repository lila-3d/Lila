#pragma once

/**
 * Preferences, and the studio restrictions documenting which ones a studio may lock: PrefStore::get().isLocked(file,
 * key) for any pref, restrictions::add(ctx, id, {label, doc, file, key}) to list an extension's own in
 * studioRestrictions.
 */
#include "core/PrefStore.h"
#include "ops/StudioRestrictions.h"
