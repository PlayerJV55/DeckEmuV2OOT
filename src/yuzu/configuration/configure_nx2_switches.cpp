// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <system_error>
#include <utility>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidgetItem>
#include "common/nx2_profiles.h"
#include "ui_configure_nx2_switches.h"
#include "yuzu/configuration/configure_nx2_switches.h"
#include "yuzu/configuration/nx2_switch_catalog.h"

namespace {

using Nx2SwitchCatalog::Category;
using Nx2SwitchCatalog::Entry;
using Nx2SwitchCatalog::ValueType;

enum Column : int {
    COLUMN_TICKED,
    COLUMN_NAME,
    COLUMN_VALUE,
    COLUMN_DEFAULT,
    COLUMN_CATEGORY,
    COLUMN_COUNT,
};

/// The category filter's entry for switches the catalog doesn't know.
constexpr int CUSTOM_CATEGORY = static_cast<int>(Nx2SwitchCatalog::NUM_CATEGORIES);

bool HasFlag(const Entry* entry, u8 flag) {
    return entry != nullptr && (entry->flags & flag) != 0;
}

QString FromLatin(const char* text) {
    return text != nullptr ? QString::fromLatin1(text) : QString{};
}

QString FromPath(const std::filesystem::path& path) {
    return QDir::toNativeSeparators(QString::fromStdU16String(path.u16string()));
}

std::filesystem::path ToPath(const QString& path) {
    return std::filesystem::path{path.toStdU16String()};
}

/// The value a variable has in this process, which LoadNx2Switches may have set from a file.
QString ProcessValue(const QString& name) {
    const char* const value = std::getenv(name.toStdString().c_str());
    return value != nullptr ? QString::fromLocal8Bit(value) : QString{};
}

std::map<std::string, std::string> ToMap(const std::vector<Common::Nx2Switch>& switches) {
    std::map<std::string, std::string> map;
    for (const auto& [name, value] : switches) {
        map[name] = value;
    }
    return map;
}

/// Word-wraps text into '#' comment lines for an exported switch file.
QString CommentLines(const QString& text) {
    QString lines;
    QString line = QStringLiteral("#");
    for (const QString& word : text.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (line.size() > 1 && line.size() + 1 + word.size() > 99) {
            lines += line;
            lines += QLatin1Char('\n');
            line = QStringLiteral("#");
        }
        line += QLatin1Char(' ');
        line += word;
    }
    lines += line;
    lines += QLatin1Char('\n');
    return lines;
}

} // Anonymous namespace

ConfigureNx2Switches::ConfigureNx2Switches(QWidget* parent)
    : QWidget(parent), ui{std::make_unique<Ui::ConfigureNx2Switches>()} {
    ui->setupUi(this);

    ui->table->setColumnCount(COLUMN_COUNT);
    QHeaderView* const header = ui->table->horizontalHeader();
    header->setSectionResizeMode(COLUMN_TICKED, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(COLUMN_NAME, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(COLUMN_VALUE, QHeaderView::Interactive);
    header->setSectionResizeMode(COLUMN_DEFAULT, QHeaderView::Stretch);
    header->setSectionResizeMode(COLUMN_CATEGORY, QHeaderView::ResizeToContents);
    header->resizeSection(COLUMN_VALUE, 280);
    // Tall enough for the combo boxes and spin boxes in the Value column
    ui->table->verticalHeader()->setDefaultSectionSize(QComboBox().sizeHint().height() + 2);

    PopulateCategoryFilter();
    PopulateProfileCombo();
    ui->new_profile_button->setEnabled(!Common::GetNx2ProfileDirectory().empty());
    connect(ui->profile_combo, &QComboBox::currentIndexChanged, this,
            &ConfigureNx2Switches::SelectProfile);
    connect(ui->new_profile_button, &QPushButton::clicked, this,
            &ConfigureNx2Switches::NewProfile);
    connect(ui->rename_profile_button, &QPushButton::clicked, this,
            &ConfigureNx2Switches::RenameProfile);
    connect(ui->delete_profile_button, &QPushButton::clicked, this,
            &ConfigureNx2Switches::DeleteProfile);
    connect(ui->search_edit, &QLineEdit::textChanged, this, &ConfigureNx2Switches::UpdateFilter);
    connect(ui->category_combo, &QComboBox::currentIndexChanged, this,
            &ConfigureNx2Switches::UpdateFilter);
    connect(ui->overridden_only, &QCheckBox::toggled, this, &ConfigureNx2Switches::UpdateFilter);
    connect(ui->import_button, &QPushButton::clicked, this, &ConfigureNx2Switches::Import);
    connect(ui->export_button, &QPushButton::clicked, this, &ConfigureNx2Switches::Export);
    connect(ui->reset_button, &QPushButton::clicked, this, &ConfigureNx2Switches::ResetAll);
    connect(ui->table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item->column() == COLUMN_TICKED) {
            UpdateEditorEnabled(item->row());
        }
    });

    SetConfiguration();
}

ConfigureNx2Switches::~ConfigureNx2Switches() = default;

void ConfigureNx2Switches::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }
    QWidget::changeEvent(event);
}

void ConfigureNx2Switches::RetranslateUI() {
    ui->retranslateUi(this);
    PopulateCategoryFilter();
    PopulateProfileCombo();
    UpdateFileLabel();
    // Rebuilt with the new texts; values typed into unticked rows are dropped.
    Populate(CurrentSwitches());
}

void ConfigureNx2Switches::SetConfiguration() {
    switch_file =
        profile.empty() ? Common::GetUserNx2SwitchFile() : Common::GetNx2ProfileFile(profile);
    const std::vector<Common::Nx2Switch> file_switches = Common::ReadNx2SwitchFile(switch_file);
    saved = ToMap(file_switches);
    other_files.clear();
    for (const std::filesystem::path& file : Common::GetNx2SwitchFiles()) {
        if (file == switch_file) {
            continue;
        }
        for (const auto& [name, value] : Common::ReadNx2SwitchFile(file)) {
            other_files[name] = {value, file};
        }
    }
    UpdateFileLabel();
    setEnabled(!switch_file.empty());
    ui->rename_profile_button->setEnabled(!profile.empty());
    ui->delete_profile_button->setEnabled(!profile.empty());
    Populate(file_switches);
}

bool ConfigureNx2Switches::ApplyConfiguration() {
    if (switch_file.empty()) {
        return true;
    }
    const std::vector<Common::Nx2Switch> switches = CurrentSwitches();
    std::map<std::string, std::string> current = ToMap(switches);
    if (current == saved) {
        return true;
    }
    if (!Common::WriteNx2SwitchFile(switch_file, switches)) {
        QMessageBox::warning(this, tr("Advanced Switches"),
                             tr("Could not write the switch file %1.").arg(FromPath(switch_file)));
        return false;
    }
    saved = std::move(current);
    return true;
}

void ConfigureNx2Switches::UpdateFileLabel() {
    if (switch_file.empty()) {
        ui->file_label->setText(tr("This platform has no switch file."));
    } else if (profile.empty()) {
        ui->file_label->setText(tr("Switch file: %1").arg(FromPath(switch_file)));
        ui->restart_label->setText(tr("Changes apply after restarting Nemyu."));
    } else {
        ui->file_label->setText(
            tr("Profile file: %1\nA game uses this profile when it is picked in the game's "
               "Properties. Ticked switches replace the global value while that game runs; "
               "unticked ones are not set, so the game keeps the value under \"If not set\".")
                .arg(FromPath(switch_file)));
        ui->restart_label->setText(
            tr("Changes apply the next time a game using this profile starts."));
    }
}

void ConfigureNx2Switches::PopulateProfileCombo() {
    [[maybe_unused]] const QSignalBlocker blocker(ui->profile_combo);
    ui->profile_combo->clear();
    ui->profile_combo->addItem(tr("Global switches"), QString{});
    for (const std::string& name : Common::ListNx2Profiles()) {
        const QString profile_name = QString::fromStdString(name);
        ui->profile_combo->addItem(tr("Profile: %1").arg(profile_name), profile_name);
    }
    ui->profile_combo->setCurrentIndex(
        (std::max)(ui->profile_combo->findData(QString::fromStdString(profile)), 0));
}

void ConfigureNx2Switches::PopulateCategoryFilter() {
    const int previous = (std::max)(ui->category_combo->currentIndex(), 0);
    [[maybe_unused]] const QSignalBlocker blocker(ui->category_combo);
    ui->category_combo->clear();
    ui->category_combo->addItem(tr("All categories"), -1);
    for (size_t i = 0; i < Nx2SwitchCatalog::NUM_CATEGORIES; ++i) {
        ui->category_combo->addItem(Nx2SwitchCatalog::CategoryName(static_cast<Category>(i)),
                                    static_cast<int>(i));
    }
    ui->category_combo->addItem(tr("Not in the catalog"), CUSTOM_CATEGORY);
    ui->category_combo->setCurrentIndex(previous);
}

void ConfigureNx2Switches::Populate(const std::vector<Common::Nx2Switch>& ticked) {
    std::map<QString, QString> values;
    for (const auto& [name, value] : ticked) {
        values[QString::fromStdString(name)] = QString::fromStdString(value);
    }

    [[maybe_unused]] const QSignalBlocker blocker(ui->table);
    ui->table->clearContents();
    ui->table->setRowCount(0);
    rows.clear();
    ui->table->setHorizontalHeaderLabels({tr("On"), tr("Name"), tr("Value"),
                                          profile.empty() ? tr("Default") : tr("If not set"),
                                          tr("Category")});

    for (const Entry& entry : Nx2SwitchCatalog::Entries()) {
        AddRow(&entry, FromLatin(entry.name));
    }
    // Unknown switches of the file stay editable, so saving loses nothing
    for (const auto& item : values) {
        if (FindRow(item.first) < 0) {
            AddRow(nullptr, item.first);
        }
    }

    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const int index = static_cast<int>(i);
        if (row.locked) {
            // The environment's value when it set the switch, else what the file holds.
            const bool from_environment = Common::IsSetInRealEnvironment(row.name.toStdString());
            QString shown;
            if (from_environment) {
                shown = ProcessValue(row.name);
            } else if (const auto file_value = saved.find(row.name.toStdString());
                       file_value != saved.end()) {
                shown = QString::fromStdString(file_value->second);
            }
            SetEditor(index, shown);
            SetTicked(index, from_environment);
        } else if (const auto value = values.find(row.name); value != values.end()) {
            SetEditor(index, value->second);
            SetTicked(index, true);
        } else {
            // A profile's unticked row starts from the global value, so ticking it tweaks that
            const auto global = other_files.find(row.name.toStdString());
            if (!profile.empty() && global != other_files.end()) {
                SetEditor(index, QString::fromStdString(global->second.first));
            } else {
                SetEditor(index,
                          row.entry != nullptr ? FromLatin(row.entry->suggested) : QString{});
            }
            SetTicked(index, false);
        }
    }
    UpdateFilter();
}

void ConfigureNx2Switches::AddRow(const Entry* entry, const QString& name) {
    const std::string std_name = name.toStdString();
    const bool locked = HasFlag(entry, Nx2SwitchCatalog::EnvironmentOnly) ||
                        Common::IsSetInRealEnvironment(std_name);
    QString search_text = name;
    if (entry != nullptr) {
        search_text += QLatin1Char(' ');
        search_text += Nx2SwitchCatalog::Translate(entry->description);
    }
    rows.push_back({entry, name, search_text.toLower(), locked, {}});
    const QString tool_tip = ToolTip(rows.back());

    QString default_text = entry != nullptr ? Nx2SwitchCatalog::Translate(entry->default_value)
                                            : tr("Unknown");
    if (const auto other = other_files.find(std_name); other != other_files.end()) {
        const QString other_value = QString::fromStdString(other->second.first);
        default_text = profile.empty()
                           ? tr("%1; another file sets %2").arg(default_text, other_value)
                           : tr("%1 (global switches)").arg(other_value);
    }
    QString category_text = entry != nullptr ? Nx2SwitchCatalog::CategoryName(entry->category)
                                             : tr("Not in the catalog");
    const bool diagnostic = HasFlag(entry, Nx2SwitchCatalog::Diagnostic);
    if (diagnostic && entry->category != Category::Diagnostics) {
        category_text = tr("%1 (diagnostic)").arg(category_text);
    }

    const int index = ui->table->rowCount();
    ui->table->insertRow(index);
    const Qt::ItemFlags plain =
        locked ? Qt::ItemFlags{Qt::ItemIsSelectable} : Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    auto* const ticked = new QTableWidgetItem;
    ticked->setFlags(locked ? plain : plain | Qt::ItemIsUserCheckable);
    ticked->setCheckState(Qt::Unchecked);
    auto* const name_item = new QTableWidgetItem(name);
    auto* const value_item = new QTableWidgetItem;
    auto* const default_item = new QTableWidgetItem(default_text);
    auto* const category_item = new QTableWidgetItem(category_text);
    if (diagnostic) {
        QFont font = name_item->font();
        font.setItalic(true);
        name_item->setFont(font);
    }
    const std::pair<int, QTableWidgetItem*> cells[] = {
        {COLUMN_TICKED, ticked},         {COLUMN_NAME, name_item},
        {COLUMN_VALUE, value_item},      {COLUMN_DEFAULT, default_item},
        {COLUMN_CATEGORY, category_item},
    };
    for (const auto& [column, item] : cells) {
        if (column != COLUMN_TICKED) {
            item->setFlags(plain);
        }
        if (diagnostic) {
            // Translucent, so it reads on light and dark themes alike
            item->setBackground(QColor(255, 140, 0, 40));
        }
        item->setToolTip(tool_tip);
        ui->table->setItem(index, column, item);
    }
}

void ConfigureNx2Switches::SetEditor(int index, const QString& value) {
    Row& row = rows[static_cast<size_t>(index)];
    const Entry* const entry = row.entry;
    const ValueType type = entry != nullptr ? entry->type : ValueType::Text;
    if (row.locked && value.isEmpty()) {
        ui->table->removeCellWidget(index, COLUMN_VALUE);
        ui->table->item(index, COLUMN_VALUE)->setText(tr("Only from the environment"));
        return;
    }

    QWidget* editor = nullptr;
    switch (type) {
    case ValueType::Toggle:
        if (value == QStringLiteral("0") || value == QStringLiteral("1")) {
            auto* const box = new QCheckBox;
            box->setChecked(value == QStringLiteral("1"));
            box->setText(box->isChecked() ? tr("On") : tr("Off"));
            connect(box, &QCheckBox::toggled, box,
                    [box](bool checked) { box->setText(checked ? tr("On") : tr("Off")); });
            editor = box;
        }
        break;
    case ValueType::Presence:
        row.presence_value = value.isEmpty() ? QStringLiteral("1") : value;
        break;
    case ValueType::Choice: {
        int selected = -1;
        for (size_t i = 0; i < entry->choices.size(); ++i) {
            if (value == FromLatin(entry->choices[i].value)) {
                selected = static_cast<int>(i);
            }
        }
        if (selected >= 0) {
            auto* const combo = new QComboBox;
            for (const Nx2SwitchCatalog::Choice& choice : entry->choices) {
                combo->addItem(Nx2SwitchCatalog::Translate(choice.label), FromLatin(choice.value));
            }
            combo->setCurrentIndex(selected);
            editor = combo;
        }
        break;
    }
    case ValueType::Number: {
        bool is_number = false;
        const int number = value.toInt(&is_number);
        if (is_number && number >= entry->minimum && number <= entry->maximum) {
            auto* const spin = new QSpinBox;
            spin->setRange(entry->minimum, entry->maximum);
            spin->setValue(number);
            editor = spin;
        }
        break;
    }
    case ValueType::Text:
        break;
    }
    if (editor == nullptr && type != ValueType::Presence) {
        // Free text, or a value the typed editor can't show
        editor = new QLineEdit(value);
    }

    if (editor == nullptr) {
        ui->table->removeCellWidget(index, COLUMN_VALUE);
        ui->table->item(index, COLUMN_VALUE)->setText(row.locked ? value : tr("On while ticked"));
        return;
    }
    ui->table->item(index, COLUMN_VALUE)->setText({});
    editor->setToolTip(ui->table->item(index, COLUMN_NAME)->toolTip());
    ui->table->setCellWidget(index, COLUMN_VALUE, editor);
    UpdateEditorEnabled(index);
}

void ConfigureNx2Switches::UpdateEditorEnabled(int index) {
    if (QWidget* const editor = ui->table->cellWidget(index, COLUMN_VALUE)) {
        editor->setEnabled(!rows[static_cast<size_t>(index)].locked && IsTicked(index));
    }
}

QString ConfigureNx2Switches::EditorValue(int index) const {
    QWidget* const editor = ui->table->cellWidget(index, COLUMN_VALUE);
    if (const auto* const box = qobject_cast<QCheckBox*>(editor)) {
        return box->isChecked() ? QStringLiteral("1") : QStringLiteral("0");
    }
    if (const auto* const combo = qobject_cast<QComboBox*>(editor)) {
        return combo->currentData().toString();
    }
    if (const auto* const spin = qobject_cast<QSpinBox*>(editor)) {
        return QString::number(spin->value());
    }
    if (const auto* const edit = qobject_cast<QLineEdit*>(editor)) {
        return edit->text().trimmed();
    }
    return rows[static_cast<size_t>(index)].presence_value;
}

bool ConfigureNx2Switches::IsTicked(int index) const {
    return ui->table->item(index, COLUMN_TICKED)->checkState() == Qt::Checked;
}

void ConfigureNx2Switches::SetTicked(int index, bool ticked) {
    ui->table->item(index, COLUMN_TICKED)->setCheckState(ticked ? Qt::Checked : Qt::Unchecked);
    UpdateEditorEnabled(index);
}

int ConfigureNx2Switches::FindRow(const QString& name) const {
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

QString ConfigureNx2Switches::ToolTip(const Row& row) const {
    using Nx2SwitchCatalog::Translate;
    const auto paragraph = [](const QString& text) {
        return QStringLiteral("<p style=\"margin-top:4px; margin-bottom:0px\">%1</p>")
            .arg(text.toHtmlEscaped());
    };
    const auto section = [](const QString& label, const QString& text) {
        if (text.isEmpty()) {
            return QString{};
        }
        return QStringLiteral("<p style=\"margin-top:4px; margin-bottom:0px\"><b>%1</b> %2</p>")
            .arg(label.toHtmlEscaped(), text.toHtmlEscaped());
    };
    const auto note = [](const QString& text) {
        return QStringLiteral("<p style=\"margin-top:6px; margin-bottom:0px\"><i>%1</i></p>")
            .arg(text.toHtmlEscaped());
    };

    QString html = QStringLiteral("<b>%1</b>").arg(row.name.toHtmlEscaped());
    const Entry* const entry = row.entry;
    if (entry == nullptr) {
        html += note(tr("This build's list doesn't know this switch: it may be misspelled, or from "
                        "another build. It stays in the switch file while it is ticked."));
    } else {
        html += paragraph(Translate(entry->description));
        html += section(tr("Gains:"), Translate(entry->gains));
        html += section(tr("Downsides:"), Translate(entry->downsides));
        html += section(tr("Matters for:"), Translate(entry->games));
        html += section(tr("Default:"), Translate(entry->default_value));
        switch (entry->type) {
        case ValueType::Choice: {
            QStringList labels;
            for (const Nx2SwitchCatalog::Choice& choice : entry->choices) {
                labels.push_back(Translate(choice.label));
            }
            html += section(tr("Values:"), labels.join(QStringLiteral("; ")));
            break;
        }
        case ValueType::Number:
            html += section(tr("Range:"),
                            tr("%1 to %2").arg(entry->minimum).arg(entry->maximum));
            break;
        case ValueType::Presence:
            html += note(tr("Any value turns it on; untick it to turn it off."));
            break;
        case ValueType::Toggle:
        case ValueType::Text:
            break;
        }
        if (HasFlag(entry, Nx2SwitchCatalog::Diagnostic)) {
            html += note(tr("Diagnostic: it slows the emulator down, fills the log or changes "
                            "behaviour for testing. Leave it off unless you are tracking down a "
                            "problem."));
        }
        if (HasFlag(entry, Nx2SwitchCatalog::EnvironmentOnly)) {
            html += note(tr("Read before the switch files are, or not an EDEN_NX2_ name: set it in "
                            "the system environment instead."));
        }
        if (HasFlag(entry, Nx2SwitchCatalog::CliOnly)) {
            html += note(tr("Only nemyu-cli reads it."));
        }
        if (HasFlag(entry, Nx2SwitchCatalog::GuiOnly)) {
            html += note(tr("Only the Nemyu window reads it, not nemyu-cli."));
        }
        if (HasFlag(entry, Nx2SwitchCatalog::NotInThisBuild)) {
            html += note(tr("Read by changes this build may not include yet: it may do nothing "
                            "here."));
        }
    }
    const std::string std_name = row.name.toStdString();
    if (Common::IsSetInRealEnvironment(std_name)) {
        html += note(tr("Set to \"%1\" in the environment, which wins over the switch file.")
                         .arg(ProcessValue(row.name)));
    }
    if (const auto other = other_files.find(std_name); other != other_files.end()) {
        html += note(tr("%1 sets it to \"%2\"; a ticked row here wins.")
                         .arg(FromPath(other->second.second),
                              QString::fromStdString(other->second.first)));
    }
    // Left to itself, Qt wraps rich tooltips into tall, thin boxes
    return QStringLiteral("<table width=\"440\" cellspacing=\"0\" cellpadding=\"0\"><tr><td>%1"
                          "</td></tr></table>")
        .arg(html);
}

std::vector<Common::Nx2Switch> ConfigureNx2Switches::CurrentSwitches() const {
    std::vector<Common::Nx2Switch> switches;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        std::string name = row.name.toStdString();
        if (row.locked) {
            // Kept as the file has it: the environment overrides it, or the file can't set it
            if (const auto file_value = saved.find(name); file_value != saved.end()) {
                switches.push_back({std::move(name), file_value->second});
            }
        } else if (IsTicked(static_cast<int>(i))) {
            switches.push_back({std::move(name), EditorValue(static_cast<int>(i)).toStdString()});
        }
    }
    return switches;
}

void ConfigureNx2Switches::UpdateFilter() {
    const QString text = ui->search_edit->text().trimmed().toLower();
    const int category = ui->category_combo->currentData().toInt();
    const bool ticked_only = ui->overridden_only->isChecked();
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& row = rows[i];
        const int index = static_cast<int>(i);
        const int row_category =
            row.entry != nullptr ? static_cast<int>(row.entry->category) : CUSTOM_CATEGORY;
        const bool shown = (text.isEmpty() || row.search_text.contains(text)) &&
                           (category < 0 || category == row_category) &&
                           (!ticked_only || IsTicked(index));
        ui->table->setRowHidden(index, !shown);
    }
}

void ConfigureNx2Switches::Import() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Switches"), {},
                                                      tr("Switch files (*.txt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    const std::vector<Common::Nx2Switch> imported = Common::ReadNx2SwitchFile(ToPath(path));
    const QString shown_path = QDir::toNativeSeparators(path);
    if (imported.empty()) {
        QMessageBox::information(this, tr("Import Switches"),
                                 tr("%1 sets no EDEN_NX2_ switch.").arg(shown_path));
        return;
    }

    QMessageBox box(QMessageBox::Question, tr("Import Switches"),
                    tr("Import the %1 switch settings in %2?\n\nMerge keeps the switches ticked "
                       "now; Replace unticks them first.")
                        .arg(static_cast<int>(imported.size()))
                        .arg(shown_path),
                    QMessageBox::Cancel, this);
    QPushButton* const merge = box.addButton(tr("Merge"), QMessageBox::AcceptRole);
    QPushButton* const replace = box.addButton(tr("Replace"), QMessageBox::DestructiveRole);
    box.setDefaultButton(merge);
    box.exec();
    if (box.clickedButton() != merge && box.clickedButton() != replace) {
        return;
    }
    if (box.clickedButton() == replace) {
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!rows[i].locked) {
                SetTicked(static_cast<int>(i), false);
            }
        }
    }

    int skipped = 0;
    for (const auto& [name, value] : imported) {
        const QString switch_name = QString::fromStdString(name);
        int index = FindRow(switch_name);
        if (index < 0) {
            AddRow(nullptr, switch_name);
            index = static_cast<int>(rows.size()) - 1;
        }
        if (rows[static_cast<size_t>(index)].locked) {
            ++skipped;
            continue;
        }
        SetEditor(index, QString::fromStdString(value));
        SetTicked(index, true);
    }
    UpdateFilter();
    if (skipped > 0) {
        QMessageBox::information(this, tr("Import Switches"),
                                 tr("%1 of them were skipped: they are set in the environment, or "
                                    "a switch file can't set them.")
                                     .arg(skipped));
    }
}

void ConfigureNx2Switches::Export() {
    const std::vector<Common::Nx2Switch> switches = CurrentSwitches();
    if (switches.empty()) {
        QMessageBox::information(this, tr("Export Switches"), tr("No switch is ticked."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Switches"),
                                                      QStringLiteral("nx2_switches.txt"),
                                                      tr("Switch files (*.txt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    // In English, like the shipped template, so a shared file reads the same for everyone
    QString text = QStringLiteral(
        "# Nemyu NX2 switches, exported from the Advanced Switches settings page.\n"
        "# One NAME=VALUE per line; '#' starts a comment. Import this file on that page, or copy\n"
        "# it to a folder Nemyu reads nx2_switches.txt from.\n");
    for (const auto& [name, value] : switches) {
        text += QLatin1Char('\n');
        if (const Entry* const entry = Nx2SwitchCatalog::Find(name); entry != nullptr) {
            text += CommentLines(QString::fromUtf8(entry->description));
        }
        text += QString::fromStdString(name);
        text += QLatin1Char('=');
        text += QString::fromStdString(value);
        text += QLatin1Char('\n');
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(text.toUtf8()) < 0 ||
        !file.commit()) {
        QMessageBox::warning(this, tr("Export Switches"),
                             tr("Could not write %1.").arg(QDir::toNativeSeparators(path)));
    }
}

void ConfigureNx2Switches::ResetAll() {
    if (QMessageBox::question(this, tr("Reset All"),
                              tr("Untick every switch? Switches set in the environment stay as "
                                 "they are, and nothing is written until you press OK or "
                                 "Apply.")) != QMessageBox::Yes) {
        return;
    }
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].locked) {
            SetTicked(static_cast<int>(i), false);
        }
    }
    UpdateFilter();
}

void ConfigureNx2Switches::SelectProfile(int index) {
    const std::string selected = ui->profile_combo->itemData(index).toString().toStdString();
    if (selected == profile) {
        return;
    }
    if (!SaveOrDiscardChanges()) {
        [[maybe_unused]] const QSignalBlocker blocker(ui->profile_combo);
        ui->profile_combo->setCurrentIndex(
            ui->profile_combo->findData(QString::fromStdString(profile)));
        return;
    }
    profile = selected;
    SetConfiguration();
}

void ConfigureNx2Switches::NewProfile() {
    const std::optional<std::string> name = AskProfileName(tr("New Profile"), {});
    if (!name) {
        return;
    }
    const std::vector<Common::Nx2Switch> ticked = CurrentSwitches();
    std::vector<Common::Nx2Switch> initial;
    if (!ticked.empty()) {
        QMessageBox box(QMessageBox::Question, tr("New Profile"),
                        tr("Start the profile with the %1 switches ticked here, or empty?")
                            .arg(static_cast<int>(ticked.size())),
                        QMessageBox::Cancel, this);
        QPushButton* const copy = box.addButton(tr("Copy Ticked"), QMessageBox::AcceptRole);
        QPushButton* const empty = box.addButton(tr("Empty"), QMessageBox::ActionRole);
        box.setDefaultButton(copy);
        box.exec();
        if (box.clickedButton() == copy) {
            initial = ticked;
        } else if (box.clickedButton() != empty) {
            return;
        }
    }
    if (!SaveOrDiscardChanges()) {
        return;
    }
    if (!Common::CreateNx2Profile(*name, initial)) {
        QMessageBox::warning(this, tr("New Profile"),
                             tr("Could not create the profile file %1.")
                                 .arg(FromPath(Common::GetNx2ProfileFile(*name))));
        return;
    }
    profile = *name;
    PopulateProfileCombo();
    SetConfiguration();
}

void ConfigureNx2Switches::RenameProfile() {
    if (profile.empty()) {
        return;
    }
    const std::optional<std::string> name =
        AskProfileName(tr("Rename Profile"), QString::fromStdString(profile));
    if (!name || *name == profile || !SaveOrDiscardChanges()) {
        return;
    }
    if (!Common::RenameNx2Profile(profile, *name)) {
        QMessageBox::warning(this, tr("Rename Profile"), tr("Could not rename the profile."));
        return;
    }
    profile = *name;
    PopulateProfileCombo();
    SetConfiguration();
}

void ConfigureNx2Switches::DeleteProfile() {
    if (profile.empty()) {
        return;
    }
    if (QMessageBox::question(this, tr("Delete Profile"),
                              tr("Delete the profile \"%1\"? Games set to use it go back to the "
                                 "global switches.")
                                  .arg(QString::fromStdString(profile))) != QMessageBox::Yes) {
        return;
    }
    if (!Common::DeleteNx2Profile(profile)) {
        QMessageBox::warning(this, tr("Delete Profile"), tr("Could not delete the profile."));
        return;
    }
    profile.clear();
    PopulateProfileCombo();
    SetConfiguration();
}

bool ConfigureNx2Switches::HasChanges() const {
    return !switch_file.empty() && ToMap(CurrentSwitches()) != saved;
}

bool ConfigureNx2Switches::SaveOrDiscardChanges() {
    if (!HasChanges()) {
        return true;
    }
    const QString edited = profile.empty()
                               ? tr("the global switches")
                               : tr("the profile \"%1\"").arg(QString::fromStdString(profile));
    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, tr("Advanced Switches"), tr("Save the changes to %1?").arg(edited),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) {
        return ApplyConfiguration();
    }
    return answer == QMessageBox::Discard;
}

std::optional<std::string> ConfigureNx2Switches::AskProfileName(const QString& title,
                                                                const QString& current) {
    QString text = current;
    while (true) {
        bool ok = false;
        text = QInputDialog::getText(this, title, tr("Profile name:"), QLineEdit::Normal, text,
                                     &ok)
                   .trimmed();
        if (!ok || text.isEmpty()) {
            return std::nullopt;
        }
        const std::string name = text.toStdString();
        if (!Common::IsValidNx2ProfileName(name)) {
            QMessageBox::warning(
                this, title,
                tr("\"%1\" can't be used as a profile name. Profile names have at most 64 "
                   "characters, none of < > : \" / \\ | ? *, no dot or space at the end, and "
                   "can't be a device name such as CON or NUL.")
                    .arg(text));
            continue;
        }
        // A change of case only renames the same file
        std::error_code ec;
        if (text.compare(current, Qt::CaseInsensitive) != 0 &&
            std::filesystem::exists(Common::GetNx2ProfileFile(name), ec)) {
            QMessageBox::warning(this, title,
                                 tr("A profile named \"%1\" already exists.").arg(text));
            continue;
        }
        return name;
    }
}
