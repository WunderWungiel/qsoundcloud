/*
 * Copyright (C) 2015 Stuart Howarth <showarth@marxoft.co.uk>
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

#include "streamsrequest.h"
#include "request_p.h"
#include "urls.h"
#include <QNetworkReply>
#ifdef QSOUNDCLOUD_DEBUG
#include <QDebug>
#endif

namespace QSoundCloud {

class Format : public QVariantMap
{

public:
    Format() :
        QVariantMap()
    {
    }
    
    Format(const QString &id, const QString &desc, const QString &ext, const QUrl &url) :
        QVariantMap()
    {
        insert("id", id);
        insert("description", desc);
        insert("ext", ext);
        insert("url", url);
    }
};

class StreamsRequestPrivate : public RequestPrivate
{

public:
    StreamsRequestPrivate(StreamsRequest *parent) :
        RequestPrivate(parent)
    {
    }

    // A stream whose API URL redirects to the media (or HLS playlist) URL, still to be resolved.
    struct PendingStream {
        PendingStream(const QString &i, const QString &d, const QString &e, const QString &p, const QUrl &u) :
            id(i),
            description(d),
            ext(e),
            protocol(p),
            url(u)
        {
        }

        QString id;
        QString description;
        QString ext;
        QString protocol;
        QUrl url;
    };

    void finish() {
        Q_Q(StreamsRequest);

        setResult(formats);
        setStatus(Request::Ready);
        setError(Request::NoError);
        setErrorString(QString());

        emit q->finished();
    }

    // Resolves the next pending stream, or finishes when there is none left.
    void resolveNextStream() {
        Q_Q(StreamsRequest);

        if (reply) {
            reply->deleteLater();
            reply = 0;
        }

        if (pending.isEmpty()) {
            finish();
            return;
        }

#ifdef QSOUNDCLOUD_DEBUG
        qDebug() << "StreamsRequestPrivate::resolveNextStream" << pending.first().url;
#endif
        q->setUrl(pending.first().url);
        reply = networkAccessManager()->head(buildRequest());
        StreamsRequest::connect(reply, SIGNAL(finished()), q, SLOT(_q_onStreamRedirect()));
    }

    // Returns true, after finishing the request as canceled, if the current reply was canceled.
    bool checkCanceled() {
        if (reply->error() != QNetworkReply::OperationCanceledError) {
            return false;
        }

        Q_Q(StreamsRequest);

        reply->deleteLater();
        reply = 0;
        pending.clear();
        setStatus(Request::Canceled);
        setError(Request::NoError);
        setErrorString(QString());
        emit q->finished();
        return true;
    }

    void _q_onStreamRedirect() {
        if ((!reply) || (pending.isEmpty()) || (checkCanceled())) {
            return;
        }

        const PendingStream stream = pending.takeFirst();

        if (reply->error() == QNetworkReply::NoError) {
            QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toString();

            if (redirect.isEmpty()) {
                redirect = reply->header(QNetworkRequest::LocationHeader).toString();
            }

            if (!redirect.isEmpty()) {
                if (stream.protocol == "http") {
                    // Media players on older platforms cannot play HTTPS streams.
                    redirect.setScheme("http");
                }

                Format format(stream.id, stream.description, stream.ext, redirect);
                format.insert("protocol", stream.protocol);
                formats << format;
            }
        }
#ifdef QSOUNDCLOUD_DEBUG
        else {
            qDebug() << "StreamsRequestPrivate::_q_onStreamRedirect" << reply->errorString();
        }
#endif
        resolveNextStream();
    }

    // Requests the track's transcodings. SoundCloud no longer serves full length tracks from stream_url (it
    // redirects to a 30 second preview), and most tracks are now only available as HLS.
    void getStreams() {
        Q_Q(StreamsRequest);

        // Prefer the URN, as large numeric ids may not survive JSON number parsing.
        const QString id = track.contains("urn") ? track.value("urn").toString() : track.value("id").toString();
        q->setUrl(API_URL + "/tracks/" + id + "/streams");
        reply = networkAccessManager()->get(buildRequest());
        StreamsRequest::connect(reply, SIGNAL(finished()), q, SLOT(_q_onStreamsListFinished()));
    }

    void _q_onStreamsListFinished() {
        if ((!reply) || (checkCanceled())) {
            return;
        }

        const QString response = QString::fromUtf8(reply->readAll());
        const QNetworkReply::NetworkError e = reply->error();
        bool ok = (e == QNetworkReply::NoError);
        const QVariantMap streams = ok ? QtJson::Json::parse(response, ok).toMap() : QVariantMap();
#ifdef QSOUNDCLOUD_DEBUG
        qDebug() << "StreamsRequestPrivate::_q_onStreamsListFinished" << e << response;
#endif
        if (streams.contains("http_mp3_128_url")) {
            pending << PendingStream("mp3", "128kb MP3", "mp3", "http", streams.value("http_mp3_128_url").toString());
        }
        else if (streams.contains("hls_mp3_128_url")) {
            // Same id and description as the progressive stream, so that saved playback/download formats match.
            pending << PendingStream("mp3", "128kb MP3", "mp3", "hls", streams.value("hls_mp3_128_url").toString());
        }
        else if (streams.contains("preview_mp3_128_url")) {
            pending << PendingStream("preview", StreamsRequest::tr("30 second preview (MP3)"), "mp3", "http",
                                     streams.value("preview_mp3_128_url").toString());
        }
        else if ((!ok) && (track.value("streamable").toBool())) {
            // The transcodings could not be requested, so fall back to the legacy stream URL.
            pending << PendingStream("mp3", "128kb MP3", "mp3", "http", track.value("stream_url").toString());
        }

        resolveNextStream();
    }

    void _q_onReplyFinished() {
        if (!reply) {
            return;
        }

        Q_Q(StreamsRequest);

        if (redirects < MAX_REDIRECTS) {
            QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toString();

            if (redirect.isEmpty()) {
                redirect = reply->header(QNetworkRequest::LocationHeader).toString();
            }

            if (!redirect.isEmpty()) {
                reply->deleteLater();
                reply = 0;
                followRedirect(redirect);
                return;
            }
        }

        const QString response = QString::fromUtf8(reply->readAll());
        const QNetworkReply::NetworkError e = reply->error();
        const QString es = reply->errorString();
        reply->deleteLater();
        reply = 0;

        switch (e) {
        case QNetworkReply::NoError:
            break;
        case QNetworkReply::OperationCanceledError:
            setStatus(Request::Canceled);
            setError(Request::NoError);
            setErrorString(QString());
            emit q->finished();
            return;
        case QNetworkReply::AuthenticationRequiredError:
            if (!refreshToken.isEmpty()) {
                refreshAccessToken();
            }
            else if (!retryWithNewClientToken()) {
                setStatus(Request::Failed);
                setError(Request::Error(e));
                setErrorString(es);
                emit q->finished();
            }

            return;
        default:
            setStatus(Request::Failed);
            setError(Request::Error(e));
            setErrorString(es);
            emit q->finished();
            return;
        }

        bool ok = true;
        track = QtJson::Json::parse(response, ok).toMap();
        formats.clear();
        pending.clear();

        if (ok) {
            if (track.value("downloadable").toBool()) {
                const QString ext = track.value("original_format").toString();
                pending << PendingStream("original", QString("%1 (%2)").arg(StreamsRequest::tr("Original format"))
                                         .arg(ext.toUpper()), ext, "http", track.value("download_url").toString());
            }

            if (track.value("streamable").toBool()) {
                getStreams();
            }
            else {
                resolveNextStream();
            }

            return;
        }

        setStatus(Request::Failed);
        setError(Request::ParseError);
        setErrorString(Request::tr("Unable to parse response"));
        emit q->finished();
    }

    QVariantList formats;
    QVariantMap track;
    QList<PendingStream> pending;

    Q_DECLARE_PUBLIC(StreamsRequest)
};

/*!
    \class StreamsRequest
    \brief Handles requests for track streams
    
    \ingroup requests
    
    The StreamsRequest class is used for requesting a list of streams for a SoundCloud track.
    
    Example usage:
    
    C++
    
    \code
    using namespace QSoundCloud;
    
    ...
    
    StreamsRequest request;
    request.get(TRACK_ID);
    connect(&request, SIGNAL(finished()), this, SLOT(onRequestFinished()));
    
    ...
    
    void MyClass::onRequestFinished() {
        if (request.status() == StreamsRequest::Ready) {
            foreach (QVariant stream, request.result().toList()) {
                qDebug() << "ID:" << stream.value("id").toString();
                qDebug() << "Description:" << stream.value("description").toString();
                qDebug() << "Extension:" << stream.value("ext").toString();
                qDebug() << "URL:" << stream.value("url").toString();
            }
        }
        else {
            qDebug() << request.errorString();
        }
    }
    \endcode
    
    QML
    
    \code
    import QtQuick 1.0
    import QSoundCloud 1.0
    
    StreamsRequest {
        id: request
        
        onFinished: {
            if (status == StreamsRequest.Ready) {
                for (var i = 0; i < result.length; i++) {
                    console.log("ID: " + result[i].id);
                    console.log("Description: " + result[i].description);
                    console.log("Extension: " + result[i].ext);
                    console.log("URL: " + result[i].url);
                }
            }
            else {
                console.log(errorString);
            }
        }
        
        Component.onCompleted: get(TRACK_ID)
    }
    \endcode
*/
StreamsRequest::StreamsRequest(QObject *parent) :
    Request(*new StreamsRequestPrivate(this), parent)
{
}

/*!
    \brief Requests a list of streams for the track identified by id.
    
    \a id can be either a track id or a URL. If a URL is provided, 
    StreamsRequest will resolve the URL before retrieving the streams.
*/
void StreamsRequest::get(const QString &id) {
    if (status() == Loading) {
        return;
    }
    
    if (id.startsWith("http")) {
        setUrl(API_URL + "/resolve?url=" + id);
    }
    else {
        setUrl(API_URL + "/tracks/" + id);
    }
    
    Request::get();
}

}

#include "moc_streamsrequest.cpp"
