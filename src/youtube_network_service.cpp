#include "youtube_network_service.h"

#include <QDesktopServices>
#include <QHttpMultiPart>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QUrlQuery>

namespace youtube_sync {

YouTubeNetworkService::YouTubeNetworkService(QObject *parent)
    : QObject(parent) {
    connect(&callbackServer_, &QTcpServer::newConnection,
            this, &YouTubeNetworkService::acceptCallback);
}

void YouTubeNetworkService::execute(const HttpRequest &request) {
    QNetworkRequest qtRequest(QUrl(QString::fromStdString(request.url)));
    for (const auto &[name, value] : request.headers)
        qtRequest.setRawHeader(QByteArray::fromStdString(name),
                               QByteArray::fromStdString(value));
    const QByteArray body = QByteArray::fromStdString(request.body);
    QNetworkReply *reply = nullptr;
    if (request.method == "GET") reply = network_.get(qtRequest);
    else if (request.method == "POST") reply = network_.post(qtRequest, body);
    else if (request.method == "PUT") reply = network_.put(qtRequest, body);
    else {
        emit networkFailed(tr("Unsupported network operation."),
                           QString::fromStdString(request.method));
        return;
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError && status == 0) {
            emit networkFailed(tr("The YouTube network request failed."),
                QString::fromStdString(redact_sensitive(
                    reply->errorString().toStdString())));
        } else {
            emit responseReady(status, body, reply->rawHeaderPairs());
        }
        reply->deleteLater();
    });
}

bool YouTubeNetworkService::beginAuthorization(
    const OAuthClientConfig &config) {
    if (!config.configured()) return false;
    callbackServer_.close();
    if (!callbackServer_.listen(QHostAddress::LocalHost, 0)) return false;
    authorizationConfig_ = config;
    attempt_ = create_pkce_attempt();
    redirectUri_ = QStringLiteral("http://127.0.0.1:%1/")
        .arg(callbackServer_.serverPort());
    const QUrl url(QString::fromStdString(authorization_url(
        config, attempt_, redirectUri_.toStdString())));
    emit authorizationBrowserUrl(url);
    return QDesktopServices::openUrl(url);
}

void YouTubeNetworkService::acceptCallback() {
    auto *socket = callbackServer_.nextPendingConnection();
    if (!socket) return;
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        const QByteArray request = socket->readAll();
        const int firstSpace = request.indexOf(' ');
        const int secondSpace = request.indexOf(' ', firstSpace + 1);
        OAuthCallback callback;
        if (firstSpace > 0 && secondSpace > firstSpace) {
            callback = parse_oauth_callback(
                request.mid(firstSpace + 1, secondSpace - firstSpace - 1).toStdString(),
                attempt_.state);
        } else callback.error = "invalid callback request";
        const QByteArray html = callback.accepted
            ? "<h1>Reliquary connected</h1><p>You may close this window.</p>"
            : "<h1>Reliquary authorization failed</h1><p>Return to the app.</p>";
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
                      QByteArray::number(html.size()) + "\r\n\r\n" + html);
        socket->disconnectFromHost();
        callbackServer_.close();
        emit authorizationCallbackReceived(callback.accepted,
            QString::fromStdString(callback.code),
            QString::fromStdString(callback.error));
        if (callback.accepted)
            exchangeAuthorizationCode(authorizationConfig_, callback);
    });
}

void YouTubeNetworkService::exchangeAuthorizationCode(
    const OAuthClientConfig &config, const OAuthCallback &callback) {
    QUrlQuery form;
    form.addQueryItem("client_id", QString::fromStdString(config.client_id));
    if (!config.client_secret.empty())
        form.addQueryItem("client_secret", QString::fromStdString(config.client_secret));
    form.addQueryItem("code", QString::fromStdString(callback.code));
    form.addQueryItem("code_verifier", QString::fromStdString(attempt_.verifier));
    form.addQueryItem("redirect_uri", redirectUri_);
    form.addQueryItem("grant_type", "authorization_code");
    postForm(QString::fromStdString(config.token_endpoint), form);
}

void YouTubeNetworkService::refreshAccessToken(
    const OAuthClientConfig &config, QString refreshToken) {
    QUrlQuery form;
    form.addQueryItem("client_id", QString::fromStdString(config.client_id));
    if (!config.client_secret.empty())
        form.addQueryItem("client_secret", QString::fromStdString(config.client_secret));
    form.addQueryItem("refresh_token", std::move(refreshToken));
    form.addQueryItem("grant_type", "refresh_token");
    postForm(QString::fromStdString(config.token_endpoint), form);
}

void YouTubeNetworkService::postForm(const QString &endpoint,
                                    const QUrlQuery &form) {
    QNetworkRequest request{QUrl(endpoint)};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      "application/x-www-form-urlencoded");
    auto *reply = network_.post(request,
        form.query(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        if (status >= 200 && status < 300) emit tokenResponseReady(body);
        else emit networkFailed(tr("YouTube authorization could not be completed."),
            QStringLiteral("HTTP %1: %2").arg(status).arg(
                QString::fromStdString(redact_sensitive(body.toStdString()))));
        reply->deleteLater();
    });
}

} // namespace youtube_sync
