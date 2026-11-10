// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The skip/remind buttons, the SHA-256 check and the download retries are adapted from Lichii
// #504 and #1101 (zortos293/Lichii).

#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <qdesktopservices.h>
#include "common/logging.h"
#include "frontend_common/update_checker.h"
#include "qt_common/abstract/frontend.h"
#include "qt_common/abstract/progress.h"
#include "ui_update_dialog.h"
#include "update_dialog.h"

#include "common/httplib.h"

#ifdef YUZU_BUNDLED_OPENSSL
#include <openssl/cert.h>
#endif

#include <QDesktopServices>

#undef GetSaveFileName

UpdateDialog::UpdateDialog(const Common::Net::Release& release, QWidget* parent)
    : QDialog(parent), ui(new Ui::UpdateDialog) {
    ui->setupUi(this);
    ui->groupBox->setHidden(true);

    ui->version->setText(
        tr("%1 is available for download.").arg(QString::fromStdString(release.title)));
    ui->url->setText(
        tr("<a href=\"%1\">View on GitLab</a>").arg(QString::fromStdString(release.html_url)));
    ui->body->setMarkdown(QString::fromStdString(release.body));

    ui->buttonBox->button(QDialogButtonBox::No)->setText(tr("Remind Me Later"));
    auto* skip = ui->buttonBox->addButton(tr("Skip This Version"), QDialogButtonBox::ActionRole);
    connect(skip, &QPushButton::clicked, this, [this] { done(SkipVersion); });

    // Without a package for this platform or its published digest, nothing can be verified, so
    // the release page opens instead.
    const auto package = UpdateChecker::FindPackage(release, UpdateChecker::CurrentPlatform());
    const auto checksum = package ? UpdateChecker::FindChecksum(release, *package) : std::nullopt;
    if (!package || !checksum) {
        ui->buttonBox->button(QDialogButtonBox::Yes)->setText(tr("Open Release Page"));
        connect(this, &QDialog::accepted, this, [url = release.html_url] {
            QDesktopServices::openUrl(QUrl{QString::fromStdString(url)});
        });
        return;
    }

    ui->buttonBox->button(QDialogButtonBox::Yes)->setText(tr("Download"));
    m_asset = *package;
    m_checksum = *checksum;
    connect(this, &QDialog::accepted, this, &UpdateDialog::Download);
}

UpdateDialog::~UpdateDialog() {
    delete ui;
}

void UpdateDialog::Download() {
    const auto checksum_file = Common::Net::MakeRequest(m_checksum.url, m_checksum.path);
    const auto expected =
        checksum_file ? UpdateChecker::ParseChecksum(*checksum_file, m_asset.filename)
                      : std::nullopt;
    if (!expected) {
        LOG_ERROR(Frontend, "Could not read the SHA-256 of {} from {}", m_asset.filename,
                  m_checksum.path);
        QtCommon::Frontend::Critical(tr("Failed to download file"),
                                     tr("Could not get the checksum GitLab published for %1, so "
                                        "the download was not started. Try again later.")
                                         .arg(QString::fromStdString(m_asset.filename)));
        return;
    }

    auto default_dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (default_dir.isEmpty()) {
        default_dir = QDir::homePath();
    }
    const auto filename = QtCommon::Frontend::GetSaveFileName(
        tr("New Version Location"),
        default_dir % QStringLiteral("/") % QString::fromStdString(m_asset.filename),
        tr("All Files (*.*)"));

    if (filename.isEmpty())
        return;

    QSaveFile file(filename);
    if (!file.open(QIODevice::Truncate | QIODevice::WriteOnly)) {
        LOG_WARNING(Frontend, "Could not open file {}", filename.toStdString());
        QtCommon::Frontend::Critical(tr("Failed to save file"),
                                     tr("Could not open file %1 for writing.").arg(filename));
        return;
    }

    constexpr std::size_t timeout_seconds = 15;

    std::unique_ptr<httplib::Client> client = std::make_unique<httplib::Client>(m_asset.url);
    client->set_connection_timeout(timeout_seconds);
    client->set_read_timeout(timeout_seconds);
    client->set_write_timeout(timeout_seconds);
    client->set_follow_location(true);

#ifdef YUZU_BUNDLED_OPENSSL
    client->load_ca_cert_store(kCert, sizeof(kCert));
#endif

    auto progress =
        QtCommon::Frontend::newProgressDialog(tr("Downloading..."), tr("Cancel"), 0, 100);
    progress->show();

    QGuiApplication::processEvents();

    auto progress_callback = [&](size_t processed_size, size_t total_size) {
        QGuiApplication::processEvents();
        if (total_size != 0) {
            progress->setValue(static_cast<int>((processed_size * 100) / total_size));
        }
        return !progress->wasCanceled();
    };

    QCryptographicHash hash(QCryptographicHash::Sha256);
    bool write_failed = false;
    auto content_receiver = [&](const char* t_data, size_t data_length) -> bool {
        hash.addData(QByteArrayView(t_data, static_cast<qsizetype>(data_length)));
        if (file.write(t_data, data_length) == -1) {
            LOG_WARNING(Frontend, "Could not write {} bytes to file {}", data_length,
                        filename.toStdString());
            write_failed = true;
            return false;
        }
        return true;
    };

    // GitLab's package storage sometimes answers 5xx or drops the connection for a moment: try up
    // to three times, a few seconds apart, starting the file over each time.
    constexpr int attempts = 3;
    httplib::Result result;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        if (attempt > 1) {
            file.seek(0);
            file.resize(0);
            hash.reset();
            progress->setValue(0);
            progress->setLabelText(tr("Downloading... (try %1 of %2)").arg(attempt).arg(attempts));
            QEventLoop wait;
            QTimer::singleShot(2000 * (attempt - 1), &wait, &QEventLoop::quit);
            wait.exec();
            if (progress->wasCanceled()) {
                break;
            }
        }
        result = client->Get(m_asset.path, content_receiver, progress_callback);
        const bool canceled = !result && result.error() == httplib::Error::Canceled;
        const bool transient = !write_failed && ((!result && !canceled) ||
                                                 (result && (result->status >= 500 ||
                                                             result->status == 429)));
        if (!transient || attempt == attempts) {
            break;
        }
        LOG_WARNING(Frontend, "GET to {}{} failed ({}), trying again", m_asset.url, m_asset.path,
                    result ? std::to_string(result->status) : httplib::to_string(result.error()));
    }
    progress->close();

    if (write_failed) {
        QtCommon::Frontend::Critical(tr("Failed to save file"),
                                     tr("Could not write to file %1.").arg(filename));
        return;
    }
    if (!result) {
        LOG_ERROR(Frontend, "GET to {}{} failed: {}", m_asset.url, m_asset.path,
                  httplib::to_string(result.error()));
        if (result.error() != httplib::Error::Canceled) {
            QtCommon::Frontend::Critical(tr("Failed to download file"),
                                         tr("Could not reach GitLab to download %1.")
                                             .arg(QString::fromStdString(m_asset.filename)));
        }
        return;
    }

    const auto& response = result.value();
    if (response.status >= 400) {
        LOG_ERROR(Frontend, "GET to {}{} returned error status code: {}", m_asset.url, m_asset.path,
                  response.status);
        QtCommon::Frontend::Critical(tr("Failed to download file"),
                                     tr("Could not download from %1%2\nError code: %3")
                                         .arg(QString::fromStdString(m_asset.url),
                                              QString::fromStdString(m_asset.path),
                                              QString::number(response.status)));
        return;
    }

    if (hash.result().toHex() != QByteArray::fromStdString(*expected)) {
        LOG_ERROR(Frontend, "Downloaded {} does not match its published SHA-256", m_asset.filename);
        file.cancelWriting();
        QtCommon::Frontend::Critical(
            tr("Download failed verification"),
            tr("The downloaded file does not match the checksum GitLab published for it, so it "
               "was discarded. Try again later."));
        return;
    }

    if (!file.commit()) {
        LOG_WARNING(Frontend, "Could not commit to file {}", filename.toStdString());
        QtCommon::Frontend::Critical(tr("Failed to save file"),
                                     tr("Could not commit to file %1.").arg(filename));
        return;
    }

    auto button = QtCommon::Frontend::Question(
        tr("Download Complete"),
        tr("Downloaded and verified %1.\n\nClose Nemyu and extract the zip over your Nemyu folder. "
           "Your settings, saves and shader caches are kept.\n\nOpen the download folder now?")
            .arg(QString::fromStdString(m_asset.filename)),
        QtCommon::Frontend::Yes | QtCommon::Frontend::No);

    if (button == QtCommon::Frontend::Yes) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(filename).absolutePath()));
    }
}
