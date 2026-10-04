#pragma once

#include "fast/oot3d/graphics_settings.h"
#include "fast/oot3d/native_presentation_policy.h"
#include "fast/oot3d/presentation_settings_transaction.h"
#include "fast/renderer3ds/pica_environment_lighting.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <nlohmann/json_fwd.hpp>

namespace Fast::Oot3d {

class GraphicsSettingsPersistencePort {
  public:
    virtual ~GraphicsSettingsPersistencePort() = default;
    [[nodiscard]] virtual bool LoadRoot(nlohmann::json& root) = 0;
    [[nodiscard]] virtual bool StoreGraphics(
        const nlohmann::json& graphics) = 0;
};

// Install before the first call to GraphicsSettingsRuntime::Instance().
// The renderer remains usable without a persistence port.
void InstallGraphicsSettingsPersistencePort(
    std::shared_ptr<GraphicsSettingsPersistencePort> persistence);

struct VersionedGraphicsSettings {
    GraphicsSettings Value;
    uint64_t Revision = 0;
    Renderer3ds::PicaEnvironmentLighting Environment;
};

// Observed renderer state, never persisted as a user request.
struct GraphicsDisplayMetrics {
    uint32_t OutputWidth = 0, OutputHeight = 0;
    uint32_t SceneWidth = 0, SceneHeight = 0;
    float InternalScale = 1.0F;
    WindowMode Window = WindowMode::Windowed;
};

// Observed state of the global illumination provider, never persisted: a
// selected mode whose provider is not ready yet (pipelines created off the
// frame) composes no GI and says why.
struct GlobalIlluminationProviderStatus {
    GlobalIlluminationMode Mode = GlobalIlluminationMode::Off;
    bool Ready = false;
    std::string Reason;
    bool operator==(const GlobalIlluminationProviderStatus&) const = default;
};

enum class GraphicsSettingsSaveState {
    Saved, Pending, WaitingForDisplay, SessionOnly, Failed
};

class GraphicsSettingsRuntime final {
  public:
    static GraphicsSettingsRuntime& Instance();
    [[nodiscard]] GraphicsSettings Snapshot() const;
    [[nodiscard]] VersionedGraphicsSettings SnapshotWithRevision() const;
    // Rendering consumes the session override; editors/persistence consume Snapshot().
    [[nodiscard]] VersionedGraphicsSettings SnapshotForRendering() const;
    [[nodiscard]] bool NativePresentationOverrideActive() const;
    [[nodiscard]] bool NativePresentationOverrideRequired() const;
    void ToggleNativePresentationOverride();
    [[nodiscard]] GraphicsCapabilities Capabilities() const;
    [[nodiscard]] PresentationTransactionStatus PresentationStatus() const;
    [[nodiscard]] GraphicsDisplayMetrics DisplayMetrics() const;
    void PublishDisplayMetrics(GraphicsDisplayMetrics metrics);
    void PublishSceneExtent(uint32_t width, uint32_t height);
    [[nodiscard]] GlobalIlluminationProviderStatus GlobalIlluminationStatus() const;
    void PublishGlobalIlluminationStatus(GlobalIlluminationProviderStatus status);
    // Resolved title-owned profile; never serialized into global graphics settings.
    void PublishSceneEnvironment(const Renderer3ds::PicaEnvironmentLighting& environment);
    GraphicsSettingsValidation Apply(GraphicsSettings candidate, bool persist = true);
    [[nodiscard]] GraphicsSettingsSaveState SaveState() const;
    void SavePending();
    bool RetrySave();
    bool AcknowledgePresentationApplied(
        const GraphicsSettings& applied);
    bool RejectPresentationApply(const GraphicsSettings& rejected, std::string reason = {});
    [[nodiscard]] std::string LastPresentationRejection() const;
    bool ConfirmPresentation();
    void PresentationConfirmationVisible();
    bool RollbackPresentation();
    bool TickPresentation();
    void SetCapability(GraphicsCapability capability, bool available, std::string reason = {});
  private:
    GraphicsSettingsRuntime();
    bool RestoreLastKnownPresentationLocked();
    bool PersistCurrentLocked();

    mutable std::mutex mMutex;
    GraphicsSettingsService mService;
    GraphicsCapabilities mCapabilities;
    GraphicsDisplayMetrics mDisplayMetrics;
    GlobalIlluminationProviderStatus mGlobalIlluminationStatus;
    Renderer3ds::PicaEnvironmentLighting mSceneEnvironment;
    PresentationSettingsTransaction mPresentationTransaction;
    std::string mLastPresentationRejection;
    std::shared_ptr<GraphicsSettingsPersistencePort> mPersistence;
    uint64_t mRevision = 1;
    NativePresentationPolicy mNativePresentation;
    bool mPersistenceSuppressed = false;
    GraphicsSettingsSaveState mSaveState = GraphicsSettingsSaveState::Saved;
};
} // namespace Fast::Oot3d
