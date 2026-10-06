#include "fast/oot3d/graphics_settings_window.h"
#include "fast/oot3d/graphics_settings_runtime.h"
#include "fast/oot3d/settings_panel_widgets.h"
#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <string_view>
#include <utility>

namespace Fast::Oot3d {
namespace {
std::mutex& PanelTabMutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::shared_ptr<GraphicsSettingsPanelTab>>& PanelTabs() {
    static std::vector<std::shared_ptr<GraphicsSettingsPanelTab>> tabs;
    return tabs;
}

std::vector<std::shared_ptr<GraphicsSettingsPanelTab>> SnapshotPanelTabs() {
    std::scoped_lock lock(PanelTabMutex());
    return PanelTabs();
}
} // namespace
void InstallGraphicsSettingsPanelTabs(
    std::vector<std::shared_ptr<GraphicsSettingsPanelTab>> tabs) {
    std::erase(tabs, nullptr);
    std::scoped_lock lock(PanelTabMutex());
    PanelTabs() = std::move(tabs);
}

void InstallGraphicsSettingsPanelTab(
    std::shared_ptr<GraphicsSettingsPanelTab> tab) {
    std::vector<std::shared_ptr<GraphicsSettingsPanelTab>> tabs;
    if (tab != nullptr) {
        tabs.push_back(std::move(tab));
    }
    InstallGraphicsSettingsPanelTabs(std::move(tabs));
}

namespace {
struct HostNotice {
    std::string Text;
    bool Failure = false;
    std::chrono::steady_clock::time_point Until{};
};
std::mutex& HostNoticeMutex() {
    static std::mutex mutex;
    return mutex;
}
HostNotice& CurrentHostNotice() {
    static HostNotice notice;
    return notice;
}
} // namespace

void PostHostNotice(std::string text, bool failure) {
    std::scoped_lock lock(HostNoticeMutex());
    auto& notice = CurrentHostNotice();
    notice.Text = std::move(text);
    notice.Failure = failure;
    // Failures stay longer: they point the player at F1.
    notice.Until = std::chrono::steady_clock::now() +
                   std::chrono::seconds(failure ? 8 : 3);
}

bool HostNoticeActive() {
    std::scoped_lock lock(HostNoticeMutex());
    return std::chrono::steady_clock::now() < CurrentHostNotice().Until;
}

void DrawHostNotice() {
    HostNotice notice;
    {
        std::scoped_lock lock(HostNoticeMutex());
        notice = CurrentHostNotice();
    }
    if (std::chrono::steady_clock::now() >= notice.Until) return;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 16.0F, viewport->WorkPos.y + 16.0F),
                            ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75F);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##TriAevumHostNotice", nullptr, flags)) {
        if (notice.Failure) {
            ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.4F, 1.0F), "%s", notice.Text.c_str());
        } else {
            ImGui::TextUnformatted(notice.Text.c_str());
        }
    }
    ImGui::End();
}

void DrawDisplayConfirmation() {
    auto& runtime = GraphicsSettingsRuntime::Instance();
    const bool awaiting = runtime.PresentationStatus().Phase == PresentationTransactionPhase::AwaitingConfirmation;
    constexpr const char* title = "Keep display settings?";
    if (awaiting && !ImGui::IsPopupOpen(title)) ImGui::OpenPopup(title);
    if (!awaiting && !ImGui::IsPopupOpen(title)) return;
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
        ImVec2(std::max(240.0F, viewport->WorkSize.x - 16.0F), viewport->WorkSize.y - 16.0F));
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
        if (!awaiting) {
            ImGui::CloseCurrentPopup();
        } else {
            runtime.PresentationConfirmationVisible();
            const auto metrics = runtime.DisplayMetrics();
            if (metrics.OutputWidth)
                ImGui::Text("Output: %u x %u", metrics.OutputWidth, metrics.OutputHeight);
            ImGui::Text("Reverting in %u seconds", (runtime.PresentationStatus().RemainingMilliseconds + 999U) / 1000U);
            if (ImGui::Button("Keep display settings")) {
                runtime.ConfirmPresentation();
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::Button("Revert display settings")) {
                runtime.RollbackPresentation();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}
void GraphicsSettingsPanel::DrawPresentationStatus() {
    auto& runtime = GraphicsSettingsRuntime::Instance();
    runtime.TickPresentation();
    if (!ImGui::IsAnyItemActive()) runtime.SavePending();
    const auto status = runtime.PresentationStatus();
    const auto rejection = runtime.LastPresentationRejection();
    if (!rejection.empty()) {
        ImGui::TextWrapped("Display change reverted: %s", rejection.c_str());
    }
    if (status.Phase == PresentationTransactionPhase::AwaitingConfirmation) {
        ImGui::TextUnformatted("Waiting for display confirmation...");
    } else if (status.Phase == PresentationTransactionPhase::ApplyRequested) {
        ImGui::TextUnformatted("Applying display settings...");
    } else if (status.Phase == PresentationTransactionPhase::RollbackRequested) {
        ImGui::TextUnformatted("Restoring previous display settings...");
    }
    switch (runtime.SaveState()) {
        case GraphicsSettingsSaveState::Failed:
            ImGui::TextWrapped("Settings applied, but could not be saved.");
            if (ImGui::Button("Retry saving graphics")) runtime.RetrySave();
            break;
        case GraphicsSettingsSaveState::SessionOnly:
            ImGui::TextDisabled("Graphics: session only");
            break;
        case GraphicsSettingsSaveState::Pending:
            ImGui::TextDisabled("Graphics: unsaved");
            break;
        case GraphicsSettingsSaveState::Saved:
            ImGui::TextDisabled("Graphics: saved");
            break;
        case GraphicsSettingsSaveState::WaitingForDisplay:
            break;
    }
}

void GraphicsSettingsPanel::Draw() {
    const bool nativeRequired = GraphicsSettingsRuntime::Instance().NativePresentationOverrideRequired();
    if (nativeRequired) {
        ImGui::TextWrapped("Native presentation: Grass, Toon/outline, AO and reflections are unavailable in this build.");
    } else {
        ImGui::TextWrapped("F2: quick native presentation - Grass, Toon/outline, AO and reflections off.");
    }
    const bool nativeOverride = GraphicsSettingsRuntime::Instance().NativePresentationOverrideActive();
    ImGui::PushStyleColor(ImGuiCol_Text, nativeOverride ? ImVec4(1.0F, 0.78F, 0.25F, 1.0F)
                                                      : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(nativeRequired ? "Vulkan/NRI active. Configured effect values are preserved." : nativeOverride
        ? "F2 override ACTIVE. Configured effects are suspended; press F2 to restore."
        : "F2 override inactive. Using configured effects.");
    ImGui::PopStyleColor();
    ImGui::Separator();
    DrawPresentationStatus();
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.48F);
    DrawPresetSelector();
    // Application tabs: the Lighting group joins the built-in Lighting tab;
    // every other group (or ungrouped tab) is a top-level tab, in the order
    // it was first installed.
    std::vector<std::shared_ptr<GraphicsSettingsPanelTab>> lightingTabs;
    std::vector<std::pair<std::string, std::vector<std::shared_ptr<GraphicsSettingsPanelTab>>>> groups;
    for (const auto& tab : SnapshotPanelTabs()) {
        const char* group = tab->Group();
        if (group != nullptr && std::string_view(group) == kLightingSettingsGroup) {
            lightingTabs.push_back(tab);
            continue;
        }
        const std::string name = group != nullptr ? group : tab->Label();
        auto existing = std::find_if(groups.begin(), groups.end(),
                                     [&](const auto& entry) { return entry.first == name; });
        if (existing == groups.end()) {
            groups.emplace_back(name, std::vector<std::shared_ptr<GraphicsSettingsPanelTab>>{});
            existing = std::prev(groups.end());
        }
        existing->second.push_back(tab);
    }
    const auto drawApplicationTab = [](const std::shared_ptr<GraphicsSettingsPanelTab>& tab) {
        ImGui::PushID(tab->Label());
        tab->Draw();
        ImGui::PopID();
    };
    if (ImGui::BeginTabBar("##Oot3dSettingsTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Display")) {
            DrawSettingsPage("##DisplayContent", {&GraphicsSettingsPanel::DrawDisplaySettings,
                                                  &GraphicsSettingsPanel::DrawAntialiasingSettings,
                                                  &GraphicsSettingsPanel::DrawMotionSettings});
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Lighting")) {
            DrawLightingTab(lightingTabs);
            ImGui::EndTabItem();
        }
        if (!nativeRequired && ImGui::BeginTabItem("Grass")) {
            DrawGrassSettings();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Materials")) {
            DrawMaterialsTab();
            ImGui::EndTabItem();
        }
        for (const auto& [name, tabs] : groups) {
            if (!ImGui::BeginTabItem(name.c_str())) continue;
            if (tabs.size() == 1U && tabs.front()->Group() == nullptr) {
                drawApplicationTab(tabs.front());
            } else if (ImGui::BeginTabBar(("##" + name + "Sections").c_str(), ImGuiTabBarFlags_FittingPolicyScroll)) {
                for (const auto& tab : tabs) {
                    if (ImGui::BeginTabItem(tab->Label())) {
                        drawApplicationTab(tab);
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::PopItemWidth();
}

void GraphicsSettingsPanel::DrawGrassSettings() {
    auto& runtime = GraphicsSettingsRuntime::Instance();
    auto settings = runtime.Snapshot();
    if (DrawGrassSettingsPanel(settings.Grass, settings.GrassSavedPreset, mGrassPanelState)) {
        settings.Preset = GraphicsPreset::Custom;
        mGrassPanelState.Status = SettingsUi::DescribeApply(
            runtime.Apply(settings, !ImGui::IsAnyItemActive()));
    }
}

void GraphicsSettingsPanel::DrawTextureSettings() {
    auto& runtime = GraphicsSettingsRuntime::Instance();
    auto settings = runtime.Snapshot();
    if (DrawAzaharTexturePackPanel(settings.TexturePacks.Azahar, mTexturePackPanelState)) {
        settings.Preset = GraphicsPreset::Custom;
        mTexturePackPanelState.Status = SettingsUi::DescribeApply(runtime.Apply(settings));
    }
}

// The graphics preset covers every graphics tab, so it sits above them.
void GraphicsSettingsPanel::DrawPresetSelector() {
    auto& runtime = GraphicsSettingsRuntime::Instance();
    auto settings = runtime.Snapshot();
    const char* const presets[] = {"Authentic", "Enhanced", "Toon", "Custom"};
    GraphicsPreset preset = settings.Preset;
    if (SettingsUi::EnumCombo("Graphics preset", preset, presets)) {
        settings = GraphicsSettingsService::PresetForCurrent(preset, settings);
        mRendererStatus = SettingsUi::DescribeApply(runtime.Apply(settings));
        mRenderScaleEditing = false;
        mOutputResolutionEditing = false;
    }
    if (!mRendererStatus.empty() && ImGui::TreeNode("Last change")) {
        ImGui::TextWrapped("%s", mRendererStatus.c_str());
        ImGui::TreePop();
    }
}

void GraphicsSettingsPanel::DrawSettingsPage(const char* id, std::initializer_list<Section> sections) {
    auto& runtime = GraphicsSettingsRuntime::Instance();
    auto settings = runtime.Snapshot();
    const auto capabilities = runtime.Capabilities();
    bool changed = false;
    ImGui::BeginChild(id);
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.48F);
    for (const auto section : sections) changed |= (this->*section)(settings, capabilities);
    ImGui::PopItemWidth();
    ImGui::EndChild();
    if (changed) {
        settings.Preset = GraphicsPreset::Custom;
        mRendererStatus = SettingsUi::DescribeApply(
            runtime.Apply(settings, !ImGui::IsAnyItemActive()));
    }
}

void GraphicsSettingsPanel::DrawLightingTab(
    const std::vector<std::shared_ptr<GraphicsSettingsPanelTab>>& lightingTabs) {
    if (!ImGui::BeginTabBar("##LightingSections", ImGuiTabBarFlags_FittingPolicyScroll)) return;
    const auto section = [&](const char* label, Section draw) {
        if (ImGui::BeginTabItem(label)) {
            DrawSettingsPage(label, {draw});
            ImGui::EndTabItem();
        }
    };
    section("Lighting & GI", &GraphicsSettingsPanel::DrawLightingSettings);
    section("Shadows", &GraphicsSettingsPanel::DrawShadowSettings);
    section("Occlusion", &GraphicsSettingsPanel::DrawAmbientOcclusionSettings);
    section("Reflections", &GraphicsSettingsPanel::DrawReflectionSettings);
    // Application-owned lighting sections (the per-scene environment).
    for (const auto& tab : lightingTabs) {
        if (ImGui::BeginTabItem(tab->Label())) {
            ImGui::BeginChild(tab->Label());
            ImGui::PushID(tab->Label());
            tab->Draw();
            ImGui::PopID();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
    }
    section("Toon", &GraphicsSettingsPanel::DrawToonSettings);
    ImGui::EndTabBar();
}

void GraphicsSettingsPanel::DrawMaterialsTab() {
    if (!ImGui::BeginTabBar("##MaterialSections", ImGuiTabBarFlags_FittingPolicyScroll)) return;
    if (ImGui::BeginTabItem("Texture packs")) {
        ImGui::BeginChild("##TextureContent");
        DrawTextureSettings();
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Material editor")) {
        ImGui::BeginChild("##MaterialEditorContent");
        DrawPbrMaterialEditor(mMaterialEditorState);
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Reflection materials")) {
        DrawSettingsPage("##ReflectionMaterialContent", {&GraphicsSettingsPanel::DrawReflectionMaterialSettings});
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}
} // namespace Fast::Oot3d
