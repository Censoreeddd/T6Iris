// Writes the animations of BO2 zones (.ff) to an Iris Studio animation pack (iris/t6-animations.bin), which T6iris
// rebuilds in memory for actors to play: the campaign's, which the multiplayer game can't load from their zones.
//
// Built against OpenAssetTools (https://github.com/Laupetin/OpenAssetTools, GPL-3.0), whose zone loading reads the
// zones: see build.sh.
//
// Usage: t6xanimpack out.bin zone.ff [zone.ff...]   (an animation in more than one zone is written once)
//
// Format (little endian), "IRISXA01", then the number of animations (u32), then each:
//   string name (u16 length, bytes)
//   u16 dataByteCount, dataShortCount, dataIntCount, randomDataByteCount, randomDataIntCount, numframes
//   u8 loop, delta, delta3D, leftHandGripIK; u32 streamedFileSize; u8 boneCount[10], notifyCount, assetType,
//   isDefault; u32 randomDataShortCount, indexCount; f32 framerate, frequency, primedLength, loopEntryTime
//   boneCount[9] strings (the bones' names); notifyCount (always 0: no notetracks) of (string name, f32 time)
//   u8 dataByte[], i16 dataShort[], i32 dataInt[], i16 randomDataShort[], u8 randomDataByte[], i32 randomDataInt[]
//   indexCount indices (u8 each below 256 frames, else u16)
//   u8 hasDelta, then if so: the translation, the 2D and the 3D rotation, each u8 present, then u16 size, and
//   for the translation u8 smallTrans; size 0: frame0 (3 f32 / 2 i16 / 4 i16); else (translation: f32 mins[3],
//   size[3]) size + 1 indices (u8 / u16 as above), size + 1 frames (translation: 3 u8 if smallTrans else 3 u16;
//   rotations: 2 or 4 i16)
#include "Game/T6/T6.h"
#include "Zone/Zone.h"
#include "ZoneLoading.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <string_view>
#include <string>
#include <vector>

using namespace T6;

namespace
{
    std::vector<std::uint8_t> out;

    template<typename T> void Put(const T& value)
    {
        const auto bytes = reinterpret_cast<const std::uint8_t*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }

    void PutString(const std::string& text)
    {
        Put(static_cast<std::uint16_t>(text.size()));
        out.insert(out.end(), text.begin(), text.end());
    }

    template<typename T> void PutArray(const T* data, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
            Put(data ? data[i] : T{});
    }

    void PutIndices(const void* indices, std::size_t count, bool bytes)
    {
        if (bytes)
            PutArray(static_cast<const std::uint8_t*>(indices), count);
        else
            PutArray(static_cast<const std::uint16_t*>(indices), count);
    }

    void PutDelta(const XAnimParts& parts)
    {
        const auto delta = parts.deltaPart;
        Put(static_cast<std::uint8_t>(delta != nullptr));
        if (!delta)
            return;
        const bool byteIndices = parts.numframes < 256;

        Put(static_cast<std::uint8_t>(delta->trans != nullptr));
        if (const auto trans = delta->trans)
        {
            Put(trans->size);
            Put(trans->smallTrans);
            if (trans->size == 0)
                PutArray(reinterpret_cast<const float*>(&trans->u.frame0), 3);
            else
            {
                PutArray(reinterpret_cast<const float*>(&trans->u.frames.mins), 3);
                PutArray(reinterpret_cast<const float*>(&trans->u.frames.size), 3);
                PutIndices(trans->u.frames.indices._1, trans->size + 1u, byteIndices);
                if (trans->smallTrans)
                    PutArray(reinterpret_cast<const std::uint8_t*>(trans->u.frames.frames._1), (trans->size + 1u) * 3u);
                else
                    PutArray(reinterpret_cast<const std::uint16_t*>(trans->u.frames.frames._2), (trans->size + 1u) * 3u);
            }
        }
        Put(static_cast<std::uint8_t>(delta->quat2 != nullptr));
        if (const auto quat = delta->quat2)
        {
            Put(quat->size);
            if (quat->size == 0)
                PutArray(quat->u.frame0.value, 2);
            else
            {
                PutIndices(quat->u.frames.indices._1, quat->size + 1u, byteIndices);
                PutArray(reinterpret_cast<const std::int16_t*>(quat->u.frames.frames), (quat->size + 1u) * 2u);
            }
        }
        Put(static_cast<std::uint8_t>(delta->quat != nullptr));
        if (const auto quat = delta->quat)
        {
            Put(quat->size);
            if (quat->size == 0)
                PutArray(quat->u.frame0.value, 4);
            else
            {
                PutIndices(quat->u.frames.indices._1, quat->size + 1u, byteIndices);
                PutArray(reinterpret_cast<const std::int16_t*>(quat->u.frames.frames), (quat->size + 1u) * 4u);
            }
        }
    }
    // The campaign's people (their j_spinelower a child of j_mainroot) and multiplayer's (a child of torso_stabilizer,
    // which turns back j_mainroot's rest rotation: 16179 16586 16586 16178) differ at the spine: a campaign animation's
    // j_spinelower, which multiplayer's torso_stabilizer doesn't animate, is turned into torso_stabilizer's space,
    // j_mainroot's rest rotation times its own (as poses compose: parent * local), and so is its translation.
    struct Quat
    {
        double x, y, z, w;
    };
    Quat Multiply(const Quat& a, const Quat& b)
    {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }
    Quat Normalized(Quat q)
    {
        const auto length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return length > 0 ? Quat{q.x / length, q.y / length, q.z / length, q.w / length} : Quat{0, 0, 0, 1};
    }
    // j_mainroot's rest rotation (torso_stabilizer's turned back)
    const Quat MAINROOT_REST = Normalized({-16179, -16586, -16586, 16178});

    void TurnQuat(std::int16_t* value, std::size_t components)
    {
        Quat q = components == 2 ? Quat{0, 0, value[0] / 32767.0, value[1] / 32767.0}
                                 : Quat{value[0] / 32767.0, value[1] / 32767.0, value[2] / 32767.0, value[3] / 32767.0};
        auto turned = Normalized(Multiply(MAINROOT_REST, Normalized(q)));
        const double v[4] = {turned.x, turned.y, turned.z, turned.w};
        for (int i = 0; i < 4; ++i)
            value[i] = static_cast<std::int16_t>(std::lround(std::clamp(v[i], -1.0, 1.0) * 32767.0));
    }

    std::array<double, 3> Rotate(const std::array<double, 3>& v)
    {
        const Quat p{v[0], v[1], v[2], 0};
        const Quat conj{-MAINROOT_REST.x, -MAINROOT_REST.y, -MAINROOT_REST.z, MAINROOT_REST.w};
        const auto r = Multiply(Multiply(MAINROOT_REST, p), conj);
        return {r.x, r.y, r.z};
    }

    // Returns whether it did; the flat data walked as the game reads it (see OpenAssetTools' FlatXAnimReader)
    bool RetargetSpine(XAnimParts& parts, const std::vector<std::string>& bones)
    {
        const auto spine = std::find(bones.begin(), bones.end(), "j_spinelower");
        if (spine == bones.end() || std::find(bones.begin(), bones.end(), "torso_stabilizer") != bones.end())
            return false;
        const auto spineIndex = static_cast<std::size_t>(spine - bones.begin());
        const bool byteIndices = parts.numframes < 256;
        std::size_t b = 0, s = 0, in = 0, rs = 0, rb = 0;
        const auto skipIndices = [&](std::size_t stored) {
            const auto count = stored + 1;
            if (byteIndices)
                b += count;
            else if (stored >= 64)
                s += (count - 2) / 256 + 2;
            else
                s += count;
        };
        std::size_t bone = parts.boneCount[0];
        bool turned = false;
        for (const int type : {1, 2})
            for (unsigned i = 0; i < parts.boneCount[type]; ++i, ++bone)
            {
                const auto stored = static_cast<std::uint16_t>(parts.dataShort[s++]);
                skipIndices(stored);
                const std::size_t components = type == 1 ? 2 : 4;
                if (bone == spineIndex && components == 4)
                {
                    for (std::size_t k = 0; k <= stored; ++k)
                        TurnQuat(parts.randomDataShort + rs + k * 4, 4);
                    turned = true;
                }
                rs += (stored + 1u) * components;
            }
        for (const int type : {3, 4})
            for (unsigned i = 0; i < parts.boneCount[type]; ++i, ++bone)
            {
                const std::size_t components = type == 3 ? 2 : 4;
                if (bone == spineIndex && components == 4)
                {
                    TurnQuat(parts.dataShort + s, 4);
                    turned = true;
                }
                s += components;
            }
        if (!turned)
            return false;
        // its translation, if it has one
        for (const int type : {5, 6})
            for (unsigned i = 0; i < parts.boneCount[type]; ++i)
            {
                const auto animBone = parts.dataByte[b++];
                const auto stored = static_cast<std::uint16_t>(parts.dataShort[s++]);
                auto* minsAndSize = reinterpret_cast<float*>(parts.dataInt + in);
                in += 6;
                skipIndices(stored);
                const std::size_t count = stored + 1u;
                if (animBone == spineIndex)
                {
                    // (each frame turned, then quantized again in the box they make)
                    std::vector<std::array<double, 3>> frames(count);
                    const double steps = type == 5 ? 255.0 : 65535.0;
                    for (std::size_t k = 0; k < count; ++k)
                        for (int c = 0; c < 3; ++c)
                        {
                            const double q = type == 5 ? parts.randomDataByte[rb + k * 3 + c]
                                                       : static_cast<std::uint16_t>(parts.randomDataShort[rs + k * 3 + c]);
                            frames[k][c] = minsAndSize[c] + minsAndSize[3 + c] * q;
                        }
                    std::array<double, 3> low{1e30, 1e30, 1e30}, high{-1e30, -1e30, -1e30};
                    for (auto& frame : frames)
                    {
                        frame = Rotate(frame);
                        for (int c = 0; c < 3; ++c)
                        {
                            low[c] = std::min(low[c], frame[c]);
                            high[c] = std::max(high[c], frame[c]);
                        }
                    }
                    for (int c = 0; c < 3; ++c)
                    {
                        minsAndSize[c] = static_cast<float>(low[c]);
                        minsAndSize[3 + c] = static_cast<float>((high[c] - low[c]) / steps);
                    }
                    for (std::size_t k = 0; k < count; ++k)
                        for (int c = 0; c < 3; ++c)
                        {
                            const double range = high[c] - low[c];
                            const auto q = range > 0 ? std::lround((frames[k][c] - low[c]) / range * steps) : 0L;
                            if (type == 5)
                                parts.randomDataByte[rb + k * 3 + c] = static_cast<std::uint8_t>(q);
                            else
                                parts.randomDataShort[rs + k * 3 + c] = static_cast<std::int16_t>(static_cast<std::uint16_t>(q));
                        }
                }
                if (type == 5)
                    rb += count * 3;
                else
                    rs += count * 3;
            }
        for (unsigned i = 0; i < parts.boneCount[7]; ++i)
        {
            const auto animBone = parts.dataByte[b++];
            auto* constant = reinterpret_cast<float*>(parts.dataInt + in);
            in += 3;
            if (animBone == spineIndex)
            {
                const auto rotated = Rotate({constant[0], constant[1], constant[2]});
                for (int c = 0; c < 3; ++c)
                    constant[c] = static_cast<float>(rotated[c]);
            }
        }
        return true;
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr << "usage: t6xanimpack out.bin zone.ff [zone.ff...]\n";
        return 1;
    }
    std::set<std::string> written;
    std::uint32_t count = 0, streamed = 0, retargeted = 0;
    out.insert(out.end(), {'I', 'R', 'I', 'S', 'X', 'A', '0', '1'});
    Put(count);
    std::vector<std::unique_ptr<Zone>> zones;
    for (int arg = 2; arg < argc; ++arg)
    {
        auto zone = ZoneLoading::LoadZone(argv[arg], std::nullopt);
        if (!zone)
        {
            std::cerr << argv[arg] << ": " << zone.error() << "\n";
            return 1;
        }
        std::uint32_t fromZone = 0;
        for (const auto* info : (*zone)->m_pools.PoolAssets<AssetXAnim>())
        {
            const auto& parts = *info->Asset();
            if (!parts.name || parts.name[0] == ',' || !written.insert(parts.name).second)
                continue;
            // (only those a person plays on their own: not the additive layers, asset types 1 and 6, which the game
            // only adds to others (aims, fire), nor the first person's arms' or the player's camera's)
            const std::string_view name(parts.name);
            if (parts.assetType == 1 || parts.assetType == 6 || name.starts_with("viewmodel_") ||
                name.starts_with("vm_") || name.starts_with("player_") || name.starts_with("pf_") ||
                name.starts_with("fp_") || name.find("face") != std::string_view::npos ||
                name.find("firstperson") != std::string_view::npos)
                continue;
            // (streamed from the disk, without its data in the zone)
            if (!parts.dataShort && !parts.randomDataShort && !parts.dataByte && parts.streamedFileSize)
            {
                ++streamed;
                continue;
            }
            {
                std::vector<std::string> bones;
                for (unsigned i = 0; parts.names && i < parts.boneCount[PART_TYPE_ALL]; ++i)
                    bones.emplace_back((*zone)->m_script_strings.Value(parts.names[i]));
                if (RetargetSpine(const_cast<XAnimParts&>(parts), bones))
                    ++retargeted;
            }
            PutString(parts.name);
            Put(parts.dataByteCount);
            Put(parts.dataShortCount);
            Put(parts.dataIntCount);
            Put(parts.randomDataByteCount);
            Put(parts.randomDataIntCount);
            Put(parts.numframes);
            Put(static_cast<std::uint8_t>(parts.bLoop));
            Put(static_cast<std::uint8_t>(parts.bDelta));
            Put(static_cast<std::uint8_t>(parts.bDelta3D));
            Put(static_cast<std::uint8_t>(parts.bLeftHandGripIK));
            Put(parts.streamedFileSize);
            PutArray(parts.boneCount, PART_TYPE_COUNT);
            Put(static_cast<unsigned char>(0));  // (no notetracks: actors don't use them)
            Put(parts.assetType);
            Put(static_cast<std::uint8_t>(parts.isDefault));
            Put(parts.randomDataShortCount);
            Put(parts.indexCount);
            Put(parts.framerate);
            Put(parts.frequency);
            Put(parts.primedLength);
            Put(parts.loopEntryTime);
            const auto& strings = (*zone)->m_script_strings;
            for (unsigned i = 0; i < parts.boneCount[PART_TYPE_ALL]; ++i)
                PutString(parts.names ? std::string(strings.Value(parts.names[i])) : std::string());
            PutArray(parts.dataByte, parts.dataByteCount);
            PutArray(parts.dataShort, parts.dataShortCount);
            PutArray(parts.dataInt, parts.dataIntCount);
            PutArray(parts.randomDataShort, parts.randomDataShortCount);
            PutArray(parts.randomDataByte, parts.randomDataByteCount);
            PutArray(parts.randomDataInt, parts.randomDataIntCount);
            PutIndices(parts.indices.data, parts.indexCount, parts.numframes < 256);
            PutDelta(parts);
            ++count;
            ++fromZone;
        }
        std::cerr << argv[arg] << ": " << fromZone << " animations\n";
        zones.push_back(std::move(*zone));
    }
    std::memcpy(out.data() + 8, &count, sizeof(count));
    std::ofstream(argv[1], std::ios::binary).write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    std::cerr << count << " animations written to " << argv[1] << " (" << out.size() << " bytes), " << streamed
              << " streamed ones left out, " << retargeted << " turned to multiplayer's spine\n";
    return 0;
}
