// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>
#include <utility>
#include <QIconEngine>
#include <QImageReader>
#include <QMenu>
#include <QPainter>
#include <QPixmapCache>
#include "yuzu/util/icons.h"

namespace Icons {
namespace {

struct Colors {
    QColor normal;
    QColor disabled;
};

std::optional<Colors> colors;

const Colors& CurrentColors() {
    if (!colors) {
        // The dark themes set the text colour in their style sheet, which only reaches the
        // palette of a polished widget
        QMenu probe;
        probe.ensurePolished();
        const QPalette& palette = probe.palette();
        const QColor normal = palette.color(QPalette::Active, QPalette::WindowText);
        QColor disabled = palette.color(QPalette::Disabled, QPalette::WindowText);
        // The style sheets give no colour of their own to disabled widgets
        if (disabled == normal) {
            disabled.setAlphaF(0.4f);
        }
        colors = Colors{normal, disabled};
    }
    return *colors;
}

class LineIconEngine final : public QIconEngine {
public:
    explicit LineIconEngine(QString path_) : path{std::move(path_)} {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override {
        const qreal scale = painter->device()->devicePixelRatio();
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, scale));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State,
                         qreal scale) override {
        const Colors& theme = CurrentColors();
        const QColor& color = mode == QIcon::Disabled ? theme.disabled : theme.normal;
        const int pixels = qRound(qMin(size.width(), size.height()) * scale);
        const QString key = QStringLiteral("line_icon/%1/%2/%3/%4")
                                .arg(path)
                                .arg(pixels)
                                .arg(scale)
                                .arg(color.rgba(), 8, 16, QLatin1Char('0'));

        QPixmap pixmap;
        if (QPixmapCache::find(key, &pixmap)) {
            return pixmap;
        }

        QImageReader reader(path);
        reader.setScaledSize(QSize(pixels, pixels));
        QImage image = reader.read().convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (!image.isNull()) {
            QPainter painter(&image);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(image.rect(), color);
        }
        pixmap = QPixmap::fromImage(std::move(image));
        pixmap.setDevicePixelRatio(scale);
        QPixmapCache::insert(key, pixmap);
        return pixmap;
    }

    QIconEngine* clone() const override {
        return new LineIconEngine(path);
    }

private:
    QString path;
};

} // Anonymous namespace

QIcon Get(const char* name) {
    return QIcon(new LineIconEngine(QStringLiteral(":/line_icons/%1.svg").arg(QLatin1String(name))));
}

QIcon ForSettingsSection(SettingsSection section) {
    switch (section) {
    case SettingsSection::General:
        return Get("settings");
    case SettingsSection::System:
        return Get("world-cog");
    case SettingsSection::Emulation:
        return Get("cpu");
    case SettingsSection::Graphics:
        return Get("photo");
    case SettingsSection::Switch2:
        return Get("device-nintendo");
    case SettingsSection::Audio:
        return Get("volume");
    case SettingsSection::Controls:
        return Get("device-gamepad-2");
    case SettingsSection::Hotkeys:
        return Get("keyboard");
    case SettingsSection::Network:
        return Get("network");
    case SettingsSection::Debug:
        return Get("bug");
    case SettingsSection::AddOns:
        return Get("puzzle");
    }
    return {};
}

void UpdateColors() {
    colors.reset();
}

} // namespace Icons
