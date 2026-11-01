#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/**
 * A .blend read as Blender writes it: blocks of structs plus the SDNA that describes every struct's layout, so fields
 * are looked up by name and survive Blender moving them. Read-only, no Blender code.
 *
 * Pointers in the file are the addresses the structs had when saved. Data blocks are only unique within the ID they
 * follow (Blender 5 writes the same address in several IDs), so a pointer resolves in its own ID first, then among IDs.
 */
namespace lila::blend
{
namespace fs = std::filesystem;

struct Field
{
    std::string name;
    std::string type;
    uint32_t offset = 0;
    /** Bytes of one element: the pointer size for pointers. */
    uint32_t elemSize = 0;
    uint32_t count = 1;
    bool pointer = false;
};

struct LILA_SCENE_API Struct
{
    std::string name;
    uint32_t size = 0;
    std::vector<Field> fields;
    std::unordered_map<std::string, uint32_t> byName;
    /** Starts with an ID, so pointers to it resolve among IDs. */
    bool isId = false;

    const Field* field(std::string_view name) const;
};

struct Block
{
    char code[5] = {};
    uint32_t sdna = 0;
    uint64_t address = 0;
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t count = 0;
    /** Index of the ID block this one follows, or -1 before the first ID. */
    int32_t owner = -1;

    std::string_view codeView() const { return code; }
    bool isId() const { return code[2] == 0 && code[0] != 0; }
};

class File;

/** One struct in the file, or nothing. Lookups of a field the struct lacks give the fallback. */
class LILA_SCENE_API View
{
public:
    View() = default;
    View(const File* file, const Struct* type, const uint8_t* data, int32_t scope) : mFile(file), mType(type), mData(data), mScope(scope) {}

    explicit operator bool() const { return mData != nullptr && mType != nullptr; }
    const Struct* type() const { return mType; }
    const std::string& typeName() const;
    const uint8_t* data() const { return mData; }
    int32_t scope() const { return mScope; }
    const File* file() const { return mFile; }

    bool has(std::string_view field) const { return mType && mType->field(field); }
    /** Any numeric field (char, short, int, float, double, int8_t, ...) as a double. */
    double num(std::string_view field, size_t i = 0, double fallback = 0.0) const;
    int64_t integer(std::string_view field, size_t i = 0, int64_t fallback = 0) const;
    float f(std::string_view field, size_t i = 0, float fallback = 0.0f) const { return float(num(field, i, fallback)); }
    int i(std::string_view field, size_t i = 0, int fallback = 0) const { return int(integer(field, i, fallback)); }
    glm::dvec3 vec3(std::string_view field, glm::dvec3 fallback = glm::dvec3(0.0)) const;
    glm::dvec4 vec4(std::string_view field, glm::dvec4 fallback = glm::dvec4(0.0)) const;
    /** A float[4][4], Blender's column-major layout (m[col][row]). */
    glm::dmat4 mat4(std::string_view field) const;
    glm::dmat3 mat3(std::string_view field) const;
    /** A char array as a string. */
    std::string str(std::string_view field) const;
    uint64_t ptr(std::string_view field, size_t i = 0) const;

    /** An embedded struct. */
    View sub(std::string_view field) const;
    /** What a pointer field points at, typed by the field (or by its block when the field is void or ID). */
    View deref(std::string_view field, size_t i = 0) const;
    /** The block a pointer field points at. */
    const Block* block(std::string_view field, size_t i = 0) const;
    /** A char* field. */
    std::string cstr(std::string_view field) const;
    /** The struct after this one in an array: element i counting this one as 0. */
    View element(size_t i) const;
    /** The items of a ListBase field, each typed by its block or itemType. */
    std::vector<View> list(std::string_view field, std::string_view itemType = {}) const;
    /** n structs of type at a pointer field (an array of structs). */
    std::vector<View> array(std::string_view field, size_t n, std::string_view type = {}) const;
    /** n pointers in an array of pointers a field points at (Object **, Material **). */
    std::vector<View> pointerArray(std::string_view field, size_t n) const;

    /** A raw array a pointer field points at: as many T as the block holds. */
    template <class T>
    std::span<const T> raw(std::string_view field) const
    {
        const Block* b = block(field);
        return b ? std::span<const T>(reinterpret_cast<const T*>(b->data), b->size / sizeof(T)) : std::span<const T>();
    }

    /** An ID's name without its two-letter type ("OBCube" -> "Cube"). */
    std::string idName() const;
    /** An ID's two-letter type ("OB"). */
    std::string idCode() const;
    /** The ID's library (null for local IDs). */
    View library() const;

private:
    const Field* field(std::string_view name) const { return mType ? mType->field(name) : nullptr; }

    const File* mFile = nullptr;
    const Struct* mType = nullptr;
    const uint8_t* mData = nullptr;
    int32_t mScope = -1;
};

class LILA_SCENE_API File
{
public:
    /** Reads and decompresses the whole file. Throws std::runtime_error. */
    explicit File(const fs::path& path);
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    const fs::path& path() const { return mPath; }
    /** 405 for 4.5, 502 for 5.2: the version the file was saved by. */
    int version() const { return mVersion; }
    std::string versionString() const;

    const Struct* type(std::string_view name) const;
    const Struct* typeOf(const Block& b) const { return b.sdna < mStructs.size() ? &mStructs[b.sdna] : nullptr; }
    const std::vector<Block>& blocks() const { return mBlocks; }

    /** Every ID with this code ("OB", "ME", "SC"). */
    std::vector<View> ids(std::string_view code) const;
    /** The block at a saved address, looked up in scope's ID first (preferId looks among IDs first). */
    const Block* block(uint64_t address, int32_t scope, bool preferId = false) const;
    /** The struct at an address. type null types it by its block. */
    View at(uint64_t address, int32_t scope, const Struct* type = nullptr) const;
    View viewOf(const Block& b) const;

    /** A path as written in the file ("//textures/a.png" is relative to the file, or to lib's file). */
    fs::path resolvePath(std::string_view path, const View& library = {}) const;

private:
    void readHeaderAndBlocks();
    void readSdna(const Block& dna);

    fs::path mPath;
    std::vector<uint8_t> mBytes;
    int mVersion = 0;
    std::vector<Block> mBlocks;
    std::vector<Struct> mStructs; // by SDNA struct index
    std::unordered_map<std::string, uint32_t> mStructByName;
    std::unordered_map<uint64_t, uint32_t> mIdByAddress;
    std::unordered_map<uint64_t, uint32_t> mFirstByAddress;
    struct ScopedHash
    {
        size_t operator()(const std::pair<int32_t, uint64_t>& k) const { return std::hash<uint64_t>()(k.second * 31 + uint64_t(k.first)); }
    };
    std::unordered_map<std::pair<int32_t, uint64_t>, uint32_t, ScopedHash> mScoped;
};

/** True for .blend and .blend1, .blend2 ... backups (case-insensitive). */
LILA_SCENE_API bool isBlendFile(const fs::path& path);
} // namespace lila::blend
