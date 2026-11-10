// SPDX-FileCopyrightText: 2016 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>
#include <QString>
#include <QWidget>
#include <qobjectdefs.h>

class QObject;
class QScrollArea;

namespace ConfigurationShared {

class Tab : public QWidget {
    Q_OBJECT

public:
    explicit Tab(std::shared_ptr<std::vector<Tab*>> group, QWidget* parent = nullptr);
    ~Tab();

    virtual void ApplyConfiguration() = 0;
    virtual void SetConfiguration() = 0;
};

/// Puts a settings page in a frameless, resizable scroll area that copies its accessibleName.
/// The page is reparented into the scroll area.
[[nodiscard]] QScrollArea* WrapInScrollArea(QWidget* page, QWidget* parent);

/// Tooltip with the option's title in bold, its explanation, then the tip in italics in the
/// theme's link colour. Empty when there is neither an explanation nor a tip.
[[nodiscard]] QString RichToolTip(const QString& title, const QString& text,
                                  const QString& tip = {});

void SetRichToolTip(QWidget* widget, const QString& title, const QString& text,
                    const QString& tip = {});

} // namespace ConfigurationShared
