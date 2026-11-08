#include "link/AssemblySend.h"

#include "core/Profile.h"

#include <fstream>
#include <sstream>

namespace lila::link
{
bool writeNaniteAssemblyUsda(const fs::path& path, const std::string& root, const std::vector<std::string>& parts,
                             const std::vector<AssemblyInstance>& instances, std::string* err)
{
    LILA_ZONE();
    std::ostringstream out;
    out.precision(9);
    out << "#usda 1.0\n(\n    defaultPrim = \"" << root << "\"\n    metersPerUnit = 0.01\n    upAxis = \"Z\"\n)\n\n";
    out << "def Xform \"" << root << "\" (\n    prepend apiSchemas = [\"NaniteAssemblyRootAPI\"]\n)\n{\n";
    out << "    def PointInstancer \"Instances\"\n    {\n        rel prototypes = [";
    for (size_t r = 0; r < parts.size(); ++r)
        out << (r ? ", " : "") << "</" << root << "/Instances/Parts/Part_" << r << ">";
    out << "]\n        int[] protoIndices = [";
    for (size_t k = 0; k < instances.size(); ++k)
        out << (k ? ", " : "") << instances[k].part;
    // Y negated: Unreal's USD importer mirrors Y on a Z-up stage.
    out << "]\n        point3f[] positions = [";
    for (size_t k = 0; k < instances.size(); ++k)
    {
        const double* c = instances[k].location;
        out << (k ? ", " : "") << "(" << c[0] << ", " << -c[1] << ", " << c[2] << ")";
    }
    out << "]\n        quath[] orientations = [";
    for (size_t k = 0; k < instances.size(); ++k)
    {
        const double* q = instances[k].rotation;
        out << (k ? ", " : "") << "(" << q[3] << ", " << -q[0] << ", " << q[1] << ", " << -q[2] << ")";
    }
    out << "]\n        float3[] scales = [";
    for (size_t k = 0; k < instances.size(); ++k)
    {
        const double* s = instances[k].scale;
        out << (k ? ", " : "") << "(" << s[0] << ", " << s[1] << ", " << s[2] << ")";
    }
    out << "]\n\n        def Scope \"Parts\"\n        {\n";
    for (size_t r = 0; r < parts.size(); ++r)
        out << "            def Xform \"Part_" << r << "\" (\n                prepend apiSchemas = [\"NaniteAssemblyExternalRefAPI\"]\n            )\n"
            << "            {\n                uniform token unreal:naniteAssembly:meshAssetPath = \"" << parts[r] << "\"\n            }\n";
    out << "        }\n    }\n}\n";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        if (err)
            *err = "cannot write " + path.string();
        return false;
    }
    file << out.str();
    return bool(file);
}

std::vector<AssemblyInstance> assemblyInstances(const json& groups, std::vector<size_t>& partGroups)
{
    std::vector<AssemblyInstance> out;
    partGroups.clear();
    for (size_t gi = 0; gi < groups.size(); ++gi)
    {
        const json& grp = groups[gi];
        if (grp.value("remove", false) || grp.value("placements", json::array()).empty())
            continue;
        const int part = int(partGroups.size());
        partGroups.push_back(gi);
        for (const json& p : grp["placements"])
        {
            AssemblyInstance a;
            a.part = part;
            for (int i = 0; i < 3; ++i)
            {
                a.location[i] = p["location"][i].get<double>();
                a.scale[i] = p["scale"][i].get<double>();
            }
            for (int i = 0; i < 4; ++i)
                a.rotation[i] = p["rotation"][i].get<double>();
            out.push_back(a);
        }
    }
    return out;
}
} // namespace lila::link
