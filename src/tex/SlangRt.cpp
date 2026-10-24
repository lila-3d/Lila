#include "tex/SlangRt.h"

#include "core/Log.h"
#include "core/Profile.h"

#include <slang-com-ptr.h>
#include <slang.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace lila::tex::slangrt
{
namespace
{
struct State
{
    std::mutex mutex;
    Slang::ComPtr<slang::IGlobalSession> global;
    bool tried = false;
    std::string globalError;
    std::map<uint64_t, std::shared_ptr<const CpuKernel>> kernels;
    CacheStats stats;
    int moduleCounter = 0;
};

State& state()
{
    static State s;
    return s;
}

std::string text(slang::IBlob* blob) { return blob ? std::string(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize()) : std::string(); }

/** Under the state mutex. */
slang::IGlobalSession* global(std::string* err)
{
    State& s = state();
    if (!s.tried)
    {
        s.tried = true;
        if (SLANG_FAILED(slang::createGlobalSession(s.global.writeRef())))
            s.globalError = "Slang couldn't start";
    }
    if (!s.global && err)
        *err = s.globalError;
    return s.global.get();
}

/** Loads source as a module, links it with texMain. Under the state mutex. */
Slang::ComPtr<slang::IComponentType> link(slang::ISession* session, const std::string& source, std::string* err)
{
    State& s = state();
    Slang::ComPtr<slang::IBlob> diag;
    const std::string name = "texpass" + std::to_string(++s.moduleCounter);
    slang::IModule* module = session->loadModuleFromSourceString(name.c_str(), (name + ".slang").c_str(), source.c_str(), diag.writeRef());
    if (!module)
    {
        *err = text(diag);
        return {};
    }
    Slang::ComPtr<slang::IEntryPoint> entry;
    if (SLANG_FAILED(module->findEntryPointByName("texMain", entry.writeRef())))
    {
        *err = "no texMain entry point";
        return {};
    }
    slang::IComponentType* parts[] = {module, entry.get()};
    Slang::ComPtr<slang::IComponentType> program;
    if (SLANG_FAILED(session->createCompositeComponentType(parts, 2, program.writeRef(), diag.writeRef())))
    {
        *err = text(diag);
        return {};
    }
    Slang::ComPtr<slang::IComponentType> linked;
    if (SLANG_FAILED(program->link(linked.writeRef(), diag.writeRef())))
    {
        *err = text(diag);
        return {};
    }
    return linked;
}

std::shared_ptr<const CpuKernel> compileCpu(const std::string& source, std::string* err)
{
    LILA_ZONE();
    slang::IGlobalSession* g = global(err);
    if (!g)
        return nullptr;
    slang::TargetDesc target{};
    target.format = SLANG_SHADER_HOST_CALLABLE;
    target.profile = g->findProfile("sm_5_0");
    slang::SessionDesc desc{};
    desc.targets = &target;
    desc.targetCount = 1;
    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(g->createSession(desc, session.writeRef())))
    {
        *err = "couldn't create a Slang session";
        return nullptr;
    }
    Slang::ComPtr<slang::IComponentType> linked = link(session.get(), source, err);
    if (!linked)
        return nullptr;
    Slang::ComPtr<slang::IBlob> diag;
    Slang::ComPtr<ISlangSharedLibrary> lib;
    if (SLANG_FAILED(linked->getEntryPointHostCallable(0, 0, lib.writeRef(), diag.writeRef())) || !lib)
    {
        *err = "CPU compile failed: " + text(diag);
        return nullptr;
    }
    auto kernel = std::make_shared<CpuKernel>();
    kernel->fn = reinterpret_cast<CpuKernel::Fn>(lib->findFuncByName("texMain"));
    if (!kernel->fn)
    {
        *err = "compiled pass has no texMain";
        return nullptr;
    }
    slang::ProgramLayout* layout = linked->getLayout();
    bool found[4] = {false, false, false, false};
    for (unsigned i = 0; i < layout->getParameterCount(); ++i)
    {
        slang::VariableLayoutReflection* p = layout->getParameterByIndex(i);
        const std::string name = p->getName() ? p->getName() : "";
        const size_t off = p->getOffset(SLANG_PARAMETER_CATEGORY_UNIFORM);
        const size_t size = p->getTypeLayout()->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM);
        kernel->globalsSize = std::max(kernel->globalsSize, off + size);
        if (name == "gPass")
            kernel->offPass = off, found[0] = true;
        else if (name == "gU")
            kernel->offU = off, found[1] = true;
        else if (name == "gArena")
            kernel->offArena = off, found[2] = true;
        else if (name == "gOut")
            kernel->offOut = off, found[3] = true;
    }
    if (!found[0] || !found[1] || !found[3])
    {
        *err = "compiled pass is missing a binding";
        return nullptr;
    }
    // A pass that reads no buffer may have had gArena optimised out; it's then never touched.
    if (!found[2])
        kernel->offArena = SIZE_MAX;
    kernel->keepAlive = std::shared_ptr<void>(lib.detach(), [](void* p) { static_cast<ISlangSharedLibrary*>(p)->release(); });
    return kernel;
}
} // namespace

bool cpuAvailable(std::string* why)
{
    std::string err;
    auto k = cpuKernel("[shader(\"compute\")][numthreads(1, 1, 1)] void texMain(uint3 id : SV_DispatchThreadID) {}\n"
                       "struct TexPassHeader { float4 res; float4 uv; };\nConstantBuffer<TexPassHeader> gPass;\nStructuredBuffer<float4> gU;\n"
                       "StructuredBuffer<float4> gArena;\nRWStructuredBuffer<float4> gOut;\n",
                       0x7e5a11ab1eull, &err);
    if (!k && why)
        *why = err;
    return k != nullptr;
}

std::shared_ptr<const CpuKernel> cpuKernel(const std::string& source, uint64_t hash, std::string* err)
{
    State& s = state();
    std::lock_guard lock(s.mutex);
    if (auto it = s.kernels.find(hash); it != s.kernels.end())
    {
        ++s.stats.hits;
        return it->second;
    }
    std::string why;
    auto k = compileCpu(source, &why);
    if (!k)
    {
        ++s.stats.failures;
        if (err)
            *err = why;
        return nullptr;
    }
    ++s.stats.compiles;
    s.kernels[hash] = k;
    return k;
}

std::vector<uint32_t> spirv(const std::string& source, std::string* err)
{
    LILA_ZONE();
    State& s = state();
    std::lock_guard lock(s.mutex);
    std::string why;
    slang::IGlobalSession* g = global(&why);
    if (!g)
    {
        if (err)
            *err = why;
        return {};
    }
    slang::CompilerOptionEntry options[2];
    options[0].name = slang::CompilerOptionName::VulkanUseEntryPointName;
    options[0].value.intValue0 = 1;
    options[1].name = slang::CompilerOptionName::MatrixLayoutColumn;
    options[1].value.intValue0 = 1;
    slang::TargetDesc target{};
    target.format = SLANG_SPIRV;
    target.profile = g->findProfile("spirv_1_6");
    target.compilerOptionEntries = options;
    target.compilerOptionEntryCount = 2;
    slang::SessionDesc desc{};
    desc.targets = &target;
    desc.targetCount = 1;
    Slang::ComPtr<slang::ISession> session;
    if (SLANG_FAILED(g->createSession(desc, session.writeRef())))
    {
        if (err)
            *err = "couldn't create a Slang session";
        return {};
    }
    Slang::ComPtr<slang::IComponentType> linked = link(session.get(), source, &why);
    if (!linked)
    {
        if (err)
            *err = why;
        return {};
    }
    Slang::ComPtr<slang::IBlob> code, diag;
    if (SLANG_FAILED(linked->getEntryPointCode(0, 0, code.writeRef(), diag.writeRef())) || !code)
    {
        if (err)
            *err = text(diag);
        return {};
    }
    std::vector<uint32_t> out(code->getBufferSize() / 4);
    std::memcpy(out.data(), code->getBufferPointer(), out.size() * 4);
    return out;
}

CacheStats cacheStats()
{
    std::lock_guard lock(state().mutex);
    return state().stats;
}

void clearCache()
{
    std::lock_guard lock(state().mutex);
    state().kernels.clear();
    state().stats = {};
}
} // namespace lila::tex::slangrt
