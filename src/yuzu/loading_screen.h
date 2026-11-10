// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <QFont>
#include <QImage>
#include <QPixmap>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <QtGlobal>

namespace Loader {
class AppLoader;
}

namespace VideoCore {
enum class LoadCallbackStage;
}

class QRegion;

class LoadingScreen : public QWidget {
    Q_OBJECT

public:
    explicit LoadingScreen(QWidget* parent = nullptr);

    ~LoadingScreen() override;

    /// Call before showing the loading screen to load the name, version and cover of the
    /// currently loaded application.
    void Prepare(Loader::AppLoader& loader);

    /// After the application has stopped, the owner of this class can call this to clean up any
    /// used resources such as the cover.
    void Clear();

    /// Slot used to update the status of the progress bar
    void OnLoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total);

    /// Hides the LoadingScreen with a fade out effect
    void OnLoadComplete();

    /// Turns the screen into the one shown while the application closes, with the name and cover
    /// from Prepare. Call before showing it.
    void ShowClosing();

signals:
    void LoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total);
    /// Signals that this widget is completely hidden now and should be replaced with the other
    /// widget
    void Hidden();

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    using Clock = std::chrono::steady_clock;

    /// Milliseconds since Prepare, which every animation is timed from
    double Elapsed(Clock::time_point now = Clock::now()) const;

    void StartAnimating();
    void Animate();
    bool IsAnimating(double t) const;
    QRegion ProgressRegion() const;
    void UpdateLayout(qreal device_pixel_ratio);

    /// The translation of source, or the built-in Portuguese text if Portuguese is shown and
    /// there is no translation

    double FillTarget() const;
    /// When the bar's current sweep began, or -1 while the bar shows progress instead
    double SweepStart(double t) const;
    QString StageText() const;
    QString PercentText() const;
    QString DetailText(double t) const;

    // Set by Prepare
    QString title;
    QString meta;
    QImage cover; ///< Null when the placeholder is shown
    std::string awaited_cover; ///< Box art that replaces the cover once downloaded

    // Drawing resources for the current size
    QSize layout_size;
    qreal layout_device_pixel_ratio = 0;
    QPixmap cover_pixmap;
    QString elided_title;
    QFont title_font;
    QFont meta_font;
    QFont row_font;
    QFont detail_font;

    QTimer animation_timer; ///< Runs only while something moves
    Clock::time_point start_time;
    double prepare_ms = 0;   ///< When preparing began, for the bar's sweep
    double complete_ms = -1; ///< When loading completed, for closing the logo
    double launch_ms = -1;   ///< When the fill gave way to the sweep, as the game starts or closes
    double closing_ms = -1;  ///< When the application began closing
    double fade_in_ms = 0;   ///< When the fade in from black began
    double exit_ms = -1;     ///< When the fade out began

    VideoCore::LoadCallbackStage stage;
    std::size_t value = 0;
    std::size_t total = 0;
    double fill = 0; ///< Shown progress, eased towards the reported one

    // newly generated shaders are added to the end of the file, so when loading and compiling
    // shaders, it will start quickly but end slow if new shaders were added since previous launch.
    // These variables are used to detect the change in speed so we can generate an ETA
    bool slow_shader_compile_start = false;
    Clock::time_point slow_shader_start;
    Clock::time_point previous_time;
    std::size_t slow_shader_first_value = 0;
    double eta_end_ms = -1;       ///< When the build should end, from the latest progress
    double shown_eta_end_ms = -1; ///< eta_end_ms eased, so the estimate does not jump around
};

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
Q_DECLARE_METATYPE(VideoCore::LoadCallbackStage);
#endif
