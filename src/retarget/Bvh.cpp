#include "retarget/Bvh.h"

#include "retarget/SceneRetarget.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/Transform.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace lila::retarget::bvh
{
int File::channelCount() const
{
    int n = 0;
    for (const Joint& j : joints)
        n += int(j.channels.size());
    return n;
}

namespace
{
struct Reader
{
    std::istringstream in;
    int line = 1;
    explicit Reader(const std::string& text) : in(text) {}

    std::string next()
    {
        std::string tok;
        char c;
        while (in.get(c))
        {
            if (c == '\n')
                ++line;
            if (std::isspace(static_cast<unsigned char>(c)))
            {
                if (!tok.empty())
                    return tok;
                continue;
            }
            tok += c;
        }
        return tok;
    }
    [[noreturn]] void fail(const std::string& what) const { throw std::invalid_argument("BVH line " + std::to_string(line) + ": " + what); }
    std::string expect(const char* want)
    {
        std::string t = next();
        if (t != want)
            fail(std::string("expected ") + want + ", found '" + t + "'");
        return t;
    }
    double number()
    {
        const std::string t = next();
        try
        {
            size_t used = 0;
            const double v = std::stod(t, &used);
            if (used != t.size())
                fail("'" + t + "' is not a number");
            return v;
        }
        catch (const std::logic_error&)
        {
            fail("'" + t + "' is not a number");
        }
    }
    /** The rest of the line, trimmed (joint names may hold spaces). */
    std::string restOfLine()
    {
        std::string s;
        std::getline(in, s);
        ++line;
        const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
};

void joint(Reader& r, File& f, int parent, bool end, const std::string& name)
{
    Joint j;
    j.name = name;
    j.parent = parent;
    j.endSite = end;
    const int self = int(f.joints.size());
    f.joints.push_back(j);
    r.expect("{");
    for (std::string t = r.next();; t = r.next())
    {
        if (t.empty())
            r.fail("the file ends inside " + name);
        if (t == "}")
            return;
        if (t == "OFFSET")
        {
            const double x = r.number(), y = r.number(), z = r.number();
            f.joints[size_t(self)].offset = {x, y, z};
        }
        else if (t == "CHANNELS")
        {
            const int n = int(r.number());
            if (n < 0 || n > 9)
                r.fail("bad channel count");
            for (int k = 0; k < n; ++k)
                f.joints[size_t(self)].channels.push_back(r.next());
        }
        else if (t == "JOINT")
            joint(r, f, self, false, r.restOfLine());
        else if (t == "End")
        {
            r.restOfLine(); // "Site"
            joint(r, f, self, true, name + "_end");
        }
        else
            r.fail("unexpected '" + t + "'");
    }
}
} // namespace

File parse(const std::string& text)
{
    Reader r(text);
    File f;
    r.expect("HIERARCHY");
    for (std::string t = r.next(); t != "MOTION"; t = r.next())
    {
        if (t.empty())
            r.fail("no MOTION section");
        if (t != "ROOT")
            r.fail("expected ROOT, found '" + t + "'");
        joint(r, f, -1, false, r.restOfLine());
    }
    r.expect("Frames:");
    f.frames = int(r.number());
    r.expect("Frame");
    r.expect("Time:");
    f.frameTime = r.number();
    if (f.frames < 0 || !(f.frameTime > 0.0))
        r.fail("bad frame count or frame time");
    const size_t n = size_t(f.channelCount()) * size_t(f.frames);
    f.values.reserve(n);
    for (size_t i = 0; i < n; ++i)
        f.values.push_back(r.number());
    return f;
}

ImportResult import(scene::Graph& g, const std::filesystem::path& path, const ImportOptions& opts)
{
    using namespace scene;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::invalid_argument("can't read " + path.string());
    std::stringstream ss;
    ss << in.rdbuf();
    const File f = parse(ss.str());

    ImportResult out;
    out.frames = f.frames;
    out.fps = 1.0 / f.frameTime;
    out.end = f.frames > 0 ? double(f.frames - 1) * f.frameTime : 0.0;
    const NodeType& jointType = *types::find("joint");
    std::vector<NodeId> ids(f.joints.size(), kNoNode);
    std::vector<int> firstChannel(f.joints.size(), 0);
    int channel = 0;
    for (size_t i = 0; i < f.joints.size(); ++i)
    {
        const Joint& j = f.joints[i];
        firstChannel[i] = channel;
        channel += int(j.channels.size());
        if (j.endSite && !opts.endSites)
            continue;
        const NodeId parent = j.parent >= 0 ? ids[size_t(j.parent)] : opts.parent;
        Node* n = g.createNode(jointType, j.name, parent);
        ids[i] = n->id();
        g.setValue({ids[i], Transform::kTranslate}, j.offset * opts.scale);
        // BVH lists rotations outermost first; Lila's order names the first-applied axis first.
        std::string axes;
        for (const std::string& c : j.channels)
            if (c.size() > 1 && c.substr(1) == "rotation")
                axes.insert(axes.begin(), char(std::tolower(static_cast<unsigned char>(c[0]))));
        static const char* kOrders[] = {"xyz", "yzx", "zxy", "xzy", "yxz", "zyx"};
        for (int o = 0; o < 6; ++o)
            if (axes == kOrders[o])
                g.setValue({ids[i], Transform::kRotateOrder}, int64_t(o));
        if (j.endSite)
            g.setValue({ids[i], scene::Joint::kDeform}, false);
        out.joints.push_back(ids[i]);
        if (j.parent < 0 && out.root == kNoNode)
            out.root = ids[i];
    }

    std::vector<double> times(size_t(f.frames));
    for (int k = 0; k < f.frames; ++k)
        times[size_t(k)] = double(k) * f.frameTime;
    const int stride = f.channelCount();
    for (size_t i = 0; i < f.joints.size(); ++i)
    {
        const Joint& j = f.joints[i];
        if (ids[i] == kNoNode || j.channels.empty() || f.frames == 0)
            continue;
        std::vector<glm::dvec3> translate(times.size(), j.offset * opts.scale), euler(times.size(), glm::dvec3(0.0));
        bool moves = false, turns = false;
        for (size_t c = 0; c < j.channels.size(); ++c)
        {
            const std::string& name = j.channels[c];
            if (name.size() < 2)
                continue;
            const int axis = std::tolower(static_cast<unsigned char>(name[0])) - 'x';
            if (axis < 0 || axis > 2)
                continue;
            const bool position = name.substr(1) == "position";
            for (int k = 0; k < f.frames; ++k)
            {
                const double v = f.values[size_t(k) * size_t(stride) + size_t(firstChannel[i]) + c];
                if (position)
                    translate[size_t(k)][axis] = v * opts.scale;
                else
                    euler[size_t(k)][axis] = v;
            }
            moves = moves || position;
            turns = turns || !position;
        }
        // Euler values from the file wrap at 180; keep each frame continuous with the last.
        for (size_t k = 1; k < euler.size(); ++k)
            for (int a = 0; a < 3; ++a)
                euler[k][a] -= 360.0 * std::round((euler[k][a] - euler[k - 1][a]) / 360.0);
        if (!euler.empty())
            g.setValue({ids[i], Transform::kRotate}, euler.front());
        keyJoint(g, ids[i], times, moves ? &translate : nullptr, turns ? &euler : nullptr);
    }
    if (opts.setPlayback && f.frames > 0)
    {
        anim::Playback& pb = anim::playback();
        pb.fps = out.fps;
        pb.minTime = pb.animStart = 0.0;
        pb.maxTime = pb.animEnd = out.end;
        anim::playbackChanged();
    }
    return out;
}
} // namespace lila::retarget::bvh
