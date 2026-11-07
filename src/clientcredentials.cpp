/*
 * Copyright (C) 2026 Wunder Wungiel
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "clientcredentials_p.h"
#include "json.h"
#include "urls.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrl>
#include <QVariantMap>
#ifdef QSOUNDCLOUD_DEBUG
#include <QDebug>
#endif

namespace QSoundCloud {

// Treat the token as expired this many seconds early, so it does not expire while a request is in flight.
static const qint64 EXPIRY_MARGIN = 60;
// Used when the token response has no expires_in.
static const qint64 DEFAULT_EXPIRES_IN = 3600;

static const QString SETTINGS_GROUP("QSoundCloud/ClientCredentials");

static qint64 currentTime() {
    return QDateTime::currentMSecsSinceEpoch() / 1000;
}

static QByteArray formValue(const QString &value) {
    return QUrl::toPercentEncoding(value);
}

ClientCredentials* ClientCredentials::self = 0;

ClientCredentials::ClientCredentials() :
    QObject(QCoreApplication::instance()),
    m_manager(0),
    m_reply(0),
    m_refreshing(false),
    m_expiresAt(0)
{
    load();
}

ClientCredentials* ClientCredentials::instance() {
    return self ? self : self = new ClientCredentials;
}

QString ClientCredentials::accessToken(const QString &clientId) const {
    if ((m_accessToken.isEmpty()) || (clientId != m_clientId) || (currentTime() >= m_expiresAt)) {
        return QString();
    }

    return m_accessToken;
}

bool ClientCredentials::isLoading() const {
    return m_reply != 0;
}

QString ClientCredentials::errorString() const {
    return m_errorString;
}

void ClientCredentials::requestAccessToken(const QString &clientId, const QString &clientSecret) {
    if (isLoading()) {
        return;
    }

    m_pendingClientId = clientId;
    m_pendingClientSecret = clientSecret;

    if ((clientId.isEmpty()) || (clientSecret.isEmpty())) {
        m_errorString = tr("A client id and client secret are required to access the SoundCloud API");
        emit finished();
        return;
    }

    if ((clientId == m_clientId) && (!m_refreshToken.isEmpty())) {
        requestRefreshedToken();
    }
    else {
        requestNewToken();
    }
}

void ClientCredentials::invalidate(const QString &token) {
    if ((!token.isEmpty()) && (token == m_accessToken)) {
#ifdef QSOUNDCLOUD_DEBUG
        qDebug() << "QSoundCloud::ClientCredentials::invalidate";
#endif
        m_accessToken.clear();
        m_expiresAt = 0;
        save();
    }
}

void ClientCredentials::requestNewToken() {
#ifdef QSOUNDCLOUD_DEBUG
    qDebug() << "QSoundCloud::ClientCredentials::requestNewToken" << m_pendingClientId;
#endif
    m_refreshing = false;

    // Only HTTP Basic authentication is supported for the client_credentials grant.
    QNetworkRequest request((QUrl(TOKEN_URL)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("Accept", "application/json; charset=utf-8");
    request.setRawHeader("Authorization", "Basic "
                         + QString(m_pendingClientId + ":" + m_pendingClientSecret).toUtf8().toBase64());

    if (!m_manager) {
        m_manager = new QNetworkAccessManager(this);
    }

    m_reply = m_manager->post(request, "grant_type=" + formValue(GRANT_TYPE_CLIENT));
    connect(m_reply, SIGNAL(finished()), this, SLOT(onReplyFinished()));
}

void ClientCredentials::requestRefreshedToken() {
#ifdef QSOUNDCLOUD_DEBUG
    qDebug() << "QSoundCloud::ClientCredentials::requestRefreshedToken" << m_pendingClientId;
#endif
    m_refreshing = true;

    QNetworkRequest request((QUrl(TOKEN_URL)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("Accept", "application/json; charset=utf-8");

    const QByteArray body = "grant_type=" + formValue(GRANT_TYPE_REFRESH)
                            + "&client_id=" + formValue(m_pendingClientId)
                            + "&client_secret=" + formValue(m_pendingClientSecret)
                            + "&refresh_token=" + formValue(m_refreshToken);

    if (!m_manager) {
        m_manager = new QNetworkAccessManager(this);
    }

    m_reply = m_manager->post(request, body);
    connect(m_reply, SIGNAL(finished()), this, SLOT(onReplyFinished()));
}

void ClientCredentials::onReplyFinished() {
    if (!m_reply) {
        return;
    }

    const QString response = QString::fromUtf8(m_reply->readAll());
    const QNetworkReply::NetworkError error = m_reply->error();
    const QString replyErrorString = m_reply->errorString();
    m_reply->deleteLater();
    m_reply = 0;

    bool ok = false;
    const QVariantMap result = QtJson::Json::parse(response, ok).toMap();
    const QString token = result.value("access_token").toString();
#ifdef QSOUNDCLOUD_DEBUG
    qDebug() << "QSoundCloud::ClientCredentials::onReplyFinished" << error << response;
#endif
    if ((error == QNetworkReply::NoError) && (ok) && (!token.isEmpty())) {
        qint64 expiresIn = result.value("expires_in").toLongLong();

        if (expiresIn <= 0) {
            expiresIn = DEFAULT_EXPIRES_IN;
        }

        m_clientId = m_pendingClientId;
        m_accessToken = token;
        // Refresh tokens are single-use: keep the new one, or none if the response has none.
        m_refreshToken = result.value("refresh_token").toString();
        m_expiresAt = currentTime() + expiresIn - EXPIRY_MARGIN;
        m_errorString.clear();
        save();
        emit finished();
        return;
    }

    if (m_refreshing) {
        // The refresh token was rejected or has already been used. Fall back to a new token.
        m_refreshToken.clear();
        save();
        requestNewToken();
        return;
    }

    if (result.contains("error_description")) {
        m_errorString = result.value("error_description").toString();
    }
    else if (result.contains("error")) {
        m_errorString = result.value("error").toString();
    }
    else if (error != QNetworkReply::NoError) {
        m_errorString = replyErrorString;
    }
    else {
        m_errorString = tr("Unable to obtain an access token");
    }

    emit finished();
}

void ClientCredentials::load() {
    QSettings settings;
    settings.beginGroup(SETTINGS_GROUP);
    m_clientId = settings.value("clientId").toString();
    m_accessToken = settings.value("accessToken").toString();
    m_refreshToken = settings.value("refreshToken").toString();
    m_expiresAt = settings.value("expiresAt").toLongLong();
    settings.endGroup();
}

void ClientCredentials::save() {
    QSettings settings;
    settings.beginGroup(SETTINGS_GROUP);
    settings.setValue("clientId", m_clientId);
    settings.setValue("accessToken", m_accessToken);
    settings.setValue("refreshToken", m_refreshToken);
    settings.setValue("expiresAt", m_expiresAt);
    settings.endGroup();
}

}
