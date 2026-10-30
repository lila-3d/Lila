#include "render/CubeLut.h"

#include "core/Paths.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace lila::render
{
namespace
{
struct Cached
{
    std::filesystem::file_time_type stamp;
    std::shared_ptr<const Lut3D> lut;
};

std::shared_ptr<const Lut3D> parse(std::istream& in, std::string* err)
{
    auto lut = std::make_shared<Lut3D>();
    glm::vec3 lo(0.0f), hi(1.0f);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ls(line);
        std::string word;
        ls >> word;
        if (word == "LUT_3D_SIZE")
            ls >> lut->size;
        else if (word == "LUT_1D_SIZE")
        {
            if (err)
                *err = "1D LUTs are not supported";
            return nullptr;
        }
        else if (word == "DOMAIN_MIN")
            ls >> lo.r >> lo.g >> lo.b;
        else if (word == "DOMAIN_MAX")
            ls >> hi.r >> hi.g >> hi.b;
        else if (word == "TITLE")
            continue;
        else if (!word.empty() && (std::isdigit(static_cast<unsigned char>(word[0])) || word[0] == '-' || word[0] == '.'))
        {
            float r = std::stof(word), g = 0.0f, b = 0.0f;
            ls >> g >> b;
            lut->rgb.insert(lut->rgb.end(), {r, g, b});
        }
    }
    const size_t want = size_t(lut->size) * lut->size * lut->size * 3;
    if (lut->size < 2 || lut->size > 256 || lut->rgb.size() != want)
    {
        if (err)
            *err = "not a 3D .cube table";
        return nullptr;
    }
    // Lila samples the table over 0..1; a domain other than that is folded into the entries' inputs only approximately.
    if (lo != glm::vec3(0.0f) || hi != glm::vec3(1.0f))
        for (size_t i = 0; i < lut->rgb.size(); ++i)
            lut->rgb[i] = (lut->rgb[i] - lo[int(i % 3)]) / std::max(hi[int(i % 3)] - lo[int(i % 3)], 1e-6f);
    return lut;
}
} // namespace

std::shared_ptr<const Lut3D> loadCubeLut(const std::string& path, std::string* err)
{
    static std::mutex mutex;
    static std::map<std::string, Cached> cache;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(paths::fromUtf8(path), ec);
    if (ec)
    {
        if (err)
            *err = "cannot read " + path;
        return nullptr;
    }
    std::scoped_lock lock(mutex);
    if (auto it = cache.find(path); it != cache.end() && it->second.stamp == stamp)
        return it->second.lut;
    std::ifstream in(paths::fromUtf8(path));
    if (!in)
    {
        if (err)
            *err = "cannot read " + path;
        return nullptr;
    }
    std::shared_ptr<const Lut3D> lut = parse(in, err);
    cache[path] = {stamp, lut};
    return lut;
}
} // namespace lila::render
