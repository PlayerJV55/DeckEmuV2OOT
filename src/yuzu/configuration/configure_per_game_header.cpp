// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QResizeEvent>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "yuzu/configuration/configure_per_game_header.h"

namespace {
constexpr int CoverSize = 96;
} // Anonymous namespace

/// A label that shortens its text to its width, with the full text as tooltip when shortened.
class ConfigurePerGameHeader::ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(Qt::TextElideMode mode_, QWidget* parent) : QLabel(parent), mode{mode_} {
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    void SetFullText(const QString& text) {
        full_text = text;
        UpdateText();
    }

    QSize sizeHint() const override {
        return {fontMetrics().horizontalAdvance(full_text), QLabel::sizeHint().height()};
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        UpdateText();
    }

    void changeEvent(QEvent* event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange) {
            UpdateText();
        }
    }

private:
    void UpdateText() {
        const QString shown = fontMetrics().elidedText(full_text, mode, width());
        setText(shown);
        setToolTip(shown == full_text ? QString() : full_text);
    }

    Qt::TextElideMode mode;
    QString full_text;
};

ConfigurePerGameHeader::ConfigurePerGameHeader(QWidget* parent) : QWidget(parent) {
    auto* const layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    cover_label = new QLabel(this);
    cover_label->setFixedSize(CoverSize, CoverSize);
    cover_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(cover_label, 0, Qt::AlignTop);

    auto* const info_layout = new QVBoxLayout;
    info_layout->setContentsMargins(0, 0, 0, 0);
    info_layout->setSpacing(2);
    layout->addLayout(info_layout, 1);
    layout->setAlignment(info_layout, Qt::AlignTop);

    name_label = new ElidedLabel(Qt::ElideRight, this);
    QFont name_font = name_label->font();
    if (name_font.pointSizeF() > 0) {
        name_font.setPointSizeF(name_font.pointSizeF() * 1.3);
    }
    name_font.setBold(true);
    name_label->setFont(name_font);
    info_layout->addWidget(name_label);

    developer_label = new QLabel(this);
    developer_label->setTextFormat(Qt::PlainText);
    info_layout->addWidget(developer_label);
    info_layout->addSpacing(4);

    auto* const details = new QGridLayout;
    details->setContentsMargins(0, 0, 0, 0);
    details->setHorizontalSpacing(8);
    details->setVerticalSpacing(2);
    info_layout->addLayout(details);

    const auto make_label = [this](bool selectable) {
        auto* const label = new QLabel(this);
        label->setTextFormat(Qt::PlainText);
        if (selectable) {
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        }
        return label;
    };

    version_key = make_label(false);
    version_label = make_label(true);
    format_key = make_label(false);
    format_label = make_label(true);
    title_id_key = make_label(false);
    title_id_label = make_label(true);
    size_key = make_label(false);
    size_label = make_label(true);
    path_key = make_label(false);

    copy_title_id_button = new QToolButton(this);
    connect(copy_title_id_button, &QToolButton::clicked, this,
            [this] { QGuiApplication::clipboard()->setText(title_id_label->text()); });
    auto* const title_id_row = new QHBoxLayout;
    title_id_row->setSpacing(6);
    title_id_row->addWidget(title_id_label);
    title_id_row->addWidget(copy_title_id_button);
    title_id_row->addStretch(1);

    path_label = new ElidedLabel(Qt::ElideMiddle, this);
    open_location_button = new QPushButton(this);
    connect(open_location_button, &QPushButton::clicked, this,
            &ConfigurePerGameHeader::OpenFileLocation);
    copy_path_button = new QPushButton(this);
    connect(copy_path_button, &QPushButton::clicked, this,
            [this] { QGuiApplication::clipboard()->setText(path); });
    auto* const path_buttons = new QHBoxLayout;
    path_buttons->setSpacing(6);
    path_buttons->addWidget(open_location_button);
    path_buttons->addWidget(copy_path_button);

    // Version and format, then title ID and size, then the file across the width
    details->addWidget(version_key, 0, 0);
    details->addWidget(version_label, 0, 1);
    details->addWidget(title_id_key, 0, 3);
    details->addLayout(title_id_row, 0, 4);
    details->addWidget(format_key, 1, 0);
    details->addWidget(format_label, 1, 1);
    details->addWidget(size_key, 1, 3);
    details->addWidget(size_label, 1, 4);
    details->addWidget(path_key, 2, 0);
    details->addWidget(path_label, 2, 1, 1, 4);
    details->addLayout(path_buttons, 2, 5);
    details->setColumnMinimumWidth(2, 16);
    details->setColumnStretch(4, 1);

    RetranslateUI();
}

ConfigurePerGameHeader::~ConfigurePerGameHeader() = default;

void ConfigurePerGameHeader::SetInfo(const Info& info) {
    path = info.path;

    if (info.cover.isNull()) {
        cover_label->clear();
    } else {
        const qreal ratio = devicePixelRatioF();
        QPixmap cover = info.cover.scaled(QSize(CoverSize, CoverSize) * ratio, Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);
        cover.setDevicePixelRatio(ratio);
        cover_label->setPixmap(cover);
    }

    name_label->SetFullText(info.name);
    developer_label->setText(info.developer);
    developer_label->setVisible(!info.developer.isEmpty());
    version_label->setText(info.version);
    title_id_label->setText(info.title_id);
    format_label->setText(info.format);
    size_label->setText(info.size);
    path_label->SetFullText(QDir::toNativeSeparators(path));

    open_location_button->setEnabled(!path.isEmpty() && QFileInfo::exists(path));
    copy_path_button->setEnabled(!path.isEmpty());
}

void ConfigurePerGameHeader::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void ConfigurePerGameHeader::RetranslateUI() {
    version_key->setText(tr("Version:"));
    title_id_key->setText(tr("Title ID:"));
    copy_title_id_button->setText(tr("Copy"));
    copy_title_id_button->setToolTip(tr("Copy the title ID"));
    format_key->setText(tr("Format:"));
    size_key->setText(tr("Size:"));
    path_key->setText(tr("File:"));
    open_location_button->setText(tr("Open File Location"));
    open_location_button->setToolTip(tr("Open the folder that holds the game file"));
    copy_path_button->setText(tr("Copy Path"));
    copy_path_button->setToolTip(tr("Copy the full path of the game file"));
}

void ConfigurePerGameHeader::OpenFileLocation() {
    const QFileInfo info(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(info.isDir() ? path : info.absolutePath()));
}
