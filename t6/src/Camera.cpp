#include "StdInclude.hpp"
#include "Camera.hpp"

#include "Addresses.hpp"
#include "Bones.hpp"
#include "Lights.hpp"
#include "Props.hpp"
#include "Mod.hpp"
#include "Structures.hpp"
#include "Functions.hpp"
#include "Components/CameraManager.hpp"
#include "Components/CameraShake.hpp"
#include "Components/LensFlare.hpp"
#include "Components/Lens.hpp"
#include "Components/Tracking.hpp"
#include "Functions.hpp"
#include "Components/KeyframeManager.hpp"
#include "Components/Playback.hpp"
#include "Utilities/HookManager.hpp"
#include "Utilities/MathUtils.hpp"

namespace IWXMVM::T6::Camera
{
    namespace
    {
        typedef void(__cdecl* CG_ViewFunction_t)(int localClientNum);
        CG_ViewFunction_t CG_CalcViewValues_Trampoline = nullptr;
        CG_ViewFunction_t CG_TheaterCamera_Trampoline = nullptr;
        CG_ViewFunction_t CG_TheaterFreeCamera_Trampoline = nullptr;
        CG_ViewFunction_t CG_OtherView_Trampoline = nullptr;

        std::uintptr_t GetCg()
        {
            return *reinterpret_cast<const std::uintptr_t*>(GetGameAddresses().cgViewPointer());
        }

        // The camera for this frame, taken once before the game builds its view, as T5iris does
        struct CameraState
        {
            bool isModControlled = false;
            Components::Camera::Mode mode = Components::Camera::Mode::FirstPerson;
            glm::vec3 position{};
            glm::vec3 rotation{};
            float fov = 0.0f;
        };
        CameraState frameState;
        // the theater's camera mode from before the editor flew the camera, to go back to afterwards
        std::optional<std::int32_t> savedTheaterMode;

        // The dolly camera: the campath at the timeline's tick. The keyframes are copied first, as the UI may be
        // editing them on the render thread. TODO(T6): copy them at the render thread's sync, as T5iris does.
        void ApplyDollyCamera(CameraState& state)
        {
            auto& keyframeManager = Components::KeyframeManager::Get();
            const auto& property = keyframeManager.GetProperty(Types::KeyframeablePropertyType::CampathCamera);
            const auto keyframes = keyframeManager.GetKeyframes(property);
            if (keyframes.empty())
                return;

            const auto value =
                keyframeManager.Interpolate(property, keyframes, Components::Playback::GetTimelineTick());
            state.position = value.cameraData.position;
            state.rotation = value.cameraData.rotation;
            state.fov = value.cameraData.fov;
        }

        CameraState GetCameraState()
        {
            CameraState state;
            auto& camera = Components::CameraManager::Get().GetActiveCamera();
            if (!camera)
                return state;
            state = {camera->IsModControlledCameraMode(), camera->GetMode(), camera->GetPosition(),
                     camera->GetRotation(), camera->GetFov()};
            if (camera->GetMode() == Components::Camera::Mode::Dolly)
                ApplyDollyCamera(state);
            return state;
        }

        // While the editor flies the camera, the theater's camera is its free camera, so the game draws the view as
        // from outside: the followed player's body, and no gun or first-person hud. (BO2 never reads cg_thirdPerson.)
        // It's set again whenever it isn't, as going back in the film restores a snapshot with its first-person
        // camera, and set back to what it was once the editor stops flying it.
        void UpdateTheaterCameraMode(int localClientNum)
        {
            const auto theater = *reinterpret_cast<const std::uintptr_t*>(GetGameAddresses().theater());
            if (!theater)
            {
                savedTheaterMode.reset();
                return;
            }
            const auto mode = *reinterpret_cast<const std::int32_t*>(theater + Structures::THEATER_CAMERA_MODE);

            typedef void(__cdecl * Demo_SetCameraMode_t)(int localClientNum, std::int32_t mode);
            const auto Demo_SetCameraMode = reinterpret_cast<Demo_SetCameraMode_t>(GetGameAddresses().Demo_SetCameraMode());

            // the editor's third person camera is the theater's, which follows its player from behind
            const auto wanted = frameState.isModControlled ? std::optional(Structures::THEATER_CAMERA_FREE)
                                : frameState.mode == Components::Camera::Mode::ThirdPerson
                                    ? std::optional(Structures::THEATER_CAMERA_THIRD_PERSON)
                                    : std::nullopt;
            if (wanted)
            {
                if (!savedTheaterMode)
                {
                    savedTheaterMode = mode;
                    LOG_DEBUG("T6: the editor has the camera; the theater's camera was mode {}", mode);
                }
                if (mode != *wanted)
                    Demo_SetCameraMode(localClientNum, *wanted);
                return;
            }

            if (savedTheaterMode)
            {
                if (mode != *savedTheaterMode && *savedTheaterMode >= Structures::THEATER_CAMERA_FIRST_PERSON &&
                    *savedTheaterMode <= Structures::THEATER_CAMERA_FREE)
                    Demo_SetCameraMode(localClientNum, *savedTheaterMode);
                LOG_DEBUG("T6: the game has the camera again; the theater's camera is back to mode {}", *savedTheaterMode);
                savedTheaterMode.reset();
            }
        }

        // the view moved by a camera shake: turned, and moved along its own axes (forward, right, up)
        std::pair<glm::vec3, glm::vec3> ShakeView(glm::vec3 position, glm::vec3 rotation,
                                                  const Components::CameraShake::Offset& shake)
        {
            rotation += shake.rotation;
            const auto axis = MathUtils::AnglesToAxis(rotation);
            position += axis[0] * shake.position.x - axis[1] * shake.position.y + axis[2] * shake.position.z;
            return {position, rotation};
        }

        // where the game's view was shaken this frame, so the second write of the frame doesn't shake it again
        std::optional<glm::vec3> shakenView;

        void WriteView(Structures::refdef_t& refdef, std::uintptr_t cg, glm::vec3 position, glm::vec3 rotation)
        {
            for (int i = 0; i < 3; ++i)
                refdef.vieworg[i] = position[i];

            const auto axis = MathUtils::AnglesToAxis(rotation);
            for (int x = 0; x < 3; ++x)
                for (int y = 0; y < 3; ++y)
                    refdef.viewaxis[x][y] = axis[x][y];

            auto viewAngles = reinterpret_cast<float*>(cg + Structures::CG_REFDEF_VIEW_ANGLES);
            for (int i = 0; i < 3; ++i)
                viewAngles[i] = rotation[i];
        }

        // For the lens flare, as the view is written: where the sun is on the picture (its direction from
        // r_lightTweakSunDirection's angles, towards the sky: the sun is taken to be above the horizon), and the depth
        // buffer's view ($scene_depth, the 12th render target image) for what's in front of it
        void ReportSun(const Structures::refdef_t& refdef)
        {
            if (!(Mod::GetGameInterface()->GetSupportedFeatures() & Types::Features_LensFlare))
                return;
            static ID3D11ShaderResourceView* const* depth = [] {
                const auto image = GetGameAddresses().renderTargetImages() + 11 * 0x50;
                const auto name = *reinterpret_cast<const char* const*>(image + 0x48);
                const bool found = name && !IsBadReadPtr(name, 16) && std::strcmp(name, "$scene_depth") == 0;
                return found ? reinterpret_cast<ID3D11ShaderResourceView* const*>(image) : nullptr;
            }();
            Components::LensFlare::SetDepthBuffer(depth, Functions::GetDvarNumber("r_znear").value_or(4.0f));

            const auto angles = Functions::GetDvarVec3("r_lightTweakSunDirection");
            if (!angles || refdef.tanHalfFovX <= 0.0f || refdef.tanHalfFovY <= 0.0f)
            {
                Components::LensFlare::SetSunPosition(std::nullopt);
                return;
            }
            auto toSun = MathUtils::AnglesToAxis(*angles)[0];
            if (toSun.z < 0.0f)
                toSun = -toSun;
            const auto axis = glm::make_mat3x3(&refdef.viewaxis[0][0]);
            const float forward = glm::dot(toSun, axis[0]);
            if (forward <= 0.01f)
            {
                Components::LensFlare::SetSunPosition(std::nullopt);
                return;
            }
            // (the view's axes: forward, left, up)
            const float x = -glm::dot(toSun, axis[1]) / forward / refdef.tanHalfFovX;
            const float y = glm::dot(toSun, axis[2]) / forward / refdef.tanHalfFovY;
            Components::LensFlare::SetSunPosition(glm::vec2(0.5f + 0.5f * x, 0.5f - 0.5f * y));
        }

        void WriteRefdef()
        {
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            const auto cg = GetCg();
            if (!cg)
                return;

            auto& refdef = *reinterpret_cast<Structures::refdef_t*>(cg + Structures::CG_REFDEF);
            auto& camera = Components::CameraManager::Get().GetActiveCamera();
            if (!camera)
                return;

            const auto& state = frameState;
            if (!state.isModControlled)
            {
                // the game's view: the editor's camera follows it (unless the UI switched to a flown one meanwhile)
                if (camera->IsModControlledCameraMode())
                    return;
                // (the view this frame already shaken: the theater's camera didn't write it again since)
                if (shakenView && glm::make_vec3(refdef.vieworg) == *shakenView)
                    return;
                camera->GetPosition() = glm::make_vec3(refdef.vieworg);
                camera->GetRotation() = MathUtils::AxisToAngles(glm::make_mat3x3(&refdef.viewaxis[0][0]));
                camera->GetFov() = glm::degrees(std::atan(refdef.tanHalfFovX) * 2.0f);

                // the camera shake over the game's view too
                const auto shake = Components::CameraShake::Evaluate(Components::Playback::GetTimelineTick());
                if (shake.rotation != glm::vec3(0.0f) || shake.position != glm::vec3(0.0f))
                {
                    const auto shaken = ShakeView(camera->GetPosition(), camera->GetRotation(), shake);
                    WriteView(refdef, cg, shaken.first, shaken.second);
                    shakenView = shaken.first;
                }
                return;
            }

            // the camera shake, by the film's time (Components::CameraShake)
            const auto shake = Components::CameraShake::Evaluate(Components::Playback::GetTimelineTick());
            const auto [position, rotation] = ShakeView(state.position, state.rotation, shake);
            const auto fov = state.fov;

            WriteView(refdef, cg, position, rotation);

            // the FOV across (the lens's when it sets one: its focal length's, or as wide as a fisheye needs), and the
            // height as the game had it for its own view: the picture's shape stays the game's (the refdef's width and
            // height aren't known to be what they're taken for, and gave the wrong shape)
            if (fov > 1.0f && fov < 179.0f && refdef.tanHalfFovX > 0.0f && refdef.tanHalfFovY > 0.0f)
            {
                const auto aspect = refdef.tanHalfFovY / refdef.tanHalfFovX;
                refdef.tanHalfFovX = std::tan(glm::radians(fov) * 0.5f);
                if (const auto lens = Components::Lens::RenderTanHalfX(aspect))
                    refdef.tanHalfFovX = *lens;
                refdef.tanHalfFovY = refdef.tanHalfFovX * aspect;
            }
        }

        // the editor's view written, then where the sun is in it (each write, the last one of the frame counting)
        void UpdateRefdef()
        {
            WriteRefdef();
            if (Mod::GetGameInterface()->GetGameState() != Types::GameState::InDemo)
                return;
            if (const auto cg = GetCg())
            {
                const auto& refdef = *reinterpret_cast<const Structures::refdef_t*>(cg + Structures::CG_REFDEF);
                // the view, for what screen effects follow (Components::Tracking)
                Components::Tracking::View view;
                view.position = glm::make_vec3(refdef.vieworg);
                view.axis = glm::make_mat3x3(&refdef.viewaxis[0][0]);
                view.tanHalfFovX = refdef.tanHalfFovX;
                view.tanHalfFovY = refdef.tanHalfFovY;
                Components::Tracking::SetView(view);
                ReportSun(refdef);
            }
        }

        void __cdecl CG_CalcViewValues_Hook(int localClientNum)
        {
            if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
            {
                // the bones the UI asked for, looked up here on the client's thread
                Bones::Update();
                frameState = GetCameraState();
                shakenView.reset();
                UpdateTheaterCameraMode(localClientNum);
            }
            else
            {
                frameState = {};
                savedTheaterMode.reset();
            }

            CG_CalcViewValues_Trampoline(localClientNum);
            UpdateRefdef();
            // the scene's lights, as the effects add theirs this frame
            if (Mod::GetGameInterface()->GetGameState() == Types::GameState::InDemo)
                Lights::AddToScene();
                Props::AddToScene();
        }

        // The theater's camera writes the view again after CG_CalcViewValues (in its third person, free and dolly
        // modes), so the editor's is written after it too: the last writer before the frame copies the refdef out
        // for the renderer (0x4F7E70). With the editor's camera following the game's, it follows the theater's.
        void __cdecl CG_TheaterCamera_Hook(int localClientNum)
        {
            CG_TheaterCamera_Trampoline(localClientNum);
            UpdateRefdef();
        }

        // In the free camera (which the theater is in while the editor flies the camera), the frame runs the theater's
        // free camera after that, which writes the view once more; and another view (0x6D8EF0) can too
        void __cdecl CG_TheaterFreeCamera_Hook(int localClientNum)
        {
            CG_TheaterFreeCamera_Trampoline(localClientNum);
            UpdateRefdef();
        }

        void __cdecl CG_OtherView_Hook(int localClientNum)
        {
            CG_OtherView_Trampoline(localClientNum);
            UpdateRefdef();
        }
    }  // namespace

    void Install()
    {
        HookManager::CreateHook(GetGameAddresses().CG_CalcViewValues(),
                                reinterpret_cast<std::uintptr_t>(CG_CalcViewValues_Hook),
                                reinterpret_cast<std::uintptr_t*>(&CG_CalcViewValues_Trampoline));
        HookManager::CreateHook(GetGameAddresses().CG_TheaterCamera(),
                                reinterpret_cast<std::uintptr_t>(CG_TheaterCamera_Hook),
                                reinterpret_cast<std::uintptr_t*>(&CG_TheaterCamera_Trampoline));
        HookManager::CreateHook(GetGameAddresses().CG_TheaterFreeCamera(),
                                reinterpret_cast<std::uintptr_t>(CG_TheaterFreeCamera_Hook),
                                reinterpret_cast<std::uintptr_t*>(&CG_TheaterFreeCamera_Trampoline));
        HookManager::CreateHook(GetGameAddresses().CG_OtherView(), reinterpret_cast<std::uintptr_t>(CG_OtherView_Hook),
                                reinterpret_cast<std::uintptr_t*>(&CG_OtherView_Trampoline));
        LOG_DEBUG("T6: hooked CG_CalcViewValues at {:#x}, the theater's camera at {:#x}, its free camera at {:#x} and "
                  "the other view at {:#x}",
                  GetGameAddresses().CG_CalcViewValues(), GetGameAddresses().CG_TheaterCamera(),
                  GetGameAddresses().CG_TheaterFreeCamera(), GetGameAddresses().CG_OtherView());
    }
}  // namespace IWXMVM::T6::Camera
