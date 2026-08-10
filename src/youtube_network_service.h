#pragma once

#include "youtube_api_client.h"
#include "youtube_auth.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QTcpServer>
#include <QUrl>
#include <QUrlQuery>

namespace youtube_sync {

class YouTubeNetworkService final : public QObject {
    Q_OBJECT
public:
    explicit YouTubeNetworkService(QObject *parent = nullptr);

    void execute(const HttpRequest &request);
    [[nodiscard]] bool beginAuthorization(const OAuthClientConfig &config);
    void exchangeAuthorizationCode(const OAuthClientConfig &config,
                                   const OAuthCallback &callback);
    void refreshAccessToken(const OAuthClientConfig &config,
                            QString refreshToken);
    [[nodiscard]] QString redirectUri() const { return redirectUri_; }

signals:
    void responseReady(int status, QByteArray body,
                       QList<QPair<QByteArray, QByteArray>> headers);
    void networkFailed(QString userMessage, QString technicalDetail);
    void authorizationBrowserUrl(QUrl url);
    void authorizationCallbackReceived(bool accepted, QString code,
                                       QString error);
    void tokenResponseReady(QByteArray json);

private:
    void acceptCallback();
    void postForm(const QString &endpoint, const QUrlQuery &form);

    QNetworkAccessManager network_;
    QTcpServer callbackServer_;
    OAuthClientConfig authorizationConfig_;
    PkceAttempt attempt_;
    QString redirectUri_;
};

} // namespace youtube_sync
