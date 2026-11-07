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

#ifndef QSOUNDCLOUD_CLIENTCREDENTIALS_P_H
#define QSOUNDCLOUD_CLIENTCREDENTIALS_P_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace QSoundCloud {

/*
    Process-wide app access token, obtained with the OAuth Client Credentials flow.

    The SoundCloud API requires an access token for every request, including requests for public resources made
    without a signed-in user. SoundCloud limits how many of these tokens can be issued (50 per 12 hours per app,
    30 per hour per IP), so one token is shared by all Request instances, persisted with QSettings and renewed with
    its refresh token instead of being requested again.
*/
class ClientCredentials : public QObject
{
    Q_OBJECT

public:
    static ClientCredentials* instance();

    // Returns the token for clientId if one has been obtained and has not expired, otherwise an empty string.
    QString accessToken(const QString &clientId) const;

    bool isLoading() const;

    // The reason the last token request failed.
    QString errorString() const;

    // Obtains a new token, refreshing the current one if possible. finished() is emitted when done.
    // Does nothing if a token request is already in progress.
    void requestAccessToken(const QString &clientId, const QString &clientSecret);

    // Discards token, e.g. after the API rejected it, if it is still the current token.
    void invalidate(const QString &token);

Q_SIGNALS:
    void finished();

private Q_SLOTS:
    void onReplyFinished();

private:
    ClientCredentials();

    void load();
    void save();

    void requestNewToken();
    void requestRefreshedToken();

    QNetworkAccessManager *m_manager;
    QNetworkReply *m_reply;

    bool m_refreshing;

    QString m_pendingClientId;
    QString m_pendingClientSecret;

    QString m_clientId;
    QString m_accessToken;
    QString m_refreshToken;
    qint64 m_expiresAt;

    QString m_errorString;

    static ClientCredentials *self;
};

}

#endif // QSOUNDCLOUD_CLIENTCREDENTIALS_P_H
