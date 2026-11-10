// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <QColor>
#include <QFontMetricsF>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QStringList>

#include "core/frontend/framebuffer_layout.h"
#include "core/loader/loader.h"
#include "qt_common/util/boxart.h"
#include "qt_common/util/title.h"
#include "video_core/rasterizer_interface.h"
#include "yuzu/loading_screen.h"

namespace {

using Stage = VideoCore::LoadCallbackStage;

// Everything is laid out on a 1920x1080 frame, scaled to fit the widget and centered.
constexpr qreal FrameWidth = 1920;
constexpr qreal FrameHeight = 1080;

constexpr qreal CoverSize = 280;
constexpr qreal CoverLeft = (FrameWidth - CoverSize) / 2;
constexpr qreal CoverTop = 264;
constexpr qreal CoverRadius = 10;
constexpr qreal CoverRise = 8;
constexpr qreal PlaceholderMarkHeight = 47;

constexpr qreal TextWidth = 900;
constexpr qreal TextLeft = (FrameWidth - TextWidth) / 2;
constexpr qreal TitleTop = 584;
constexpr qreal TitleHeight = 62;
constexpr qreal MetaTop = 654;
constexpr qreal MetaHeight = 26;

// The progress block: stage and percent, the line, then the shader count.
constexpr qreal BarWidth = 600;
constexpr qreal BarLeft = (FrameWidth - BarWidth) / 2;
constexpr qreal BarTop = 742;
constexpr qreal RowHeight = 24;
constexpr qreal LineTop = BarTop + 34;
constexpr qreal LineHeight = 3;
constexpr qreal DetailTop = BarTop + 52;
constexpr qreal DetailHeight = 22;
constexpr qreal SweepWidth = 140;

constexpr qreal BrandLeft = 72;
constexpr qreal BrandTop = 996;
constexpr qreal BrandHeight = 29;
constexpr qreal BrandMarkWidth = 34;
constexpr qreal BrandTextGap = 12;

constexpr QColor BackgroundColor{0x0F, 0x10, 0x12};
constexpr QColor CardColor{0x1A, 0x1B, 0x1F};
constexpr QColor CardBorderColor{0xFF, 0xFF, 0xFF, 15};
constexpr QColor PlaceholderMarkColor{0x3A, 0x3D, 0x44};
constexpr QColor TitleColor{0xF2, 0xF2, 0xF3};
constexpr QColor MetaColor{0x7C, 0x80, 0x89};
constexpr QColor StageColor{0xB5, 0xB8, 0xBF};
constexpr QColor TrackColor{0x26, 0x28, 0x2D};
constexpr QColor FillColor{0xED, 0xEE, 0xF0};
constexpr QColor SweepColor{0xED, 0xEE, 0xF0, 115};
constexpr QColor DetailColor{0x6E, 0x72, 0x7B};
constexpr QColor MarkColor{0x2A, 0x2C, 0x31};
constexpr QColor MarkLeftColor{0x16, 0xC6, 0xA8};
constexpr QColor MarkRightColor{0x2B, 0x6C, 0xFF};

constexpr int TickMs = 33;
constexpr double FadeInMs = 500;
constexpr double IntroMs = 900; ///< When the last element has faded in
constexpr double SweepMs = 1400;
constexpr double LaunchHoldMs = 250; ///< How long the full bar stays before the sweep takes over
constexpr double LaunchFadeMs = 350;
constexpr double LogoCloseMs = 400;
constexpr double ExitMs = 500;

// The brand mark is two halves in a 113x100 box. While loading they sit this far apart.
constexpr qreal MarkGap = 8;
constexpr qreal BrandMarkScale = BrandMarkWidth / (113 + 2 * MarkGap);

struct Frame {
    qreal scale;
    QPointF origin;

    QPointF Point(qreal x, qreal y) const {
        return origin + QPointF{x, y} * scale;
    }

    QRectF Rect(qreal x, qreal y, qreal width, qreal height) const {
        return {Point(x, y), QSizeF{width, height} * scale};
    }
};

Frame MakeFrame(const QSize& size) {
    const qreal scale = std::min(size.width() / FrameWidth, size.height() / FrameHeight);
    return {scale,
            {(size.width() - FrameWidth * scale) / 2, (size.height() - FrameHeight * scale) / 2}};
}

double Segment(double t, double begin, double end) {
    return std::clamp((t - begin) / (end - begin), 0.0, 1.0);
}

double OutCubic(double x) {
    return 1 - std::pow(1 - x, 3);
}

double InOutCubic(double x) {
    return x < 0.5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2;
}

qreal Snap(qreal value, qreal device_pixel_ratio) {
    return std::round(value * device_pixel_ratio) / device_pixel_ratio;
}

QPainterPath MarkHalf(bool right) {
    QPainterPath path;
    if (right) {
        path.moveTo(63.5, 0);
        path.lineTo(89, 0);
        path.arcTo(65, 0, 48, 48, 90, -90);
        path.lineTo(113, 76);
        path.arcTo(65, 52, 48, 48, 0, -90);
        path.lineTo(63.5, 100);
        path.quadTo(61, 100, 61, 97.5);
        path.lineTo(61, 2.5);
        path.quadTo(61, 0, 63.5, 0);
    } else {
        path.moveTo(24, 0);
        path.lineTo(49.5, 0);
        path.quadTo(52, 0, 52, 2.5);
        path.lineTo(52, 97.5);
        path.quadTo(52, 100, 49.5, 100);
        path.lineTo(24, 100);
        path.arcTo(0, 52, 48, 48, 270, -90);
        path.lineTo(0, 24);
        path.arcTo(0, 0, 48, 48, 180, -90);
    }
    path.closeSubpath();
    return path;
}

QFont MakeFont(const QWidget& widget, QStringList families, qreal pixel_size, QFont::Weight weight,
               bool tabular_figures) {
    QFont font = widget.font();
    // The widget's font is the last fallback, for systems without these.
    families.append(font.family());
    font.setFamilies(families);
    // Set as a point size, since pixel sizes cannot be fractional.
    font.setPointSizeF(pixel_size * 72 / widget.logicalDpiY());
    font.setWeight(weight);
    font.setHintingPreference(QFont::PreferVerticalHinting);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    if (tabular_figures) {
        font.setFeature(QFont::Tag("tnum"), 1);
    }
#endif
    return font;
}

// The cover with rounded corners and its border, size pixels wide.
QPixmap RenderCover(const QImage& image, int size, qreal radius, qreal border) {
    QImage canvas(size, size, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath card;
    card.addRoundedRect(QRectF(0, 0, size, size), radius, radius);
    painter.fillPath(card, CardColor);

    // Only drawn where the card is, which rounds the image's corners.
    painter.setCompositionMode(QPainter::CompositionMode_SourceAtop);
    if (!image.isNull()) {
        const QImage scaled =
            image.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        painter.drawImage(QPoint((size - scaled.width()) / 2, (size - scaled.height()) / 2),
                          scaled);
    } else {
        const qreal unit = size / CoverSize * PlaceholderMarkHeight / 100;
        painter.translate((size - 113 * unit) / 2, (size - 100 * unit) / 2);
        painter.scale(unit, unit);
        painter.fillPath(MarkHalf(false), PlaceholderMarkColor);
        painter.fillPath(MarkHalf(true), PlaceholderMarkColor);
        painter.resetTransform();
    }

    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setPen(QPen(CardBorderColor, border));
    painter.setBrush(Qt::NoBrush);
    const qreal inset = border / 2;
    painter.drawRoundedRect(QRectF(inset, inset, size - border, size - border), radius - inset,
                            radius - inset);
    painter.end();
    return QPixmap::fromImage(std::move(canvas));
}

} // Anonymous namespace

LoadingScreen::LoadingScreen(QWidget* parent) : QWidget(parent), stage(Stage::Complete) {
    setMinimumSize(Layout::MinimumSize::Width, Layout::MinimumSize::Height);
    // Every paint covers the whole widget.
    setAttribute(Qt::WA_OpaquePaintEvent);

    animation_timer.setTimerType(Qt::PreciseTimer);
    animation_timer.setInterval(TickMs);
    connect(&animation_timer, &QTimer::timeout, this, &LoadingScreen::Animate);

    connect(this, &LoadingScreen::LoadProgress, this, &LoadingScreen::OnLoadProgress,
            Qt::QueuedConnection);
    qRegisterMetaType<VideoCore::LoadCallbackStage>();
}

LoadingScreen::~LoadingScreen() {
    QtCommon::Boxart::OnSaved(this, {});
}

void LoadingScreen::Prepare(Loader::AppLoader& loader) {
    u64 program_id = 0;
    if (loader.ReadProgramId(program_id) != Loader::ResultStatus::Success) {
        program_id = 0;
    }
    const std::string name = QtCommon::Title::GetName(loader);
    title = QString::fromStdString(name);

    const QString platform =
        (program_id >> 56) == 0x04 ? QStringLiteral("Switch 2") : QStringLiteral("Switch");
    QString version = QString::fromStdString(QtCommon::Title::GetDisplayVersion(loader));
    if (!version.isEmpty() && !version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) {
        version.prepend(QLatin1Char('v'));
    }
    meta = version.isEmpty() ? platform : QStringLiteral("%1  ·  %2").arg(platform, version);

    // Only local images, the network is never waited on here. Missing box art is downloaded and
    // shown once it arrives.
    QtCommon::Boxart::OnSaved(this, {});
    awaited_cover.clear();
    cover = QtCommon::Boxart::LoadCachedCover(program_id, name);
    if (cover.isNull()) {
        std::vector<u8> icon;
        if (loader.ReadIcon(icon) == Loader::ResultStatus::Success && !icon.empty()) {
            cover.loadFromData(icon.data(), static_cast<int>(icon.size()));
        }
        awaited_cover = QtCommon::Boxart::GetSlug(program_id, name);
        if (!awaited_cover.empty()) {
            QtCommon::Boxart::OnSaved(this, [this, program_id, name](const std::string& slug) {
                if (slug != awaited_cover) {
                    return;
                }
                const QImage saved = QtCommon::Boxart::LoadCachedCover(program_id, name);
                if (saved.isNull()) {
                    return;
                }
                cover = saved;
                awaited_cover.clear();
                QtCommon::Boxart::OnSaved(this, {});
                layout_size = {};
                update();
            });
        }
        QtCommon::Boxart::Prefetch(program_id, name, true);
    }
    layout_size = {};

    start_time = Clock::now();
    stage = Stage::Prepare;
    value = 0;
    total = 0;
    fill = 0;
    prepare_ms = 0;
    complete_ms = -1;
    launch_ms = -1;
    closing_ms = -1;
    fade_in_ms = 0;
    exit_ms = -1;
    slow_shader_compile_start = false;
    previous_time = start_time;
    eta_end_ms = -1;
    shown_eta_end_ms = -1;

    animation_timer.stop();
    StartAnimating();
    update();
}

void LoadingScreen::OnLoadComplete() {
    // A closing application stays on this screen until it has stopped.
    if (exit_ms >= 0 || closing_ms >= 0) {
        return;
    }
    exit_ms = Elapsed();
    if (complete_ms < 0) {
        complete_ms = exit_ms;
    }
    StartAnimating();
}

void LoadingScreen::ShowClosing() {
    if (!isVisible() || exit_ms >= 0) {
        // Shown whole at once, fading in from black like the loading did.
        start_time = Clock::now() - std::chrono::milliseconds(static_cast<int>(IntroMs));
        stage = Stage::Complete;
        fill = 1;
        complete_ms = 0;
        launch_ms = 0;
        fade_in_ms = IntroMs;
        exit_ms = -1;
    }
    closing_ms = Elapsed();
    // A fill still showing fades out as it does when the game starts, and a running sweep carries on.
    if (launch_ms < 0 || launch_ms > closing_ms) {
        launch_ms = closing_ms;
    }
    StartAnimating();
    update();
}

void LoadingScreen::OnLoadProgress(VideoCore::LoadCallbackStage new_stage, std::size_t new_value,
                                   std::size_t new_total) {
    using namespace std::chrono;
    const auto now = Clock::now();
    if (new_stage != stage) {
        stage = new_stage;
        launch_ms = -1;
        if (stage == Stage::Prepare) {
            prepare_ms = Elapsed(now);
            complete_ms = -1;
            fill = 0;
        } else if (stage == Stage::Complete) {
            complete_ms = Elapsed(now);
            // With nothing filled, preparing's sweep carries on and the fill never shows.
            if (fill == 0) {
                launch_ms = prepare_ms;
            }
        }
        // reset back to fast shader compiling since the stage changed, and don't take the time
        // spent in the previous stage for a slowdown
        slow_shader_compile_start = false;
        previous_time = now;
        eta_end_ms = -1;
        shown_eta_end_ms = -1;
    }
    value = new_value;
    total = new_total;

    // If there's a drastic slowdown in the rate, then display an estimate
    if (stage == Stage::Build &&
        (now - previous_time > milliseconds{50} || slow_shader_compile_start)) {
        if (!slow_shader_compile_start) {
            slow_shader_start = now;
            slow_shader_compile_start = true;
            slow_shader_first_value = value;
        }
        // only calculate an estimate time after a second has passed since the slowdown, and
        // once shaders were built since
        const auto diff = duration_cast<milliseconds>(now - slow_shader_start);
        if (diff > seconds{1} && value > slow_shader_first_value && total >= value) {
            const double remaining_ms = static_cast<double>(total - value) /
                                        static_cast<double>(value - slow_shader_first_value) *
                                        static_cast<double>(diff.count());
            eta_end_ms = Elapsed(now) + remaining_ms + 1000;
        }
    }
    previous_time = now;

    StartAnimating();
    if (!animation_timer.isActive()) {
        update(ProgressRegion());
    }
}

void LoadingScreen::Clear() {
    QtCommon::Boxart::OnSaved(this, {});
    awaited_cover.clear();
    animation_timer.stop();
    closing_ms = -1;
    exit_ms = -1;
    cover = {};
    cover_pixmap = {};
    layout_size = {};
}

double LoadingScreen::Elapsed(Clock::time_point now) const {
    return std::chrono::duration<double, std::milli>(now - start_time).count();
}

void LoadingScreen::StartAnimating() {
    if (!animation_timer.isActive() && IsAnimating(Elapsed())) {
        animation_timer.start();
    }
}

bool LoadingScreen::IsAnimating(double t) const {
    if (exit_ms >= 0) {
        // Finishes even when hidden, so Hidden is always sent.
        return true;
    }
    if (!isVisible()) {
        return false;
    }
    // Past the intro, outside of building shaders a sweep always runs. While building only the
    // eased fill and the estimate move.
    return t < IntroMs || stage != Stage::Build || closing_ms >= 0 || fill != FillTarget() ||
           eta_end_ms >= 0;
}

void LoadingScreen::Animate() {
    const double t = Elapsed();
    if (exit_ms >= 0 && t >= exit_ms + ExitMs) {
        animation_timer.stop();
        exit_ms = -1;
        hide();
        emit Hidden();
        return;
    }

    const double target = FillTarget();
    fill += (target - fill) * 0.15;
    if (std::abs(target - fill) < 0.001) {
        fill = target;
    }
    if (stage == Stage::Complete && launch_ms < 0 && fill == target) {
        // The full bar shows for a moment, then the sweep runs until the game starts.
        launch_ms = t + LaunchHoldMs;
    }
    if (eta_end_ms >= 0) {
        shown_eta_end_ms = shown_eta_end_ms < 0
                               ? eta_end_ms
                               : shown_eta_end_ms + (eta_end_ms - shown_eta_end_ms) * 0.1;
    }

    // Past the intro and fades only the progress and the brand mark change.
    if (t < IntroMs + TickMs || t < fade_in_ms + FadeInMs + TickMs || exit_ms >= 0) {
        update();
    } else {
        update(ProgressRegion());
    }
    if (!IsAnimating(t)) {
        animation_timer.stop();
    }
}

QRegion LoadingScreen::ProgressRegion() const {
    const Frame frame = MakeFrame(size());
    const auto padded = [](const QRectF& rect) {
        return rect.toAlignedRect().adjusted(-2, -2, 2, 2);
    };
    QRegion region{
        padded(frame.Rect(BarLeft, BarTop, BarWidth, DetailTop + DetailHeight - BarTop))};
    region += padded(frame.Rect(BrandLeft - MarkGap * BrandMarkScale, BrandTop, 240, BrandHeight));
    return region;
}

void LoadingScreen::UpdateLayout(qreal device_pixel_ratio) {
    const qreal scale = MakeFrame(size()).scale;
    // Windows lists the variable font's weights as families of their own.
    const QStringList display_families{QStringLiteral("Segoe UI Variable Display Semibold"),
                                       QStringLiteral("Segoe UI Variable Display"),
                                       QStringLiteral("Segoe UI")};
    const QStringList text_families{QStringLiteral("Segoe UI")};

    title_font = MakeFont(*this, display_families, 52 * scale, QFont::DemiBold, false);
    title_font.setLetterSpacing(QFont::AbsoluteSpacing, -0.4 * scale);
    meta_font = MakeFont(*this, text_families, 19 * scale, QFont::Normal, false);
    row_font = MakeFont(*this, text_families, 17 * scale, QFont::Normal, true);
    detail_font = MakeFont(*this, text_families, 16 * scale, QFont::Normal, true);
    elided_title =
        QFontMetricsF(title_font, this).elidedText(title, Qt::ElideRight, TextWidth * scale);

    const int cover_size = std::max(1, qRound(CoverSize * scale * device_pixel_ratio));
    cover_pixmap = RenderCover(cover, cover_size, CoverRadius * scale * device_pixel_ratio,
                               std::max<qreal>(1, scale * device_pixel_ratio));
    cover_pixmap.setDevicePixelRatio(device_pixel_ratio);

    layout_size = size();
    layout_device_pixel_ratio = device_pixel_ratio;
}

double LoadingScreen::FillTarget() const {
    switch (stage) {
    case Stage::Build:
        return total == 0 ? 0 : std::min(1.0, static_cast<double>(value) / total);
    case Stage::Complete:
        return 1;
    default:
        return 0;
    }
}

double LoadingScreen::SweepStart(double t) const {
    if (stage == Stage::Prepare) {
        return prepare_ms;
    }
    if ((stage == Stage::Complete || closing_ms >= 0) && launch_ms >= 0 && t >= launch_ms) {
        return launch_ms;
    }
    return -1;
}

QString LoadingScreen::StageText() const {
    if (closing_ms >= 0) {
        return tr("Closing");
    }
    switch (stage) {
    case Stage::Prepare:
        return tr("Preparing");
    case Stage::Build:
        return tr("Loading shaders");
    default:
        return tr("Starting");
    }
}

QString LoadingScreen::PercentText() const {
    std::size_t percent = 100;
    switch (stage) {
    case Stage::Prepare:
        return {};
    case Stage::Build:
        // Never 100% before loading is really done.
        percent = total == 0 ? 0 : std::min<std::size_t>(99, value * 100 / total);
        break;
    default:
        break;
    }
    return QStringLiteral("%1%").arg(percent);
}

QString LoadingScreen::DetailText(double t) const {
    if (stage != Stage::Build || total == 0) {
        return {};
    }
    const QLocale locale;
    const QString count = tr("%1 of %2")
                              .arg(locale.toString(static_cast<qulonglong>(value)),
                                   locale.toString(static_cast<qulonglong>(total)));

    QString note;
    if (!slow_shader_compile_start) {
        note = tr("reading from cache");
    } else if (shown_eta_end_ms >= 0) {
        const double seconds = std::ceil((shown_eta_end_ms - t) / 1000);
        if (seconds < 5) {
            note = tr("almost there");
        } else if (seconds < 60) {
            note = tr("about %1 s").arg(static_cast<int>(seconds));
        } else {
            note = tr("about %1 min").arg(qRound(seconds / 60));
        }
    }
    return note.isEmpty() ? count : QStringLiteral("%1 · %2").arg(count, note);
}

void LoadingScreen::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    StartAnimating();
}

void LoadingScreen::paintEvent(QPaintEvent*) {
    const qreal device_pixel_ratio = devicePixelRatioF();
    if (layout_size != size() || layout_device_pixel_ratio != device_pixel_ratio) {
        UpdateLayout(device_pixel_ratio);
    }
    const Frame frame = MakeFrame(size());
    const qreal scale = frame.scale;
    const double t = Elapsed();

    QPainter painter(this);
    painter.fillRect(rect(), BackgroundColor);
    painter.setRenderHint(QPainter::Antialiasing);

    const double cover_in = OutCubic(Segment(t, 100, 700));
    if (cover_in > 0) {
        painter.setOpacity(cover_in);
        const QPointF position = frame.Point(CoverLeft, CoverTop + CoverRise * (1 - cover_in));
        painter.drawPixmap(
            QPointF(Snap(position.x(), device_pixel_ratio), Snap(position.y(), device_pixel_ratio)),
            cover_pixmap);
    }

    const double title_in = OutCubic(Segment(t, 250, 800));
    if (title_in > 0) {
        painter.setOpacity(title_in);
        painter.setFont(title_font);
        painter.setPen(TitleColor);
        painter.drawText(frame.Rect(TextLeft, TitleTop, TextWidth, TitleHeight),
                         Qt::AlignCenter | Qt::TextSingleLine, elided_title);
        painter.setFont(meta_font);
        painter.setPen(MetaColor);
        painter.drawText(frame.Rect(TextLeft, MetaTop, TextWidth, MetaHeight),
                         Qt::AlignCenter | Qt::TextSingleLine, meta);
    }

    const double progress_in = OutCubic(Segment(t, 400, IntroMs));
    if (progress_in > 0) {
        painter.setOpacity(progress_in);
        // The fill, the percent and the shader count fade out once the sweep takes over.
        const double fill_opacity =
            launch_ms >= 0 ? 1 - Segment(t, launch_ms, launch_ms + LaunchFadeMs) : 1;

        const QRectF row = frame.Rect(BarLeft, BarTop, BarWidth, RowHeight);
        painter.setFont(row_font);
        painter.setPen(StageColor);
        painter.drawText(row, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, StageText());
        if (fill_opacity > 0) {
            painter.setOpacity(progress_in * fill_opacity);
            painter.drawText(row, Qt::AlignRight | Qt::AlignVCenter | Qt::TextSingleLine,
                             PercentText());
            painter.setOpacity(progress_in);
        }

        // Kept on whole pixels so the line stays sharp at any scale.
        const QRectF line = frame.Rect(BarLeft, LineTop, BarWidth, LineHeight);
        const qreal line_top = Snap(line.top(), device_pixel_ratio);
        const qreal line_height =
            std::max<qreal>(1, std::round(line.height() * device_pixel_ratio)) / device_pixel_ratio;
        painter.fillRect(QRectF(line.left(), line_top, line.width(), line_height), TrackColor);
        if (fill > 0 && fill_opacity > 0) {
            painter.setOpacity(progress_in * fill_opacity);
            painter.fillRect(QRectF(line.left(), line_top, line.width() * fill, line_height),
                             FillColor);
            painter.setOpacity(progress_in);
        }
        if (const double sweep_start = SweepStart(t); sweep_start >= 0) {
            const double phase = std::fmod(t - sweep_start, SweepMs) / SweepMs;
            const double sweep = -SweepWidth + phase * (BarWidth + SweepWidth);
            const double begin = std::max(sweep, 0.0);
            const double end = std::min<double>(sweep + SweepWidth, BarWidth);
            if (end > begin) {
                painter.fillRect(QRectF(line.left() + begin * scale, line_top,
                                        (end - begin) * scale, line_height),
                                 SweepColor);
            }
        }

        painter.setOpacity(progress_in * fill_opacity);
        painter.setFont(detail_font);
        painter.setPen(DetailColor);
        painter.drawText(frame.Rect(BarLeft, DetailTop, BarWidth, DetailHeight),
                         Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, DetailText(t));
    }
    painter.setOpacity(1);

    // The brand mark fills with the progress, its halves close up once loading is done, and they
    // part again when closing.
    double joined =
        complete_ms >= 0 ? OutCubic(Segment(t, complete_ms, complete_ms + LogoCloseMs)) : 0;
    if (closing_ms >= 0) {
        joined *= 1 - OutCubic(Segment(t, closing_ms, closing_ms + LogoCloseMs));
    }
    const qreal gap = MarkGap * (1 - joined);
    const qreal level = stage == Stage::Prepare ? 0 : fill;
    painter.save();
    painter.translate(frame.Point(BrandLeft + MarkGap * BrandMarkScale,
                                  BrandTop + (BrandHeight - 100 * BrandMarkScale) / 2));
    painter.scale(BrandMarkScale * scale, BrandMarkScale * scale);
    for (const bool right : {false, true}) {
        const QPainterPath half = MarkHalf(right).translated(right ? gap : -gap, 0);
        painter.fillPath(half, MarkColor);
        const QColor color = right ? MarkRightColor : MarkLeftColor;
        if (level >= 1) {
            painter.fillPath(half, color);
        } else if (level > 0) {
            QPainterPath filled;
            filled.addRect(half.boundingRect().adjusted(0, 100 * (1 - level), 0, 0));
            painter.fillPath(half.intersected(filled), color);
        }
    }
    painter.restore();
    painter.setFont(row_font);
    painter.setPen(DetailColor);
    painter.drawText(
        frame.Rect(BrandLeft + BrandMarkWidth + BrandTextGap, BrandTop, 200, BrandHeight),
        Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, QStringLiteral("Nemyu"));

    // Fades in from black, and back to black on exit.
    double veil = 1 - OutCubic(Segment(t, fade_in_ms, fade_in_ms + FadeInMs));
    if (exit_ms >= 0) {
        veil += InOutCubic(Segment(t, exit_ms, exit_ms + ExitMs));
    }
    if (veil > 0) {
        painter.fillRect(rect(), QColor(0, 0, 0, qRound(std::min(veil, 1.0) * 255)));
    }
}
