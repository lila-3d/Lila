// SPDX-License-Identifier: Apache-2.0
#include "groomlab/paths.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace groomlab {

namespace {

namespace fs = std::filesystem;

thread_local const PathResolver* tCurrent = nullptr;

bool isNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool lookup(const std::string& name, std::string* value) {
    const std::optional<std::string> v = environmentVariable(name);
    if (!v) return false;
    *value = *v;
    return true;
}

}  // namespace

std::optional<std::string> environmentVariable(const std::string& name) {
#ifdef _MSC_VER
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, name.c_str()) != 0 || !buffer) return std::nullopt;
    std::string value = buffer;
    std::free(buffer);
    return value;
#else
    const char* v = std::getenv(name.c_str());
    if (!v) return std::nullopt;
    return std::string(v);
#endif
}

std::string expandEnvironment(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (std::size_t i = 0; i < path.size();) {
        const char c = path[i];
        std::string name;
        std::size_t end = i;
        if (c == '$' && i + 1 < path.size() && path[i + 1] == '{') {
            const std::size_t close = path.find('}', i + 2);
            if (close != std::string::npos) {
                name = path.substr(i + 2, close - i - 2);
                end = close + 1;
            }
        } else if (c == '$') {
            std::size_t j = i + 1;
            while (j < path.size() && isNameChar(path[j])) ++j;
            name = path.substr(i + 1, j - i - 1);
            end = j;
#ifdef _WIN32
        } else if (c == '%') {
            const std::size_t close = path.find('%', i + 1);
            if (close != std::string::npos && close > i + 1) {
                name = path.substr(i + 1, close - i - 1);
                end = close + 1;
            }
#endif
        }
        std::string value;
        if (!name.empty() && lookup(name, &value)) {
            out += value;
            i = end;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

bool isAbsolutePath(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] == '/' || path[0] == '\\') return true;  // POSIX root, UNC share, drive-relative root
    return fs::u8path(path).is_absolute();
}

std::string resolvePath(const std::string& path) {
    const std::string expanded = expandEnvironment(path);
    if (expanded.empty() || isAbsolutePath(expanded) || !tCurrent || !*tCurrent) return expanded;
    const std::string resolved = (*tCurrent)(expanded);
    return resolved.empty() ? expanded : resolved;
}

PathResolver searchPathResolver(std::vector<std::string> directories) {
    return [dirs = std::move(directories)](const std::string& relative) -> std::string {
        std::string first;
        for (const std::string& dir : dirs) {
            if (dir.empty()) continue;
            const fs::path candidate = (fs::u8path(expandEnvironment(dir)) / fs::u8path(relative)).lexically_normal();
            std::error_code ec;
            if (fs::exists(candidate, ec)) return candidate.u8string();
            if (first.empty()) first = candidate.u8string();
        }
        return first;
    };
}

std::string resolvePathIn(const std::string& path, std::vector<std::string> directories) {
    const PathResolver resolver = searchPathResolver(std::move(directories));
    const ScopedPathResolver scope(&resolver);
    return resolvePath(path);
}

namespace {

std::string forward(std::string path) {
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

// The path as the file system names it (its existing part resolved: case, links, short names).
fs::path canonical(const std::string& path) {
    std::error_code ec;
    const fs::path p = fs::weakly_canonical(fs::u8path(path), ec);
    return ec ? fs::u8path(path).lexically_normal() : p;
}

bool startsWithParent(const fs::path& relative) {
    const auto first = relative.begin();
    return first != relative.end() && *first == "..";
}

}  // namespace

std::string portablePath(const std::string& path, const std::vector<std::string>& folders, bool beside) {
    if (path.empty() || path.find('$') != std::string::npos || path.find('%') != std::string::npos) return path;
    if (!isAbsolutePath(path)) return forward(path);
    const fs::path full = canonical(path);
    std::vector<fs::path> bases;
    for (const std::string& folder : folders) {
        if (!folder.empty()) bases.push_back(canonical(folder));
    }
    // `full` relative to `base` when both are on one volume ("C:" and "c:" are one drive, which
    // lexically_relative would not see), else empty.
    auto relativeTo = [&full](const fs::path& base) {
        auto volume = [](const fs::path& p) {
            std::string root = p.root_name().generic_u8string();
            for (char& c : root) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return root;
        };
        if (volume(full) != volume(base)) return fs::path();
        return full.relative_path().lexically_relative(base.relative_path());
    };
    for (const fs::path& base : bases) {
        const fs::path relative = relativeTo(base);
        if (!relative.empty() && !startsWithParent(relative)) return relative.generic_u8string();
    }
    if (beside && !bases.empty()) {
        const fs::path relative = relativeTo(bases.front());
        if (!relative.empty()) return relative.generic_u8string();
    }
    return forward(path);
}

ScopedPathResolver::ScopedPathResolver(const PathResolver* resolver) : previous_(tCurrent) {
    if (resolver) tCurrent = resolver;
}

ScopedPathResolver::~ScopedPathResolver() { tCurrent = previous_; }

}  // namespace groomlab
