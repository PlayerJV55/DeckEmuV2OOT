// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/settings.h"
#include "core/core.h"
#include "ui_configure_cpu_debug.h"
#include "yuzu/configuration/configuration_shared.h"
#include "yuzu/configuration/configure_cpu_debug.h"

ConfigureCpuDebug::ConfigureCpuDebug(const Core::System& system_, QWidget* parent)
    : QWidget(parent), ui{std::make_unique<Ui::ConfigureCpuDebug>()}, system{system_} {
    ui->setupUi(this);

    SetConfiguration();
    SetupToolTips();
}

ConfigureCpuDebug::~ConfigureCpuDebug() = default;

void ConfigureCpuDebug::SetConfiguration() {
    const bool runtime_lock = !system.IsPoweredOn();

    ui->cpuopt_page_tables->setEnabled(runtime_lock);
    ui->cpuopt_page_tables->setChecked(Settings::values.cpuopt_page_tables.GetValue());
    ui->cpuopt_block_linking->setEnabled(runtime_lock);
    ui->cpuopt_block_linking->setChecked(Settings::values.cpuopt_block_linking.GetValue());
    ui->cpuopt_return_stack_buffer->setEnabled(runtime_lock);
    ui->cpuopt_return_stack_buffer->setChecked(
        Settings::values.cpuopt_return_stack_buffer.GetValue());
    ui->cpuopt_fast_dispatcher->setEnabled(runtime_lock);
    ui->cpuopt_fast_dispatcher->setChecked(Settings::values.cpuopt_fast_dispatcher.GetValue());
    ui->cpuopt_context_elimination->setEnabled(runtime_lock);
    ui->cpuopt_context_elimination->setChecked(
        Settings::values.cpuopt_context_elimination.GetValue());
    ui->cpuopt_const_prop->setEnabled(runtime_lock);
    ui->cpuopt_const_prop->setChecked(Settings::values.cpuopt_const_prop.GetValue());
    ui->cpuopt_misc_ir->setEnabled(runtime_lock);
    ui->cpuopt_misc_ir->setChecked(Settings::values.cpuopt_misc_ir.GetValue());
    ui->cpuopt_reduce_misalign_checks->setEnabled(runtime_lock);
    ui->cpuopt_reduce_misalign_checks->setChecked(
        Settings::values.cpuopt_reduce_misalign_checks.GetValue());
    ui->cpuopt_fastmem->setEnabled(runtime_lock);
    ui->cpuopt_fastmem->setChecked(Settings::values.cpuopt_fastmem.GetValue());
    ui->cpuopt_fastmem_exclusives->setEnabled(runtime_lock);
    ui->cpuopt_fastmem_exclusives->setChecked(
        Settings::values.cpuopt_fastmem_exclusives.GetValue());
    ui->cpuopt_recompile_exclusives->setEnabled(runtime_lock);
    ui->cpuopt_recompile_exclusives->setChecked(
        Settings::values.cpuopt_recompile_exclusives.GetValue());
    ui->cpuopt_ignore_memory_aborts->setEnabled(runtime_lock);
    ui->cpuopt_ignore_memory_aborts->setChecked(
        Settings::values.cpuopt_ignore_memory_aborts.GetValue());
}

void ConfigureCpuDebug::ApplyConfiguration() {
    Settings::values.cpuopt_page_tables = ui->cpuopt_page_tables->isChecked();
    Settings::values.cpuopt_block_linking = ui->cpuopt_block_linking->isChecked();
    Settings::values.cpuopt_return_stack_buffer = ui->cpuopt_return_stack_buffer->isChecked();
    Settings::values.cpuopt_fast_dispatcher = ui->cpuopt_fast_dispatcher->isChecked();
    Settings::values.cpuopt_context_elimination = ui->cpuopt_context_elimination->isChecked();
    Settings::values.cpuopt_const_prop = ui->cpuopt_const_prop->isChecked();
    Settings::values.cpuopt_misc_ir = ui->cpuopt_misc_ir->isChecked();
    Settings::values.cpuopt_reduce_misalign_checks = ui->cpuopt_reduce_misalign_checks->isChecked();
    Settings::values.cpuopt_fastmem = ui->cpuopt_fastmem->isChecked();
    Settings::values.cpuopt_fastmem_exclusives = ui->cpuopt_fastmem_exclusives->isChecked();
    Settings::values.cpuopt_recompile_exclusives = ui->cpuopt_recompile_exclusives->isChecked();
    Settings::values.cpuopt_ignore_memory_aborts = ui->cpuopt_ignore_memory_aborts->isChecked();
}

void ConfigureCpuDebug::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureCpuDebug::RetranslateUI() {
    ui->retranslateUi(this);
    SetupToolTips();
}

void ConfigureCpuDebug::SetupToolTips() {
    using ConfigurationShared::SetRichToolTip;

    SetRichToolTip(ui->cpuopt_page_tables, ui->cpuopt_page_tables->text(),
                   tr("Lets compiled game code look up guest memory through the page table inline. "
                      "When off, accesses not handled by Host MMU Emulation call Nemyu's slower "
                      "memory functions. Only used when CPU > Accuracy is Debugging; locked while "
                      "a game runs."),
                   tr("To send every access through Nemyu's memory functions, also turn off Host "
                      "MMU Emulation (general memory instructions)."));
    SetRichToolTip(ui->cpuopt_block_linking, ui->cpuopt_block_linking->text(),
                   tr("Lets a compiled code block jump straight to the next one when the target is "
                      "known at compile time. Off sends those jumps through the slower dispatcher "
                      "lookup. Only used when CPU > Accuracy is Debugging; locked while a game "
                      "runs."),
                   tr("Dynarmic marks this optimization as safe, so turning it off only helps when "
                      "hunting a JIT bug."));
    SetRichToolTip(ui->cpuopt_return_stack_buffer, ui->cpuopt_return_stack_buffer->text(),
                   tr("Predicts where functions return to, like a real CPU's return stack buffer, "
                      "so returns skip the dispatcher lookup. Off sends every return through the "
                      "dispatcher. Only used when CPU > Accuracy is Debugging; locked while a game "
                      "runs."));
    SetRichToolTip(ui->cpuopt_fast_dispatcher, ui->cpuopt_fast_dispatcher->text(),
                   tr("Checks a small cache of recent jump targets in assembly before falling back "
                      "to the slower dispatcher. Off always uses the slower one. Only used when "
                      "CPU > Accuracy is Debugging; locked while a game runs."));
    SetRichToolTip(ui->cpuopt_context_elimination, ui->cpuopt_context_elimination->text(),
                   tr("Removes redundant reads and writes of the emulated CPU registers in the "
                      "JIT's intermediate code. Off keeps every access. Only used when CPU > "
                      "Accuracy is Debugging; locked while a game runs."));
    SetRichToolTip(ui->cpuopt_const_prop, ui->cpuopt_const_prop->text(),
                   tr("Works out values already known at compile time (constant propagation) in "
                      "the JIT's intermediate code, instead of computing them at run time. Only "
                      "used when CPU > Accuracy is Debugging; locked while a game runs."));
    SetRichToolTip(ui->cpuopt_misc_ir, ui->cpuopt_misc_ir->text(),
                   tr("Runs Dynarmic's other safe optimizations on the JIT's intermediate code. "
                      "Only used when CPU > Accuracy is Debugging; locked while a game runs."),
                   tr("Turn the optimizations on this page off one at a time; the one whose "
                      "removal fixes a bug points at the faulty JIT pass."));
    SetRichToolTip(ui->cpuopt_reduce_misalign_checks, ui->cpuopt_reduce_misalign_checks->text(),
                   tr("On the page-table path, sends a misaligned memory access to the slow path "
                      "only when it crosses a page boundary. Off sends every misaligned access "
                      "there, which is slower. Only used when CPU > Accuracy is Debugging; locked "
                      "while a game runs."),
                   tr("Accesses handled by Host MMU Emulation skip this check, so it matters "
                      "mostly with that option off."));
    SetRichToolTip(ui->cpuopt_fastmem, ui->cpuopt_fastmem->text(),
                   tr("Lets compiled code read and write guest memory directly through a host "
                      "memory mirror (fastmem). Off uses software page lookups, which is slower, "
                      "and also disables the exclusive variant below. Only used when CPU > "
                      "Accuracy is Debugging; locked while a game runs."),
                   tr("With it off, Enable inline page tables decides whether accesses use the "
                      "page table or Nemyu's memory functions."));
    SetRichToolTip(ui->cpuopt_fastmem_exclusives, ui->cpuopt_fastmem_exclusives->text(),
                   tr("Runs exclusive (atomic) memory instructions on the host memory mirror too. "
                      "Dynarmic notes this relies on x86 compare-exchange and may not be fully "
                      "accurate. Needs the general Host MMU option. Only used when CPU > Accuracy "
                      "is Debugging; locked while a game runs."),
                   tr("Turn it off to check whether a multithreading bug comes from the less exact "
                      "host atomics."));
    SetRichToolTip(ui->cpuopt_recompile_exclusives, ui->cpuopt_recompile_exclusives->text(),
                   tr("When an exclusive instruction faults on the host memory mirror, recompiles "
                      "that code block to use Nemyu's memory functions, so the fault isn't hit "
                      "again. Off keeps taking the fault. Only used when CPU > Accuracy is "
                      "Debugging; locked while a game runs."),
                   tr("Only matters while exclusive Host MMU Emulation is on."));
    SetRichToolTip(ui->cpuopt_ignore_memory_aborts, ui->cpuopt_ignore_memory_aborts->text(),
                   tr("Lets reads and writes to unmapped guest memory go on: reads return zero, "
                      "writes are dropped. Off logs \"unmapped memory access\" and stops that game "
                      "thread, in every CPU accuracy mode. Enable GDB Stub always applies the Off "
                      "behavior. Locked while a game runs."),
                   tr("Turn it off to find where a game touches bad memory; the log then shows the "
                      "address and a backtrace."));
}
