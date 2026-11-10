// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "qt_common/config/uisettings.h"
#include "ui_configure_debug.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_debug.h"
#include "yuzu/debugger/console.h"

ConfigureDebug::ConfigureDebug(const Core::System& system_, QWidget* parent)
    : QScrollArea(parent), ui{std::make_unique<Ui::ConfigureDebug>()}, system{system_} {
    ui->setupUi(this);
    SetConfiguration();

    connect(ui->open_log_button, &QPushButton::clicked, []() {
        const auto path =
            QString::fromStdString(Common::FS::GetEdenPathString(Common::FS::EdenPath::LogDir));
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });

    connect(ui->toggle_gdbstub, &QCheckBox::toggled,
            [&]() { ui->gdbport_spinbox->setEnabled(ui->toggle_gdbstub->isChecked()); });

    SetupToolTips();
}

ConfigureDebug::~ConfigureDebug() = default;

void ConfigureDebug::SetConfiguration() {
    const bool runtime_lock = !system.IsPoweredOn();
    ui->toggle_gdbstub->setChecked(Settings::values.use_gdbstub.GetValue());
    ui->gdbport_spinbox->setEnabled(Settings::values.use_gdbstub.GetValue());
    ui->gdbport_spinbox->setValue(Settings::values.gdbstub_port.GetValue());
    ui->log_filter_edit->setText(QString::fromStdString(Settings::values.log_filter.GetValue()));
    ui->flush_line->setChecked(Settings::values.log_flush_line.GetValue());
    ui->censor_username->setChecked(Settings::values.censor_username.GetValue());
    ui->reporting_services->setChecked(Settings::values.reporting_services.GetValue());
    ui->dump_audio_commands->setChecked(Settings::values.dump_audio_commands.GetValue());
    ui->quest_flag->setChecked(Settings::values.quest_flag.GetValue());
    ui->use_dev_keys->setChecked(Settings::values.use_dev_keys.GetValue());
    ui->use_debug_asserts->setChecked(Settings::values.use_debug_asserts.GetValue());
    ui->use_auto_stub->setChecked(Settings::values.use_auto_stub.GetValue());
    ui->enable_all_controllers->setChecked(Settings::values.enable_all_controllers.GetValue());
    ui->extended_logging->setChecked(Settings::values.extended_logging.GetValue());
    ui->perform_vulkan_check->setChecked(Settings::values.perform_vulkan_check.GetValue());
    ui->serial_battery_edit->setText(
        QString::number(Settings::values.serial_battery.GetValue())
    );
    ui->serial_board_edit->setText(
        QString::number(Settings::values.serial_unit.GetValue())
    );

    // Immutable after starting
    ui->homebrew_args_edit->setEnabled(runtime_lock);
    ui->homebrew_args_edit->setText(
        QString::fromStdString(Settings::values.program_args.GetValue()));
    ui->toggle_console->setEnabled(runtime_lock);
    ui->toggle_console->setChecked(UISettings::values.show_console.GetValue());
    ui->fs_access_log->setEnabled(runtime_lock);
    ui->fs_access_log->setChecked(Settings::values.enable_fs_access_log.GetValue());
    ui->enable_renderdoc_hotkey->setEnabled(runtime_lock);
    ui->enable_renderdoc_hotkey->setChecked(Settings::values.enable_renderdoc_hotkey.GetValue());
    ui->disable_buffer_reorder->setEnabled(runtime_lock);
    ui->disable_buffer_reorder->setChecked(Settings::values.disable_buffer_reorder.GetValue());
    ui->enable_graphics_debugging->setEnabled(runtime_lock);
    ui->enable_graphics_debugging->setChecked(Settings::values.renderer_debug.GetValue());
    ui->enable_shader_feedback->setEnabled(runtime_lock);
    ui->enable_shader_feedback->setChecked(Settings::values.renderer_shader_feedback.GetValue());
    ui->enable_nsight_aftermath->setEnabled(runtime_lock);
    ui->enable_nsight_aftermath->setChecked(Settings::values.enable_nsight_aftermath.GetValue());
    ui->dump_guest_shaders->setEnabled(runtime_lock);
    ui->dump_guest_shaders->setChecked(Settings::values.dump_guest_shaders.GetValue());
    ui->dump_macros->setEnabled(runtime_lock);
    ui->dump_macros->setChecked(Settings::values.dump_macros.GetValue());
    ui->disable_macro_jit->setEnabled(runtime_lock);
    ui->disable_macro_jit->setChecked(Settings::values.disable_macro_jit.GetValue());
    ui->disable_macro_hle->setEnabled(runtime_lock);
    ui->disable_macro_hle->setChecked(Settings::values.disable_macro_hle.GetValue());
    ui->disable_loop_safety_checks->setEnabled(runtime_lock);
    ui->disable_loop_safety_checks->setChecked(
        Settings::values.disable_shader_loop_safety_checks.GetValue());
    ui->perform_vulkan_check->setChecked(Settings::values.perform_vulkan_check.GetValue());
    ui->debug_knobs_spinbox->setValue(Settings::values.debug_knobs.GetValue());

    ui->gpu_log_level->setEnabled(runtime_lock);
    ui->gpu_log_level->setCurrentIndex(
        static_cast<int>(Settings::values.gpu_log_level.GetValue()));
    ui->gpu_log_shader_dumps->setEnabled(runtime_lock);
    ui->gpu_log_shader_dumps->setChecked(Settings::values.gpu_log_shader_dumps.GetValue());
}

void ConfigureDebug::ApplyConfiguration() {
    Settings::values.use_gdbstub = ui->toggle_gdbstub->isChecked();
    Settings::values.gdbstub_port = ui->gdbport_spinbox->value();
    UISettings::values.show_console = ui->toggle_console->isChecked();
    Settings::values.log_filter = ui->log_filter_edit->text().toStdString();
    Settings::values.log_flush_line = ui->flush_line->isChecked();
    Settings::values.censor_username = ui->censor_username->isChecked();
    Settings::values.program_args = ui->homebrew_args_edit->text().toStdString();
    Settings::values.enable_fs_access_log = ui->fs_access_log->isChecked();
    Settings::values.reporting_services = ui->reporting_services->isChecked();
    Settings::values.dump_audio_commands = ui->dump_audio_commands->isChecked();
    Settings::values.quest_flag = ui->quest_flag->isChecked();
    Settings::values.use_dev_keys = ui->use_dev_keys->isChecked();
    Settings::values.use_debug_asserts = ui->use_debug_asserts->isChecked();
    Settings::values.use_auto_stub = ui->use_auto_stub->isChecked();
    Settings::values.enable_all_controllers = ui->enable_all_controllers->isChecked();
    Settings::values.renderer_debug = ui->enable_graphics_debugging->isChecked();
    Settings::values.enable_renderdoc_hotkey = ui->enable_renderdoc_hotkey->isChecked();
    Settings::values.disable_buffer_reorder = ui->disable_buffer_reorder->isChecked();
    Settings::values.renderer_shader_feedback = ui->enable_shader_feedback->isChecked();
    Settings::values.enable_nsight_aftermath = ui->enable_nsight_aftermath->isChecked();
    Settings::values.dump_guest_shaders = ui->dump_guest_shaders->isChecked();
    Settings::values.dump_macros = ui->dump_macros->isChecked();
    Settings::values.disable_shader_loop_safety_checks =
        ui->disable_loop_safety_checks->isChecked();
    Settings::values.disable_macro_jit = ui->disable_macro_jit->isChecked();
    Settings::values.disable_macro_hle = ui->disable_macro_hle->isChecked();
    Settings::values.extended_logging = ui->extended_logging->isChecked();
    Settings::values.perform_vulkan_check = ui->perform_vulkan_check->isChecked();
    Settings::values.serial_battery = ui->serial_battery_edit->text().toUInt();
    Settings::values.serial_unit = ui->serial_board_edit->text().toUInt();
    Settings::values.debug_knobs = ui->debug_knobs_spinbox->value();
    Settings::values.gpu_log_level =
        static_cast<Settings::GpuLogLevel>(ui->gpu_log_level->currentIndex());
    Settings::values.gpu_log_shader_dumps = ui->gpu_log_shader_dumps->isChecked();
    Debugger::ToggleConsole();
    Common::Log::Filter filter;
    filter.ParseFilterString(Settings::values.log_filter.GetValue());
    Common::Log::SetGlobalFilter(filter);
    Core::Crypto::KeyManager::Instance().ReloadKeys();
}

void ConfigureDebug::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureDebug::RetranslateUI() {
    ui->retranslateUi(this);
    SetupToolTips();
}

void ConfigureDebug::SetupToolTips() {
    using ConfigurationShared::SetRichToolTip;

    SetRichToolTip(ui->toggle_gdbstub, ui->toggle_gdbstub->text(),
                   tr("Starts a GDB server at game boot and keeps the game paused until a debugger "
                      "connects and resumes it. Adds a check after every memory access, which "
                      "slows games. Change it only with no game running."),
                   tr("The server listens on every network interface, so other devices on your "
                      "network can reach it; turn it off when you're done."));
    SetRichToolTip(ui->gdbport_spinbox, ui->label_11->text(),
                   tr("TCP port for the GDB server, from 1024 to 65535 (default 6543). Editable "
                      "only while Enable GDB Stub is checked; used at the next game boot."),
                   tr("If the log shows \"Stopping server\" right after boot, the port is probably "
                      "in use; pick another one."));
    SetRichToolTip(ui->log_filter_edit, ui->label_1->text(),
                   tr("Chooses which messages reach the log, as space-separated class:level rules "
                      "applied left to right. Default is *:Info. Levels: Trace, Debug, Info, "
                      "Warning, Error, Critical. Applies when you press OK."),
                   tr("Example: *:Info Service.FS:Debug. A rule covers only the class it names, "
                      "not its subclasses, and Trace lines exist only in debug builds."));
    SetRichToolTip(ui->gpu_log_level, ui->gpu_log_level_label->text(),
                   tr("Writes a separate Vulkan log, nemyu_gpu.log, in the log folder. Used at the "
                      "next game boot.\n"
                      "Off: nothing.\n"
                      "Errors, Standard: draws, dispatches, render passes, pipeline binds and "
                      "memory allocations (both log the same).\n"
                      "Verbose, All: also queue submits, shader compiles and pipeline state "
                      "changes."),
                   tr("The GPU log has no size limit and every level above Off records each draw, "
                      "so keep test runs short."));
    SetRichToolTip(ui->toggle_console, ui->toggle_console->text(),
                   tr("Opens a separate console window that shows log lines live, in color, "
                      "filtered by the Global Log Filter. Applies when you press OK; can't be "
                      "changed while a game runs."),
                   tr("Your user name is not censored in this window, unlike in nemyu_log.txt, so "
                      "take care when streaming or sharing screenshots."));
    SetRichToolTip(ui->extended_logging, ui->extended_logging->text(),
                   tr("Raises the log file size limit from 100 MB to 1 GB. Past the limit, Nemyu "
                      "stops writing to the file. Resets to off when Nemyu closes."),
                   tr("Turn it on for long sessions with a detailed Global Log Filter, when the "
                      "log cuts off before the problem shows up."));
    SetRichToolTip(ui->open_log_button, ui->open_log_button->text(),
                   tr("Opens the log folder. nemyu_log.txt holds the current session and "
                      "nemyu_log.txt.old.txt the previous one; the GPU log and reports are saved "
                      "there too."),
                   tr("Attach nemyu_log.txt to bug reports, after closing Nemyu so the last "
                      "buffered lines are written."));
    SetRichToolTip(ui->homebrew_args_edit, ui->label_3->text(),
                   tr("Text passed as launch arguments to the program you boot. Homebrew (.nro) "
                      "receives it after the name \"homebrew\"; games also receive it through "
                      "their loader module. Can't be changed while a game runs."),
                   tr("The text is saved and passed to every program you boot, so clear it when "
                      "you finish testing."));
    SetRichToolTip(ui->enable_graphics_debugging, ui->enable_graphics_debugging->text(),
                   tr("Loads the Vulkan validation layer and writes its messages to the log, or "
                      "uses a debug context on OpenGL, and adds an extra check on every translated "
                      "shader. Games run noticeably slower. Applies at the next game boot."),
                   tr("The validation layer comes with the Vulkan SDK; without it the log says the "
                      "layer is not available and nothing is validated."));
    SetRichToolTip(ui->enable_renderdoc_hotkey, ui->enable_renderdoc_hotkey->text(),
                   tr("At game boot, connects to RenderDoc if Nemyu was launched from it. The "
                      "Toggle Renderdoc Capture hotkey then starts a capture on one press and ends "
                      "it on the next. Does nothing without RenderDoc."),
                   tr("That hotkey has no key by default, so assign one in the Hotkeys settings "
                      "first."));
    SetRichToolTip(ui->enable_shader_feedback, ui->enable_shader_feedback->text(),
                   tr("Turns on the driver's pipeline statistics extension and, after the shader "
                      "cache loads at game boot, logs the average code size, register and branch "
                      "counts of the cached pipelines. Vulkan only."),
                   tr("Also turn on Enable Graphics Debugging; Nemyu only asks the driver to "
                      "capture these statistics when both are on."));
    SetRichToolTip(ui->enable_nsight_aftermath, ui->enable_nsight_aftermath->text(),
                   tr("On NVIDIA GPUs with Vulkan, turns on the driver's GPU crash diagnostics at "
                      "the next game boot. Dump files also need a build compiled with the "
                      "Aftermath SDK, which is off by default, plus GFSDK_Aftermath_Lib.x64.dll."),
                   tr("In such builds dumps go to log/gpucrash, which is wiped at every game boot, "
                      "so copy them out before starting another game."));
    SetRichToolTip(ui->disable_loop_safety_checks, ui->disable_loop_safety_checks->text(),
                   tr("Removes the 8,192-iteration limit Nemyu adds to every loop in translated "
                      "shaders. Without it, a shader loop that never ends can hang the GPU. Can't "
                      "be changed while a game runs."),
                   tr("Try it only when an effect that needs very long shader loops renders "
                      "incomplete."));
    SetRichToolTip(ui->disable_buffer_reorder, ui->disable_buffer_reorder->text(),
                   tr("Records every buffer upload in order with the draws, instead of moving safe "
                      "uploads into a separate command buffer that runs first. Ends render passes "
                      "more often, which can cost speed. Vulkan only."),
                   tr("Turn it on for RenderDoc captures, so each upload appears next to the draw "
                      "that uses it."));
    SetRichToolTip(ui->dump_guest_shaders, ui->dump_guest_shaders->text(),
                   tr("Saves each game shader's original GPU code, before translation, as .ash "
                      "files in the dump folder, named by program ID, shader hash and stage. Works "
                      "on Vulkan and OpenGL. Resets to off when Nemyu closes."),
                   tr("The files are padded for nvdisasm, so use it to disassemble them."));
    SetRichToolTip(ui->gpu_log_shader_dumps, ui->gpu_log_shader_dumps->text(),
                   tr("Saves the SPIR-V that Nemyu generates for each compiled shader as .spv "
                      "files in the dump folder, named by program ID and shader hash. Vulkan only; "
                      "works without GPU logging. Can't be changed while a game runs."),
                   tr("Inspect the files with spirv-dis, spirv-cross or spirv-val from "
                      "SPIRV-Tools, or in RenderDoc."));
    SetRichToolTip(ui->disable_macro_jit, ui->disable_macro_jit->text(),
                   tr("Runs GPU macros that have no built-in replacement through the interpreter "
                      "instead of compiling them to x86-64 code, which makes games slower. Switch "
                      "1 games only; Switch 2 games use a separate macro path. Applies at the next "
                      "game boot."),
                   tr("If a graphics bug goes away with this on, the macro JIT is the likely "
                      "cause."));
    SetRichToolTip(ui->dump_macros, ui->dump_macros->text(),
                   tr("Saves each GPU macro a Switch 1 game runs as a .macro file in the dump "
                      "folder, once per macro when it is first compiled. Switch 2 games use a "
                      "separate macro path and produce none. Resets to off when Nemyu closes."));
    SetRichToolTip(ui->disable_macro_hle, ui->disable_macro_hle->text(),
                   tr("Stops replacing known GPU macros with Nemyu's built-in versions, so every "
                      "macro runs through the JIT or interpreter. Makes games slower. Only affects "
                      "Switch 1 games; Switch 2 games use a separate macro path. Can't be changed "
                      "while a game runs."),
                   tr("If a graphics bug goes away with this on, one of the built-in macro "
                      "replacements is the likely cause."));
    SetRichToolTip(ui->quest_flag, ui->quest_flag->text(),
                   tr("Reports the console as a store demo (kiosk) unit to software that reads "
                      "this flag through the settings service. Software that checks it may switch "
                      "to its store-demo behavior."),
                   tr("Turn it off if a game unexpectedly behaves like a store demo."));
    SetRichToolTip(ui->use_dev_keys, ui->use_dev_keys->text(),
                   tr("Loads dev.keys instead of prod.keys from the keys folder, for content "
                      "signed with development keys. Keys reload when you press OK, on top of the "
                      "ones already loaded, so restart Nemyu for a clean switch."),
                   tr("Leave it off for retail games and firmware, which need prod.keys; with it "
                      "on they may fail to open."));
    SetRichToolTip(ui->use_debug_asserts, ui->use_debug_asserts->text(),
                   tr("Turns every failed internal check (assert), including calls to "
                      "unimplemented functions, into a debugger breakpoint instead of only a "
                      "Critical log line. With no debugger attached, that ends Nemyu at the first "
                      "one."),
                   tr("Use it only with Nemyu running under a debugger such as Visual Studio."));
    SetRichToolTip(ui->perform_vulkan_check, ui->perform_vulkan_check->text(),
                   tr("Meant to skip the launch test where a second copy of Nemyu starts Vulkan "
                      "and, if it crashes, Nemyu warns and switches to OpenGL. The test runs "
                      "before settings are loaded, so it always runs; this box has no effect."),
                   tr("If the \"Broken Vulkan Installation Detected\" warning appears at launch, "
                      "update or reinstall your GPU driver."));
    SetRichToolTip(ui->enable_all_controllers, ui->enable_all_controllers->text(),
                   tr("Adds Poke Ball Plus, NES, SNES, N64 and Sega Genesis controllers to the "
                      "controller type lists when the game allows them. Nemyu's code treats these "
                      "types as unsupported."),
                   tr("Close and reopen Settings for the new types to show up in the Controls "
                      "pages."));
    SetRichToolTip(ui->use_auto_stub, ui->use_auto_stub->text(),
                   tr("Replies \"success\" to service calls Nemyu hasn't implemented so the game "
                      "can keep going, and logs each one only once. Switch 2 programs always get "
                      "this, so it only changes Switch 1 games."),
                   tr("Try it when a Switch 1 game stops right after an \"Unknown / "
                      "unimplemented\" log line; faked replies can cause odd behavior later."));
    SetRichToolTip(ui->dump_audio_commands, ui->dump_audio_commands->text(),
                   tr("Writes the audio renderer's command list to the log as a Warning each time "
                      "it changes. Only games that use the audio renderer produce output. Resets "
                      "to off when Nemyu closes."),
                   tr("Turn on Show Log in Console to watch it live; each change adds a full "
                      "command list, so the log grows quickly."));
    SetRichToolTip(ui->flush_line, ui->flush_line->text(),
                   tr("Writes every log line to disk right away instead of in 4 KB chunks; Error "
                      "and Critical lines are always written right away. Can slow Nemyu down when "
                      "a lot is being logged."),
                   tr("Turn it on when Nemyu crashes and the last lines before the crash are "
                      "missing from the log."));
    SetRichToolTip(ui->fs_access_log, ui->fs_access_log->text(),
                   tr("Tells games to send their file system access log, which Nemyu appends to "
                      "FsAccessLog.txt in the emulated SD card folder. Only games that support it "
                      "write anything. Can't be changed while a game runs."),
                   tr("FsAccessLog.txt is emptied each time Nemyu starts, so copy it before "
                      "relaunching."));
    SetRichToolTip(ui->reporting_services, ui->reporting_services->text(),
                   tr("Saves JSON reports in subfolders of the log folder: fatal errors and error "
                      "screens a game raises, unimplemented functions and applets, SVC breaks and "
                      "play reports. Crashes of Nemyu itself are not covered. Resets to off when "
                      "Nemyu closes."),
                   tr("Without Enable Auto-Stub, every call to an unimplemented function writes a "
                      "report, so the folders can fill up fast."));
    SetRichToolTip(ui->censor_username, ui->censor_username->text(),
                   tr("Replaces your system user name with \"user\" in nemyu_log.txt, so folder "
                      "paths in shared logs don't reveal it. On by default."),
                   tr("The console window, nemyu_gpu.log and report files are not censored, so "
                      "check them before sharing."));
    SetRichToolTip(ui->serial_battery_edit, ui->label_12->text(),
                   tr("Number used to build the battery lot code that the emulated console reports "
                      "to software. 0 makes Nemyu pick a random number at the next launch."),
                   tr("Type digits only; any other text is saved as 0 and replaced by a random "
                      "number."));
    SetRichToolTip(ui->serial_board_edit, ui->label_13->text(),
                   tr("Number used to build the console serial number that the emulated system "
                      "reports; its region letter follows the Region setting. 0 makes Nemyu pick a "
                      "random number at the next launch."),
                   tr("Type digits only; any other text is saved as 0 and replaced by a random "
                      "number."));
    SetRichToolTip(ui->debug_knobs_spinbox, ui->label_debug_knobs->text(),
                   tr("Holds a 16-bit mask of developer test switches. Nothing in this desktop "
                      "build reads it, so any value has no effect."));
}
