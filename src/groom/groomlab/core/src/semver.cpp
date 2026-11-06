// SPDX-License-Identifier: Apache-2.0
#include "groomlab/semver.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace groomlab {

namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }
bool identifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-'; }
bool allDigits(std::string_view s) { return !s.empty() && std::all_of(s.begin(), s.end(), digit); }

// Decimal digits without their leading zeros ("0" for all zeros).
std::string stripZeros(std::string_view s) {
    const std::size_t first = s.find_first_not_of('0');
    return first == std::string_view::npos ? std::string("0") : std::string(s.substr(first));
}

// Two numbers of decimal digits (no leading zeros), compared numerically.
int compareNumbers(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    const int c = a.compare(b);
    return (c > 0) - (c < 0);
}

// `text` split at dots: every part non-empty and of `ok` characters, or false.
template <typename Ok>
bool splitDots(std::string_view text, Ok ok, std::vector<std::string_view>* parts) {
    std::size_t start = 0;
    while (true) {
        const std::size_t dot = text.find('.', start);
        const std::string_view part = text.substr(start, dot == std::string_view::npos ? std::string_view::npos : dot - start);
        if (part.empty() || !std::all_of(part.begin(), part.end(), ok)) return false;
        parts->push_back(part);
        if (dot == std::string_view::npos) return true;
        start = dot + 1;
    }
}

}  // namespace

std::optional<SemVer> parseSemVer(std::string_view text) {
    const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    if (!text.empty() && text.front() == 'v') text.remove_prefix(1);
    // "+build" metadata: ignored, but well formed.
    const std::size_t plus = text.find('+');
    if (plus != std::string_view::npos) {
        std::vector<std::string_view> build;
        if (!splitDots(text.substr(plus + 1), identifierChar, &build)) return std::nullopt;
        text = text.substr(0, plus);
    }
    // The numbers end at the first "-" (a pre-release's identifiers may hold more of them).
    const std::size_t dash = text.find('-');
    std::vector<std::string_view> numbers, pre;
    if (!splitDots(text.substr(0, dash), digit, &numbers)) return std::nullopt;
    if (dash != std::string_view::npos && !splitDots(text.substr(dash + 1), identifierChar, &pre)) return std::nullopt;
    SemVer v;
    for (std::string_view n : numbers) v.numbers.push_back(stripZeros(n));
    while (v.numbers.size() > 1 && v.numbers.back() == "0") v.numbers.pop_back();
    for (std::string_view p : pre) v.prerelease.emplace_back(p);
    return v;
}

int compareVersions(std::string_view a, std::string_view b) {
    const std::optional<SemVer> va = parseSemVer(a), vb = parseSemVer(b);
    if (!va || !vb) throw std::invalid_argument("not a version: '" + std::string(!va ? a : b) + "'");
    for (std::size_t i = 0; i < std::max(va->numbers.size(), vb->numbers.size()); ++i) {
        const std::string x = i < va->numbers.size() ? va->numbers[i] : "0", y = i < vb->numbers.size() ? vb->numbers[i] : "0";
        if (const int c = compareNumbers(x, y)) return c;
    }
    const bool releaseA = va->prerelease.empty(), releaseB = vb->prerelease.empty();
    if (releaseA != releaseB) return releaseA ? 1 : -1;  // a release after its pre-releases
    for (std::size_t i = 0; i < std::min(va->prerelease.size(), vb->prerelease.size()); ++i) {
        const std::string &x = va->prerelease[i], &y = vb->prerelease[i];
        const bool nx = allDigits(x), ny = allDigits(y);
        if (nx != ny) return nx ? -1 : 1;  // numeric identifiers below alphanumeric ones
        const int c = nx ? compareNumbers(stripZeros(x), stripZeros(y)) : (x.compare(y) > 0) - (x.compare(y) < 0);
        if (c) return c;
    }
    const std::size_t na = va->prerelease.size(), nb = vb->prerelease.size();
    return (na > nb) - (na < nb);
}

}  // namespace groomlab
