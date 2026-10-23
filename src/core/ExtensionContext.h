#pragma once

#include "cmds/Command.h"
#include "core/Events.h"
#include "core/Export.h"
#include "core/Log.h"
#include "core/Registry.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** The SDK version extensions compile against. Lila loads an extension when the major matches and the minor is no newer than its own. */
#define LILA_SDK_VERSION_MAJOR 1
#define LILA_SDK_VERSION_MINOR 0

namespace lila::ext
{
struct SdkVersion
{
    // Not major/minor: glibc's <sys/sysmacros.h> defines macros with those names.
    int majorVersion = 0;
    int minorVersion = 0;

    /** "1.0" or "1"; nullopt for anything else. */
    LILA_FOUNDATION_API static std::optional<SdkVersion> parse(std::string_view text);
    LILA_FOUNDATION_API std::string str() const;
    static constexpr uint32_t pack(int majorVersion, int minorVersion) { return (uint32_t(majorVersion) << 16) | uint32_t(minorVersion & 0xffff); }
    static constexpr SdkVersion unpack(uint32_t v) { return {int(v >> 16), int(v & 0xffff)}; }
};

/** What the extension's headers said, compiled into it. */
inline constexpr SdkVersion kSdkVersion{LILA_SDK_VERSION_MAJOR, LILA_SDK_VERSION_MINOR};

/** What this Lila provides. */
LILA_FOUNDATION_API SdkVersion hostSdkVersion();

/** Empty when an extension built for `extension` can load into `host`, else why not. */
LILA_FOUNDATION_API std::string sdkMismatch(SdkVersion extension, SdkVersion host = hostSdkVersion());

/**
 * What an extension registers through. Every registration is tagged with owner() and tracked here, so unloading
 * removes everything the extension added, newest first, whatever system it went into. Systems outside Lila Foundation
 * take a Context& in their own registration functions (scene::types::registerType(ctx, type), for instance) and record
 * their removal with track().
 *
 * Main thread only.
 */
class LILA_FOUNDATION_API Context
{
public:
    using Handle = uint64_t;

    /** root: the extension's package folder (resources live under it). */
    explicit Context(std::string owner, std::filesystem::path root = {});
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    const std::string& owner() const;
    const std::filesystem::path& root() const;
    std::filesystem::path resources() const { return root() / "resources"; }

    /** Records how to undo a registration made elsewhere. what names it for listings ("command:hello"). Never 0. */
    Handle track(std::string what, std::function<void()> remove);
    /** Undoes one registration now. False when it is not (or no longer) registered. */
    bool remove(Handle handle);
    /** Undoes every registration, newest first. Lila calls this after lilaExtensionUninit. */
    void unloadAll();

    /** "kind:id" per live registration, oldest first. */
    std::vector<std::string> registrations() const;
    size_t registrationCount() const;

    /** Adds to any owner-tagged registry. Throws std::invalid_argument when id is taken. */
    template <class T> Handle add(Registry<T>& registry, std::string kind, std::string id, T item)
    {
        registry.add(id, std::move(item), owner());
        std::string own = owner();
        return track(kind + ":" + id, [&registry, id, own] {
            if (const std::string* o = registry.ownerOf(id); o && *o == own)
                registry.remove(id);
        });
    }

    /** Takes def, owned by this extension. 0, with a log message, when the name belongs to Lila or another extension. */
    Handle registerCommand(CommandDef def);
    Handle subscribe(events::Event event, events::Handler handler);

    /** Logged with the extension's name in front. */
    void log(LogLevel level, std::string_view text) const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace lila::ext
