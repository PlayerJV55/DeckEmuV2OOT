// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>
#include <QColor>
#include <QCoreApplication>
#include <QFrame>
#include <QPalette>
#include <QRegularExpression>
#include <QScrollArea>
#include <QToolTip>
#include "qt_common/config/uisettings.h"
#include "yuzu/configuration/configuration_shared.h"

namespace ConfigurationShared {

Tab::Tab(std::shared_ptr<std::vector<Tab*>> group, QWidget* parent) : QWidget(parent) {
    if (group != nullptr) {
        group->push_back(this);
    }
}

Tab::~Tab() = default;

QScrollArea* WrapInScrollArea(QWidget* page, QWidget* parent) {
    auto* const scroll_area = new QScrollArea(parent);
    scroll_area->setFrameShape(QFrame::NoFrame);
    scroll_area->setWidgetResizable(true);
    scroll_area->setAccessibleName(page->accessibleName());
    scroll_area->setWidget(page);
    return scroll_area;
}

// Readable on each theme's tooltip background: the qdarkstyle themes paint their own, the native
// style follows the system.
static QColor TipColor() {
    const std::string& theme = UISettings::values.theme;
    if (theme == "qdarkstyle_midnight_blue" || theme == "colorful_midnight_blue") {
        return QColor(0x4a, 0x0d, 0x0d);
    }
    if (UISettings::IsDarkTheme() ||
        QToolTip::palette().color(QPalette::ToolTipBase).lightness() < 128) {
        return QColor(0xff, 0xe5, 0x9a);
    }
    return QColor(0x00, 0x63, 0x9b);
}

QString RichToolTip(const QString& title, const QString& text, const QString& tip) {
    if (text.isEmpty() && tip.isEmpty()) {
        return {};
    }
    const auto to_html = [](const QString& plain) {
        return plain.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    };
    // Labels end in a colon and may carry an accelerator
    QString heading = title.trimmed();
    heading.remove(QRegularExpression(QStringLiteral("&(?!&)")));
    if (heading.endsWith(QLatin1Char(':'))) {
        heading.chop(1);
    }

    QString html = QStringLiteral("<b>%1</b>").arg(to_html(heading));
    if (!text.isEmpty()) {
        html += QStringLiteral("<p style=\"margin-top:4px; margin-bottom:0px\">%1</p>")
                    .arg(to_html(text));
    }
    if (!tip.isEmpty()) {
        const QString line =
            QCoreApplication::translate("ConfigurationShared", "Tip: %1").arg(tip);
        html += QStringLiteral(
                    "<p style=\"margin-top:6px; margin-bottom:0px; color:%1\"><i>%2</i></p>")
                    .arg(TipColor().name(), to_html(line));
    }
    // Left to itself, Qt wraps rich tooltips into tall, thin boxes, some only a few words wide
    const int width = std::clamp(4 * int(text.size() + tip.size()), 220, 440);
    return QStringLiteral("<table width=\"%1\" cellspacing=\"0\" cellpadding=\"0\"><tr><td>%2"
                          "</td></tr></table>")
        .arg(width)
        .arg(html);
}

void SetRichToolTip(QWidget* widget, const QString& title, const QString& text,
                    const QString& tip) {
    widget->setToolTip(RichToolTip(title, text, tip));
}

} // namespace ConfigurationShared
