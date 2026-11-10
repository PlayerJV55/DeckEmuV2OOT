// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <initializer_list>
#include <map>
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QGroupBox>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include "common/logging.h"
#include "common/settings.h"
#include "qt_common/config/shared_translation.h"
#include "qt_common/config/uisettings.h"
#include "yuzu/configuration/settings_layout.h"
#include "yuzu/configuration/shared_widget.h"

namespace ConfigurationShared {

namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("ConfigurationShared", text);
}

// Dynamic properties of the group boxes AddSettingGroups makes
constexpr char GroupProperty[] = "settings_group";
constexpr char AdvancedProperty[] = "settings_group_advanced";
constexpr char ChangedProperty[] = "settings_group_changed";

bool IsChanged(const Settings::BasicSetting& setting) {
    if (!Settings::IsConfiguringGlobal()) {
        return !setting.UsingGlobal();
    }
    return setting.ToString() != setting.DefaultToString();
}

bool IsChanged(const SettingGroup& group) {
    for (const Settings::BasicSetting* const setting : group.settings) {
        // The checkbox in front of a value is a setting of its own
        const Settings::BasicSetting* const paired = setting->PairedSetting();
        if (IsChanged(*setting) ||
            (paired != nullptr && paired->Specialization() == Settings::Specialization::Paired &&
             IsChanged(*paired))) {
            return true;
        }
    }
    return false;
}

} // Anonymous namespace

namespace Layout {

std::vector<SettingGroup> General() {
    auto& ui = UISettings::values;
    return {
        {tr("Behavior"),
         {&ui.confirm_before_stopping, &ui.pause_when_in_background, &ui.hide_mouse,
          &ui.select_user_on_boot}},
        {tr("Updates"), {&ui.check_for_updates}},
        {tr("Linux"),
         {&ui.enable_gamemode,
#ifdef __unix__
          &ui.gui_force_x11
#endif
         }},
    };
}

std::vector<SettingGroup> System() {
    auto& v = Settings::values;
    return {
        {tr("Console"), {&v.use_docked_mode, &v.device_name, &v.memory_layout_mode}},
        {tr("Language and Region"), {&v.language_index, &v.region_index, &v.time_zone_index}},
        {tr("Date and Time"), {&v.custom_rtc, &v.custom_rtc_offset}},
        {.title = tr("Advanced"), .settings = {&v.rng_seed}, .advanced = true},
    };
}

std::vector<SettingGroup> Applets() {
    auto& v = Settings::values;
    return {
        {tr("Overlay"), {&v.enable_overlay}},
        {tr("System Applets"),
         {&v.cabinet_applet_mode, &v.controller_applet_mode, &v.error_applet_mode,
          &v.player_select_applet_mode, &v.swkbd_applet_mode, &v.mii_edit_applet_mode,
          &v.web_applet_mode, &v.photo_viewer_applet_mode, &v.offline_web_applet_mode}},
        // Untested applets
        {.settings = {&v.data_erase_applet_mode, &v.net_connect_applet_mode, &v.shop_applet_mode,
                      &v.login_share_applet_mode, &v.wifi_web_auth_applet_mode,
                      &v.my_page_applet_mode},
         .hidden = true},
    };
}

std::vector<SettingGroup> Emulation() {
    auto& v = Settings::values;
    return {
        {tr("Speed"),
         {&v.speed_limit, &v.turbo_speed_limit, &v.slow_speed_limit, &v.sync_core_speed}},
        {tr("CPU"), {&v.cpu_accuracy, &v.use_multi_core}},
        {tr("Time Seen by Games"), {&v.cpu_clock, &v.gpu_clock}},
        {.title = tr("Advanced"), .settings = {&v.cpu_backend, &v.cpu_ticks}, .advanced = true},
        {.title = tr("Unsafe CPU Optimizations"),
         .settings = {&v.cpuopt_unsafe_host_mmu, &v.cpuopt_unsafe_unfuse_fma,
                      &v.cpuopt_unsafe_reduce_fp_error, &v.cpuopt_unsafe_ignore_standard_fpcr,
                      &v.cpuopt_unsafe_inaccurate_nan, &v.cpuopt_unsafe_fastmem_check,
                      &v.cpuopt_unsafe_ignore_global_monitor},
         .advanced = true},
    };
}

std::vector<SettingGroup> Graphics() {
    auto& v = Settings::values;
    return {
        {tr("API"), {&v.renderer_backend, &v.vulkan_device}},
        {tr("Image"),
         {&v.resolution_setup, &v.scaling_filter, &v.fsr_sharpening_slider, &v.anti_aliasing,
          &v.max_anisotropy}},
        // bg_red stands for the background color row, which sets all three
        {tr("Display"),
         {&v.fullscreen_mode, &v.aspect_ratio, &v.bg_red, &v.vsync_mode, &v.frame_pacing_mode}},
        {tr("Post-Processing"), {&v.post_shader_enabled}},
    };
}

std::vector<SettingGroup> FrameGeneration() {
    auto& v = Settings::values;
    return {
        {tr("Frame Generation"),
         {&v.frame_gen, &v.frame_gen_type, &v.frame_gen_multiplier, &v.frame_gen_target_rate}},
        {.title = tr("Tuning"),
         .settings = {&v.frame_gen_back_off, &v.frame_gen_flow_scale_auto, &v.frame_gen_flow_scale,
                      &v.frame_gen_queue_target},
         .advanced = true},
    };
}

std::vector<SettingGroup> GraphicsAdvanced() {
    auto& v = Settings::values;
    return {
        {tr("Accuracy"), {&v.gpu_accuracy}},
        {.title = tr("Synchronization"),
         .settings = {&v.dma_accuracy, &v.gpu_fence_behavior, &v.sync_memory_operations,
                      &v.use_reactive_flushing},
         .advanced = true},
        {tr("Shaders and Pipelines"),
         {&v.use_disk_shader_cache, &v.pipeline_preload, &v.use_asynchronous_shaders}},
        {.title = tr("Pipeline Internals"),
         .settings = {&v.use_vulkan_driver_pipeline_cache, &v.enable_compute_pipelines},
         .advanced = true},
        {tr("Textures and Memory"), {&v.astc_recompression, &v.vram_usage_mode}},
        {.title = tr("Texture Decoding"), .settings = {&v.accelerate_astc}, .advanced = true},
        {tr("Video"), {&v.nvdec_emulation, &v.use_video_framerate}},
        {.title = tr("Presentation"),
         .settings = {&v.renderer_force_max_clock, &v.async_presentation,
                      &v.enable_buffer_history},
         .advanced = true},
    };
}

std::vector<SettingGroup> GraphicsFixes() {
    auto& v = Settings::values;
    return {
        {.title = tr("Vulkan Extensions"),
         .settings = {&v.dyna_state, &v.vertex_input_dynamic_state, &v.sample_shading},
         .advanced = true},
        {.title = tr("Game Fixes"),
         .settings = {&v.barrier_feedback_loops, &v.fix_bloom_effects,
                      &v.skip_cpu_inner_invalidation},
         .advanced = true},
        {.title = tr("GPU Unswizzle"),
         .settings = {&v.gpu_unswizzle_enabled, &v.gpu_unswizzle_texture_size,
                      &v.gpu_unswizzle_stream_size, &v.gpu_unswizzle_chunk_size},
         .advanced = true},
    };
}

std::vector<SettingGroup> Switch2() {
    auto& v = Settings::values;
    return {
        {tr("DLSS"), {&v.game_dlss_network, &v.game_dlss_sharpness, &v.nvidia_dlss}},
        {tr("Rendering"), {&v.nx2_mesh_shaders}},
        {tr("Mario Kart World"), {&v.mkw_frame_rate}},
    };
}

std::vector<SettingGroup> Audio() {
    auto& v = Settings::values;
    return {
        {tr("Output"),
         {&v.sink_id, &v.audio_output_device_id, &v.volume, &v.audio_muted,
          &UISettings::values.mute_when_in_background}},
        // Nothing in this build reads them
        {.settings = {&v.audio_input_device_id, &v.sound_index}, .hidden = true},
    };
}

} // namespace Layout

Widget* BuildSettingWidget(const Builder& builder, Settings::BasicSetting* setting,
                           std::vector<std::function<void(bool)>>& apply_funcs) {
    Widget* const widget = builder.BuildWidget(setting, apply_funcs);
    if (widget == nullptr) {
        return nullptr;
    }
    if (!widget->Valid()) {
        widget->deleteLater();
        return nullptr;
    }
    return widget;
}

QWidget* SettingInput(const Widget& widget) {
    if (widget.combobox != nullptr) {
        return widget.combobox;
    }
    if (widget.spinbox != nullptr) {
        return widget.spinbox;
    }
    if (widget.slider != nullptr) {
        return widget.slider;
    }
    return widget.checkbox;
}

void EnableWhileChecked(QCheckBox* parent, const std::vector<QWidget*>& inputs) {
    const auto update = [parent, inputs] {
        for (QWidget* const input : inputs) {
            input->setEnabled(parent->isChecked());
        }
    };
    QObject::connect(parent, &QCheckBox::toggled, parent, update);
    update();
}

std::vector<QGroupBox*> AddSettingGroups(
    QBoxLayout& layout, const std::vector<SettingGroup>& groups,
    const std::function<QWidget*(Settings::BasicSetting*)>& build) {
    std::vector<QGroupBox*> boxes;
    boxes.reserve(groups.size());
    for (const SettingGroup& group : groups) {
        auto* const box = new QGroupBox(group.title, layout.parentWidget());
        auto* const box_layout = new QVBoxLayout(box);
        for (Settings::BasicSetting* const setting : group.settings) {
            if (QWidget* const widget = build(setting); widget != nullptr) {
                box_layout->addWidget(widget);
            }
        }
        if (box_layout->isEmpty()) {
            delete box;
            boxes.push_back(nullptr);
            continue;
        }
        layout.addWidget(box);
        box->setVisible(!group.hidden);
        if (!group.hidden) {
            box->setProperty(GroupProperty, true);
            box->setProperty(ChangedProperty, IsChanged(group));
            box->setProperty(AdvancedProperty, group.advanced);
        }
        boxes.push_back(box);
    }
    return boxes;
}

void MarkAdvancedGroup(QGroupBox& box, bool changed) {
    box.setProperty(GroupProperty, true);
    box.setProperty(AdvancedProperty, true);
    box.setProperty(ChangedProperty, changed);
}

void ShowAdvancedGroups(QWidget& page, bool show) {
    for (QGroupBox* const box : page.findChildren<QGroupBox*>()) {
        if (box->property(AdvancedProperty).toBool()) {
            box->setVisible(show || box->property(ChangedProperty).toBool());
        }
    }
}

std::vector<QPushButton*> OverrideRestoreButtons(const QWidget& page) {
    std::vector<QPushButton*> buttons;
    for (QPushButton* const button : page.findChildren<QPushButton*>()) {
        // Named by Widget::CreateRestoreGlobalButton, which hides them while the global value is
        // used
        if (!button->objectName().startsWith(QStringLiteral("RestoreButton")) ||
            button->isHidden()) {
            continue;
        }
        bool shown = true;
        for (const QWidget* parent = button->parentWidget(); parent != nullptr && parent != &page;
             parent = parent->parentWidget()) {
            if (parent->isHidden() && !parent->property(AdvancedProperty).toBool()) {
                shown = false;
                break;
            }
        }
        if (shown) {
            buttons.push_back(button);
        }
    }
    return buttons;
}

bool IsEmptyAdvancedPage(const QWidget& page) {
    bool has_groups = false;
    for (const QGroupBox* const box : page.findChildren<QGroupBox*>()) {
        if (!box->property(GroupProperty).toBool()) {
            continue;
        }
        if (!box->property(AdvancedProperty).toBool() || !box->isHidden()) {
            return false;
        }
        has_groups = true;
    }
    return has_groups;
}

bool LayoutCoversAllSettings() {
    std::map<u32, int> placed;
    for (const auto& table :
         {Layout::General(), Layout::System(), Layout::Applets(), Layout::Emulation(),
          Layout::Graphics(), Layout::FrameGeneration(), Layout::GraphicsAdvanced(),
          Layout::GraphicsFixes(), Layout::Switch2(), Layout::Audio()}) {
        for (const SettingGroup& group : table) {
            for (const Settings::BasicSetting* const setting : group.settings) {
                ++placed[setting->Id()];
            }
        }
    }

    const auto translations = InitializeTranslations(nullptr);
    bool covered = true;
    const auto check = [&](const Settings::BasicSetting& setting) {
        if (setting.Specialization() == Settings::Specialization::Paired ||
            setting.GetLabel() == "emulate_bgr565") {
            return;
        }
        // A blank name hides a setting
        const auto translation = translations->find(setting.Id());
        if (translation != translations->end() && translation->second.first.isEmpty()) {
            return;
        }
        const auto it = placed.find(setting.Id());
        const int count = it == placed.end() ? 0 : it->second;
        if (count != 1) {
            LOG_ERROR(Frontend, "Setting \"{}\" is placed {} times in the settings layout",
                      setting.GetLabel(), count);
            covered = false;
        }
    };

    using Settings::Category;
    for (const Category category :
         {Category::Renderer, Category::RendererAdvanced, Category::RendererHacks,
          Category::RendererExtensions, Category::Cpu, Category::CpuUnsafe, Category::Core,
          Category::System, Category::SystemAudio, Category::Audio, Category::LibraryApplet}) {
        for (const Settings::BasicSetting* const setting :
             Settings::values.linkage.by_category[category]) {
            check(*setting);
        }
    }
    for (const Category category : {Category::UiGeneral, Category::UiAudio}) {
        for (const Settings::BasicSetting* const setting :
             UISettings::values.linkage.by_category[category]) {
            check(*setting);
        }
    }
    return covered;
}

} // namespace ConfigurationShared
