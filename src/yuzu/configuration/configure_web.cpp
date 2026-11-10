// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2017 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <QIcon>
#include <QMessageBox>
#include "yuzu/configuration/configure_web.h"

#if QT_VERSION_MAJOR >= 6
#include <QRegularExpressionValidator>
#else
#include <QRegExpValidator>
#endif

#include <QtConcurrentRun>
#include "common/settings.h"
#include "qt_common/config/uisettings.h"
#include "ui_configure_web.h"
#include "yuzu/configuration/configuration_shared.h"

ConfigureWeb::ConfigureWeb(QWidget* parent)
    : QWidget(parent), ui(std::make_unique<Ui::ConfigureWeb>()), m_rng{QRandomGenerator::system()} {
    ui->setupUi(this);

    QString user_regex = QStringLiteral(".{4,20}");
    QString token_regex = QStringLiteral("[a-z]{48}");

#if QT_VERSION_MAJOR >= 6
    QRegularExpressionValidator* username_validator = new QRegularExpressionValidator(this);
    QRegularExpressionValidator* token_validator = new QRegularExpressionValidator(this);

    username_validator->setRegularExpression(QRegularExpression(user_regex));
    token_validator->setRegularExpression(QRegularExpression(token_regex));
#else
    QRegExpValidator* username_validator = new QRegExpValidator(this);
    QRegExpValidator* token_validator = new QRegExpValidator(this);

    username_validator->setRegExp(QRegExp(user_regex));
    token_validator->setRegExp(QRegExp(token_regex));
#endif

    ui->edit_username->setValidator(username_validator);
    ui->edit_token->setValidator(token_validator);

    connect(ui->button_generate, &QPushButton::clicked, this, &ConfigureWeb::GenerateToken);

    SetConfiguration();
    RetranslateUI();
}

ConfigureWeb::~ConfigureWeb() = default;

void ConfigureWeb::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigureWeb::RetranslateUI() {
    ui->retranslateUi(this);
    SetupToolTips();
}

void ConfigureWeb::SetupToolTips() {
    using ConfigurationShared::SetRichToolTip;

    SetRichToolTip(ui->edit_username, ui->label_username->text(),
                   tr("Sets your Nemyu web service name. Public rooms use it with the token: "
                      "hosting one needs both, and joining one from the lobby uses them so the "
                      "host can verify you. Must be 4 to 20 characters."),
                   tr("The multiplayer windows also fill it in as your nickname until you type a "
                      "different one there."));
    SetRichToolTip(ui->edit_token, ui->label_token->text(),
                   tr("Sets the secret sent with your username to the Nemyu web service when you "
                      "host a public room or join one from the lobby. If empty, Nemyu makes a "
                      "random one at startup. Must be 48 lowercase letters."),
                   tr("Treat it like a password: Nemyu keeps it out of logs, but the field shows "
                      "it in plain text while you edit it."));
    SetRichToolTip(ui->button_generate, ui->button_generate->text(),
                   tr("Replaces the token with a new random 48-letter one. It is saved when you "
                      "press OK or Apply."));
}

void ConfigureWeb::SetConfiguration() {
    connect(ui->edit_username, &QLineEdit::textChanged, this, &ConfigureWeb::VerifyLogin);
    connect(ui->edit_token, &QLineEdit::textChanged, this, &ConfigureWeb::VerifyLogin);

    ui->edit_username->setText(QString::fromStdString(Settings::values.eden_username.GetValue()));
    ui->edit_token->setText(QString::fromStdString(Settings::values.eden_token.GetValue()));

    VerifyLogin();
}

void ConfigureWeb::GenerateToken() {
    constexpr size_t length = 48;
    QString set = QStringLiteral("abcdefghijklmnopqrstuvwxyz");
    QString result;

    for (size_t i = 0; i < length; ++i) {
        size_t idx = m_rng->bounded(set.length());
        result.append(set.at(idx));
    }

    ui->edit_token->setText(result);
}

void ConfigureWeb::ApplyConfiguration() {
    Settings::values.eden_username = ui->edit_username->text().toStdString();
    Settings::values.eden_token = ui->edit_token->text().toStdString();
}

void ConfigureWeb::VerifyLogin() {
    const QPixmap checked = QIcon::fromTheme(QStringLiteral("checked")).pixmap(16);
    const QPixmap failed = QIcon::fromTheme(QStringLiteral("failed")).pixmap(16);

    const bool username_good = ui->edit_username->hasAcceptableInput();
    const bool token_good = ui->edit_token->hasAcceptableInput();

    if (username_good) {
        ui->label_username_verified->setPixmap(checked);
        ui->label_username_verified->setToolTip(tr("All Good", "Tooltip"));
    } else {
        ui->label_username_verified->setPixmap(failed);
        ui->label_username_verified->setToolTip(tr("Must be between 4-20 characters", "Tooltip"));
    }

    if (token_good) {
        ui->label_token_verified->setPixmap(checked);
        ui->label_token_verified->setToolTip(tr("All Good", "Tooltip"));
    } else {
        ui->label_token_verified->setPixmap(failed);
        ui->label_token_verified->setToolTip(
            tr("Must be 48 characters, and lowercase a-z", "Tooltip"));
    }
}

void ConfigureWeb::SetWebServiceConfigEnabled(bool enabled) {
    ui->label_disable_info->setVisible(!enabled);
    ui->groupBoxWebConfig->setEnabled(enabled);
}
