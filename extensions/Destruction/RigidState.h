#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

/**
 * The rigid-body simulation's state bytes, as the framework caches them per frame:
 *
 *   Header | Body x bodies | Link x links | Jolt's own snapshot (joltBytes)
 *
 * The leading records are everything the node's outputs need, readable without Jolt. Positions are the bodies' shape
 * origins in metres, rotations quaternions xyzw, velocities m/s and rad/s, all as Jolt holds them, so writing them back
 * is exact. The Jolt snapshot carries the rest (contacts, warm starting, sleep) for a restore that matches a straight run.
 */
namespace destruction
{
inline constexpr uint32_t kStateMagic = 0x4252444c; // "LDRB"
inline constexpr uint32_t kStateVersion = 1;

struct StateHeader
{
    uint32_t magic = kStateMagic;
    uint32_t version = kStateVersion;
    /** Which live world produced it, and its step count, so the solver can carry on without restoring. */
    uint64_t serial = 0;
    int64_t step = 0;
    int64_t startFrame = 1;
    int32_t substeps = 1;
    uint32_t bodies = 0;
    uint32_t links = 0;
    /** 1 between the substeps of a frame: no Jolt snapshot, as only the live world carries on from it. */
    uint32_t partial = 0;
    uint64_t joltBytes = 0;
};

struct BodyRecord
{
    float position[3] = {};
    float rotation[4] = {0, 0, 0, 1};
    float velocity[3] = {};
    float angularVelocity[3] = {};
    uint8_t active = 0; // 1 once simulated; 0 while it follows its input transform
    uint8_t pad[3] = {};
};

struct LinkRecord
{
    int32_t brokenStep = -1; // the step it broke on, -1 while intact
    float impulse = 0.0f;    // N s that broke it
    uint8_t colliding = 0;   // the two pieces collide again (they separated after the break)
    uint8_t pad[3] = {};
};

/** Read-only view over state bytes; valid() is false for anything else. */
class StateView
{
public:
    explicit StateView(std::span<const uint8_t> bytes)
    {
        if (bytes.size() < sizeof(StateHeader))
            return;
        std::memcpy(&mHeader, bytes.data(), sizeof mHeader);
        const size_t need = sizeof(StateHeader) + mHeader.bodies * sizeof(BodyRecord) + mHeader.links * sizeof(LinkRecord);
        if (mHeader.magic != kStateMagic || mHeader.version != kStateVersion || bytes.size() < need + mHeader.joltBytes)
            return;
        mBytes = bytes;
        mValid = true;
    }

    bool valid() const { return mValid; }
    const StateHeader& header() const { return mHeader; }
    BodyRecord body(size_t i) const
    {
        BodyRecord r;
        std::memcpy(&r, mBytes.data() + sizeof(StateHeader) + i * sizeof(BodyRecord), sizeof r);
        return r;
    }
    LinkRecord link(size_t i) const
    {
        LinkRecord r;
        std::memcpy(&r, mBytes.data() + sizeof(StateHeader) + mHeader.bodies * sizeof(BodyRecord) + i * sizeof(LinkRecord), sizeof r);
        return r;
    }
    std::span<const uint8_t> jolt() const
    {
        const size_t at = sizeof(StateHeader) + mHeader.bodies * sizeof(BodyRecord) + mHeader.links * sizeof(LinkRecord);
        return mBytes.subspan(at, size_t(mHeader.joltBytes));
    }
    /** The frame a step belongs to: steps 1..substeps end the first frame after the start. */
    int64_t frameOfStep(int64_t step) const
    {
        const int64_t sub = std::max<int64_t>(1, mHeader.substeps);
        return mHeader.startFrame + (step + sub - 1) / sub;
    }

private:
    std::span<const uint8_t> mBytes;
    StateHeader mHeader;
    bool mValid = false;
};

/** Writes state bytes in the layout above. */
inline void writeState(std::vector<uint8_t>& out, const StateHeader& h, std::span<const BodyRecord> bodies, std::span<const LinkRecord> links,
                       std::span<const uint8_t> jolt)
{
    StateHeader header = h;
    header.bodies = uint32_t(bodies.size());
    header.links = uint32_t(links.size());
    header.joltBytes = jolt.size();
    out.resize(sizeof header + bodies.size_bytes() + links.size_bytes() + jolt.size());
    uint8_t* p = out.data();
    std::memcpy(p, &header, sizeof header);
    p += sizeof header;
    if (!bodies.empty())
        std::memcpy(p, bodies.data(), bodies.size_bytes());
    p += bodies.size_bytes();
    if (!links.empty())
        std::memcpy(p, links.data(), links.size_bytes());
    p += links.size_bytes();
    if (!jolt.empty())
        std::memcpy(p, jolt.data(), jolt.size());
}
} // namespace destruction
