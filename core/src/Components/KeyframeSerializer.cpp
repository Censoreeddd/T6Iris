#include "StdInclude.hpp"
#include "KeyframeSerializer.hpp"
#include "LensFlare.hpp"
#include "Tracking.hpp"
#include "ProjectEffects.hpp"

#include "nlohmann/json.hpp"

#include "Utilities/PathUtils.hpp"
#include "KeyframeManager.hpp"
#include "Mod.hpp"
#include "Playback.hpp"
#include "Markers.hpp"
#include "CameraManager.hpp"
#include "RuntimeProperties.hpp"

namespace IWXMVM::Components
{
    constexpr std::string_view NODE_GAME_NAME = "game";
    constexpr std::string_view NODE_DEMO_NAME = "demo";
    constexpr std::string_view NODE_FROZEN_TICK = "frozenTick";
    constexpr std::string_view NODE_PROPERTIES = "properties";
    constexpr std::string_view NODE_PROPERTY = "property";
    constexpr std::string_view NODE_TICK = "tick";
    constexpr std::string_view NODE_TYPE = "type";
    constexpr std::string_view NODE_VALUES = "values";
    constexpr std::string_view NODE_VALUE = "value";
    constexpr std::string_view NODE_KEYFRAMES = "keyframes";
    constexpr std::string_view NODE_MARKERS = "markers";
    constexpr std::string_view NODE_NAME = "name";
    // added with interpolation per key (older builds skip them): a key's, when it isn't "Auto" (which is what keys
    // without one are), its eases when it has them, and the campath's constant speed when it is on
    constexpr std::string_view NODE_INTERPOLATION = "interpolation";
    constexpr std::string_view NODE_EASE_IN = "easeIn";
    constexpr std::string_view NODE_EASE_OUT = "easeOut";
    // a node's eases that follow Auto Ease, when they do (older builds ignore them)
    constexpr std::string_view NODE_AUTO_EASE_IN = "autoEaseIn";
    constexpr std::string_view NODE_AUTO_EASE_OUT = "autoEaseOut";
    constexpr std::string_view NODE_CONSTANT_SPEED = "constantSpeed";
    // a key's Bezier handles (the graph's; older builds skip them)
    constexpr std::string_view NODE_HANDLE_IN = "handleIn";
    constexpr std::string_view NODE_HANDLE_OUT = "handleOut";
    // the bone camera's settings (older builds skip them)
    constexpr std::string_view NODE_BONE_CAMERA = "boneCamera";
    // what the lens's Track focus and the lens flare follow (Types::ScreenTrack), and the flare's light
    constexpr std::string_view NODE_TRACKING = "tracking";
    // the effects' settings (Components::ProjectEffects; older builds skip them)
    constexpr std::string_view NODE_EFFECTS = "effects";
    // the effects as last written or read, so a change of them is saved too (WriteRecentIfChanged)
    std::string lastEffects;
    std::mutex effectsMutex;
    // the bullet time spans (older builds skip them, and play what is keyed after a span
    // that much early)
    constexpr std::string_view NODE_FROZEN_SPANS = "frozenSpans";
    // A property made at run time (ReShade's values) is "ReShade", with its id and what
    // it needs to be made again while ReShade doesn't have it (RuntimeProperties::Write), so its keys stay in the file
    // (older builds skip it)
    constexpr std::string_view NODE_RUNTIME_PROPERTY = "ReShade";

    BoneCamera& GetBoneCamera()
    {
        return static_cast<BoneCamera&>(*CameraManager::Get().GetCamera(Camera::Mode::Bone));
    }

    nlohmann::json WriteBoneCamera(const Types::BoneCameraSettings& settings)
    {
        return {
            {"target", settings.target.kind == Types::BoneCameraTargetKind::Actor ? "actor" : "player"},
            {"id", settings.target.id},
            {"bone", settings.bone},
            {"offset", {settings.positionOffset.x, settings.positionOffset.y, settings.positionOffset.z}},
            {"rotation", {settings.rotationOffset.x, settings.rotationOffset.y, settings.rotationOffset.z}},
            {"fov", settings.fov},
            {"steady", settings.steady},
            {"turn", magic_enum::enum_name(settings.turn)},
            {"showBone", settings.showBone},
        };
    }

    Types::BoneCameraSettings ReadBoneCamera(const nlohmann::json& node)
    {
        Types::BoneCameraSettings settings;
        settings.target.kind = node.value("target", std::string()) == "actor" ? Types::BoneCameraTargetKind::Actor
                                                                             : Types::BoneCameraTargetKind::Player;
        settings.target.id = node.value("id", 0);
        settings.bone = node.value("bone", settings.bone);
        const auto vector = [&](const char* name) {
            const auto values = node.value(name, std::vector<float>{});
            return values.size() == 3 ? glm::vec3(values[0], values[1], values[2]) : glm::vec3(0.0f);
        };
        settings.positionOffset = vector("offset");
        settings.rotationOffset = vector("rotation");
        settings.fov = std::clamp(node.value("fov", settings.fov), 1.0f, 160.0f);
        settings.steady = std::clamp(node.value("steady", 0.0f), 0.0f, 0.5f);
        settings.turn = magic_enum::enum_cast<Types::BoneCameraTurn>(node.value("turn", std::string()))
                            .value_or(Types::BoneCameraTurn::Bone);
        settings.showBone = node.value("showBone", true);
        return settings;
    }

    void KeyframeSerializer::Write(std::filesystem::path path)
    {
        using json = nlohmann::json;

        if (!std::filesystem::exists(path.parent_path()))
        {
            std::filesystem::create_directories(path.parent_path());
        }

        json rootNode;
        rootNode[NODE_GAME_NAME] = magic_enum::enum_name(Mod::GetGameInterface()->GetGame());
        rootNode[NODE_DEMO_NAME] = Mod::GetGameInterface()->GetDemoInfo().name;
        if (Components::Playback::IsGameFrozen())
        {
            rootNode[NODE_FROZEN_TICK] = Components::Playback::GetFrozenTick().value();
        }
        
        json properties = json::array();

        for (auto& [p, ks] : KeyframeManager::Get().GetKeyframes())
        {
            // Properties without keys are left out, but the campath, which says whether it goes at constant speed:
            // older builds stop reading at a property they don't know, and a file without keys of the properties
            // added since stays readable for them
            if (ks.empty() && p.type != Types::KeyframeablePropertyType::CampathCamera)
                continue;

            json propertyObject;
            if (const auto runtime = RuntimeProperties::Find(p.type))
            {
                RuntimeProperties::Write(*runtime, propertyObject);
                propertyObject[NODE_PROPERTY] = NODE_RUNTIME_PROPERTY;
            }
            else
            {
                propertyObject[NODE_PROPERTY] = magic_enum::enum_name(p.type);
            }
            if (p.type == Types::KeyframeablePropertyType::CampathCamera && KeyframeManager::Get().IsConstantSpeed())
                propertyObject[NODE_CONSTANT_SPEED] = true;

            json keyframeList = json::array();
            for (auto& k : ks)
            {
                json keyframeObject;
                keyframeObject[NODE_TICK] = k.tick;

                json keyframeValueObject;
                keyframeValueObject[NODE_TYPE] = magic_enum::enum_name(p.valueType);

                json keyframeValuesArray;
                keyframeValuesArray = json::array();
                for (int i = 0; i < p.GetValueCount(); i++)
                {
                    keyframeValuesArray.push_back(k.value.GetByIndex(i));
                }
                keyframeValueObject[NODE_VALUES] = keyframeValuesArray;

                keyframeObject[NODE_VALUE] = keyframeValueObject;
                if (k.interpolation != Types::KeyInterpolation::Auto)
                    keyframeObject[NODE_INTERPOLATION] = magic_enum::enum_name(k.interpolation);
                if (k.easeIn > 0.0f)
                    keyframeObject[NODE_EASE_IN] = k.easeIn;
                if (k.easeOut > 0.0f)
                    keyframeObject[NODE_EASE_OUT] = k.easeOut;
                if (k.autoEaseIn)
                    keyframeObject[NODE_AUTO_EASE_IN] = true;
                if (k.autoEaseOut)
                    keyframeObject[NODE_AUTO_EASE_OUT] = true;
                // its Bezier handles, when it has them: how far along (influence) and each value's slope
                const auto writeHandle = [&](std::string_view node, const Types::Keyframe::Handle& handle) {
                    if (!handle.set)
                        return;
                    keyframeObject[node] = {{"influence", handle.influence},
                                            {"slope", std::vector<float>(handle.slope.begin(),
                                                                         handle.slope.begin() + std::clamp(p.GetValueCount(), 1, 7))}};
                };
                writeHandle(NODE_HANDLE_IN, k.handleIn);
                writeHandle(NODE_HANDLE_OUT, k.handleOut);
                keyframeList.push_back(keyframeObject);
            }

            propertyObject[NODE_KEYFRAMES] = keyframeList;
            properties.push_back(propertyObject);
        }

        rootNode[NODE_PROPERTIES] = properties;

        // the timeline's markers (older builds read the file without them)
        json markers = json::array();
        for (const auto& marker : Markers::GetAll())
            markers.push_back({{NODE_TICK, marker.tick}, {NODE_NAME, marker.name}});
        rootNode[NODE_MARKERS] = markers;

        rootNode[NODE_BONE_CAMERA] = WriteBoneCamera(GetBoneCamera().GetSettings());
        {
            const auto flare = LensFlare::GetSettings();
            rootNode[NODE_TRACKING] = {{"focus", Tracking::ToJson(Tracking::GetFocusTrack())},
                                       {"flare", Tracking::ToJson(flare.track)},
                                       {"flareLight", magic_enum::enum_name(flare.source)}};
        }

        {
            const auto effects = ProjectEffects::Write();
            rootNode[NODE_EFFECTS] = effects;
            std::lock_guard lock(effectsMutex);
            lastEffects = effects.dump();
        }

        json spans = json::array();
        for (const auto& span : *Components::Playback::GetFrozenSpans())
            spans.push_back({{NODE_TICK, span.worldTick}, {"length", span.length}, {"actorsMove", span.actorsMove}});
        if (!spans.empty())
            rootNode[NODE_FROZEN_SPANS] = spans;

        // written next to it first, so a crash while writing doesn't leave half a file
        auto temporary = path;
        temporary += ".tmp";
        {
            std::ofstream keyframeFile(temporary);
            keyframeFile << rootNode.dump(4);
            if (!keyframeFile)
            {
                LOG_ERROR("Failed to write keyframes to {}", temporary.string());
                return;
            }
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error)
            LOG_ERROR("Failed to write keyframes to {}: {}", path.string(), error.message());
    }

    Types::KeyframeValue ReadValueOfType(const Types::KeyframeValueType valueType, const nlohmann::json& values)
    {
        switch (valueType)
        {
            case Types::KeyframeValueType::FloatingPoint:
                return Types::KeyframeValue(values[0].get<float>());
            case Types::KeyframeValueType::Vector3:
                return Types::KeyframeValue(
                    glm::vec3(
                        values[0].get<float>(),
                        values[1].get<float>(), 
                        values[2].get<float>()
                    )
                );
            case Types::KeyframeValueType::CameraData:
                auto cameraData = Types::CameraData();
                cameraData.position = glm::vec3(values[0].get<float>(), values[1].get<float>(), values[2].get<float>());
                cameraData.rotation = glm::vec3(values[3].get<float>(), values[4].get<float>(), values[5].get<float>());
                cameraData.fov = values[6].get<float>();
                return Types::KeyframeValue(cameraData);
            default:
                LOG_ERROR("Value type {} is unhandled", magic_enum::enum_name(valueType));
                throw std::runtime_error("Encountered unhandled value type");
        }
    }

    // A run-time property as the file has it: the one of its id if ReShade has it now, else made from what the file
    // says, so that its keys are kept (and show as of an effect that isn't loaded)
    std::optional<Types::KeyframeablePropertyType> ReadRuntimeProperty(const nlohmann::json& node)
    {
        const auto description = RuntimeProperties::Read(node);
        if (!description)
            return std::nullopt;
        const auto type = RuntimeProperties::Register(*description);
        KeyframeManager::Get().AddProperty(type);
        return type;
    }

    void KeyframeSerializer::Read(std::filesystem::path path, bool requireDemoMatch)
    {
        using json = nlohmann::json;

        std::ifstream file(path);
        if (!file.is_open())
        {
            LOG_ERROR("Failed to read keyframe file at {}", path.string());
            return;
        }

        try
        {
            json rootNode = json::parse(file);

            auto gameName = rootNode[NODE_GAME_NAME].get<std::string>();
            auto currentGameName = magic_enum::enum_name(Mod::GetGameInterface()->GetGame());
            if (gameName.compare(currentGameName) != 0)
            {
                LOG_WARN("Game of loaded keyframes file doesnt match current game!");
                LOG_WARN("Expected: {0}", gameName);
                LOG_WARN("Actual: {0}", currentGameName);
            }

            auto demoName = rootNode[NODE_DEMO_NAME].get<std::string>();
            auto currentDemoName = Mod::GetGameInterface()->GetDemoInfo().name;
            if (demoName.compare(currentDemoName) != 0)
            {
                if (requireDemoMatch)
                {
                    LOG_INFO("Not loading keyframes since this demo is not the previous demo");
                    return;
                }

                LOG_WARN("Demo names dont match {0} vs {1}", demoName, currentDemoName);
                LOG_WARN("Expected: {0}", demoName);
                LOG_WARN("Actual: {0}", currentDemoName);
            }

            // (before the frozen tick, which is on the timeline they make)
            if (rootNode.contains(NODE_FROZEN_SPANS))
            {
                std::vector<Types::FrozenSpan> spans;
                for (const auto& span : rootNode[NODE_FROZEN_SPANS])
                {
                    const Types::FrozenSpan read{.worldTick = span.value(NODE_TICK, 0u),
                                                 .length = span.value("length", 0u),
                                                 .actorsMove = span.value("actorsMove", false)};
                    // (sorted, one at a tick, none empty)
                    if (read.length == 0 ||
                        std::ranges::find(spans, read.worldTick, &Types::FrozenSpan::worldTick) != spans.end())
                        continue;
                    spans.push_back(read);
                }
                std::ranges::sort(spans, {}, &Types::FrozenSpan::worldTick);
                Components::Playback::SetFrozenSpans(std::move(spans));
            }

            auto optFrozenTick = rootNode[NODE_FROZEN_TICK];
            Components::Playback::HandleImportedFrozenTickLogic(
                !optFrozenTick.is_null() ? std::optional{optFrozenTick.get<std::uint32_t>()} : std::nullopt);

            auto properties = rootNode[NODE_PROPERTIES];
            for (json::iterator propertyObject = properties.begin(); propertyObject != properties.end();
                 ++propertyObject)
            {
                auto propertyName = (*propertyObject)[NODE_PROPERTY].get<std::string>();

                auto propertyType = propertyName == NODE_RUNTIME_PROPERTY
                                        ? ReadRuntimeProperty(*propertyObject)
                                        : magic_enum::enum_cast<Types::KeyframeablePropertyType>(propertyName);
                if (!propertyType.has_value())
                {
                    // (one a later build animates: the others are read)
                    LOG_WARN("Skipping the keys of an unknown property \"{0}\"", propertyName);
                    continue;
                }
                // (the manager's own: each key read keeps a reference to it, KeyframeLogic::IsPath reads it)
                const auto& property = Components::KeyframeManager::Get().GetProperty(propertyType.value());
                if (property.type == Types::KeyframeablePropertyType::CampathCamera)
                    Components::KeyframeManager::Get().LoadConstantSpeed(
                        propertyObject->value(NODE_CONSTANT_SPEED, false));

                auto& keyframes = Components::KeyframeManager::Get().GetKeyframes(property);
                for (json::iterator keyframe = propertyObject->at(NODE_KEYFRAMES).begin();
                     keyframe != propertyObject->at(NODE_KEYFRAMES).end(); ++keyframe)
                {
                    auto tick = keyframe->at(NODE_TICK).get<uint32_t>();
                    auto valueNode = keyframe->at(NODE_VALUE);
                    auto valueTypeName = valueNode[NODE_TYPE].get<std::string>();
                    auto valueType = magic_enum::enum_cast<Types::KeyframeValueType>(valueTypeName);
                    if (!valueType.has_value())
                    {
                        LOG_ERROR("Unknown value type \"{0}\"", valueTypeName);
                        throw std::runtime_error("Unknown value type encountered");
                    }
                    // (a ReShade value whose effect has another type of value now)
                    if (valueType.value() != property.valueType)
                        continue;
                    Types::KeyframeValue value = ReadValueOfType(valueType.value(), valueNode[NODE_VALUES]);

                    Types::Keyframe read(property, tick, Types::KeyframeValue(value));
                    // (one a later build adds is read as Auto)
                    read.interpolation =
                        magic_enum::enum_cast<Types::KeyInterpolation>(keyframe->value(NODE_INTERPOLATION, "Auto"))
                            .value_or(Types::KeyInterpolation::Auto);
                    read.easeIn = std::clamp(keyframe->value(NODE_EASE_IN, 0.0f), 0.0f, 1.0f);
                    read.easeOut = std::clamp(keyframe->value(NODE_EASE_OUT, 0.0f), 0.0f, 1.0f);
                    read.autoEaseIn = keyframe->value(NODE_AUTO_EASE_IN, false);
                    read.autoEaseOut = keyframe->value(NODE_AUTO_EASE_OUT, false);
                    const auto readHandle = [&](std::string_view node, Types::Keyframe::Handle& handle) {
                        if (!keyframe->contains(node) || !(*keyframe)[node].is_object())
                            return;
                        const auto& h = (*keyframe)[node];
                        handle.set = true;
                        handle.influence = std::clamp(h.value("influence", 1.0f / 3.0f), 0.01f, 1.0f);
                        if (h.contains("slope") && h["slope"].is_array())
                            for (std::size_t i = 0; i < h["slope"].size() && i < handle.slope.size(); ++i)
                                handle.slope[i] = h["slope"][i].is_number() ? h["slope"][i].get<float>() : 0.0f;
                    };
                    readHandle(NODE_HANDLE_IN, read.handleIn);
                    readHandle(NODE_HANDLE_OUT, read.handleOut);
                    keyframes.push_back(KeyframeLogic::Normalize(read, property.type));
                }
            }

            if (rootNode.contains(NODE_MARKERS))
            {
                std::vector<Markers::Marker> markers;
                for (const auto& marker : rootNode[NODE_MARKERS])
                    markers.push_back({.tick = marker.at(NODE_TICK).get<std::uint32_t>(),
                                       .name = marker.value(NODE_NAME, std::string())});
                Markers::Append(std::move(markers));
            }

            // (a demo's own file without them leaves the defaults a demo starts with, see BoneCamera::Initialize)
            if (rootNode.contains(NODE_BONE_CAMERA))
                GetBoneCamera().SetSettings(ReadBoneCamera(rootNode[NODE_BONE_CAMERA]));
            // the effects' settings (a file from before leaves them as they are)
            if (rootNode.contains(NODE_EFFECTS))
            {
                ProjectEffects::Read(rootNode[NODE_EFFECTS]);
                std::lock_guard lock(effectsMutex);
                lastEffects = rootNode[NODE_EFFECTS].dump();
            }
            // what's followed: the file's, or nothing for one from before (the flare's light kept then)
            {
                const auto tracking = rootNode.value(NODE_TRACKING, nlohmann::json::object());
                Tracking::SetFocusTrack(Tracking::FromJson(tracking.value("focus", nlohmann::json())));
                auto flare = LensFlare::GetSettings();
                flare.track = Tracking::FromJson(tracking.value("flare", nlohmann::json()));
                // (a spot of the map only)
                if (flare.track.kind != Types::TrackKind::Point)
                    flare.track = {};
                if (const auto light =
                        magic_enum::enum_cast<LensFlare::Source>(tracking.value("flareLight", std::string())))
                    flare.source = *light;
                LensFlare::SetSettings(flare);
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Failed to parse keyframe file ({})", e.what());
        }
    }

    std::filesystem::path GetRecentKeyframesPath()
    {
        return PathUtils::GetModPath() / "keyframes";
    }

    auto GetDemoNameHash()
    {
        auto demoName = Mod::GetGameInterface()->GetDemoInfo().name;
        return std::hash<std::string>{}(demoName);
    }

    void KeyframeSerializer::WriteRecent()
    {
        Write(GetRecentKeyframesPath() / std::format("{:X}.json", GetDemoNameHash()));
    }

    // when the keyframes last changed without being saved since
    std::mutex recentMutex;
    std::optional<std::chrono::steady_clock::time_point> recentChangedAt;

    void KeyframeSerializer::WriteRecentSoon()
    {
        std::lock_guard lock(recentMutex);
        recentChangedAt = std::chrono::steady_clock::now();
    }

    void KeyframeSerializer::WriteRecentIfChanged(bool now)
    {
        // the effects changed since they were last written or read (looked at each second): saved as keys are
        if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
        {
            static auto lookedAt = std::chrono::steady_clock::now();
            if (now || std::chrono::steady_clock::now() - lookedAt >= std::chrono::seconds(1))
            {
                lookedAt = std::chrono::steady_clock::now();
                const auto effects = ProjectEffects::Write().dump();
                bool changed = false;
                {
                    std::lock_guard lock(effectsMutex);
                    if (lastEffects.empty())
                        lastEffects = effects;  // (what a demo starts with isn't a change)
                    else
                        changed = effects != lastEffects;
                }
                if (changed)
                    WriteRecentSoon();
            }
        }
        {
            std::lock_guard lock(recentMutex);
            if (!recentChangedAt ||
                (!now && std::chrono::steady_clock::now() - *recentChangedAt < std::chrono::milliseconds(500)))
                return;
            recentChangedAt.reset();
        }
        // (the demo's name says where they go)
        if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
            WriteRecent();
    }

    void KeyframeSerializer::ReadRecent()
    {
        {
            std::lock_guard lock(effectsMutex);
            lastEffects.clear();
        }
        auto path = GetRecentKeyframesPath() / std::format("{:X}.json", GetDemoNameHash());
        if (std::filesystem::exists(path))
        {
            LOG_INFO("Reading last sessions keyframes for this demo...");
            Read(path, true);
        }
    }
}