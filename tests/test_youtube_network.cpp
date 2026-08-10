#include "youtube_network_service.h"
#include "youtube_upload_manager.h"

#include <gtest/gtest.h>

#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace {

struct ReplyResult {
    int status = 0;
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
};

ReplyResult runFakeRequest(const QByteArray &response,
                           const youtube_sync::HttpRequest &request,
                           QByteArray *captured = nullptr) {
    QTcpServer server;
    EXPECT_TRUE(server.listen(QHostAddress::LocalHost, 0));
    youtube_sync::YouTubeNetworkService service;
    QEventLoop loop;
    ReplyResult result;
    QByteArray incoming;
    QObject::connect(&server, &QTcpServer::newConnection, [&]() {
        auto *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::readyRead, [&, socket]() {
            incoming += socket->readAll();
            if (captured) *captured = incoming;
            const int split = incoming.indexOf("\r\n\r\n");
            const int contentLengthAt = incoming.indexOf("Content-Length:");
            int expected = 0;
            if (contentLengthAt >= 0) {
                const int end = incoming.indexOf("\r\n", contentLengthAt);
                expected = incoming.mid(contentLengthAt + 15,
                    end - contentLengthAt - 15).trimmed().toInt();
            }
            if (split >= 0 && incoming.size() - split - 4 >= expected) {
                socket->write(response);
                socket->disconnectFromHost();
            }
        });
    });
    QObject::connect(&service,
        &youtube_sync::YouTubeNetworkService::responseReady,
        [&](const int status, const QByteArray body,
            const QList<QPair<QByteArray, QByteArray>> headers) {
        result = {status, body, headers};
        loop.quit();
    });
    QObject::connect(&service,
        &youtube_sync::YouTubeNetworkService::networkFailed,
        [&](const QString &, const QString &) { loop.quit(); });
    auto local = request;
    local.url = QStringLiteral("http://127.0.0.1:%1/test")
        .arg(server.serverPort()).toStdString();
    service.execute(local);
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    loop.exec();
    return result;
}

} // namespace

TEST(YouTubeNetworkFakeHttp, PlaylistStylePostUsesInjectedEndpoint) {
    QByteArray captured;
    youtube_sync::HttpRequest request{
        "POST", "ignored", {{"Content-Type", "application/json"}},
        "{\"snippet\":{\"title\":\"Set\"}}"};
    const auto result = runFakeRequest(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 17\r\nConnection: close\r\n\r\n{\"id\":\"playlist\"}",
        request, &captured);
    EXPECT_EQ(result.status, 200);
    EXPECT_EQ(result.body, "{\"id\":\"playlist\"}");
    EXPECT_TRUE(captured.startsWith("POST /test HTTP/1.1"));
    EXPECT_TRUE(captured.contains("\"title\":\"Set\""));
}

TEST(YouTubeNetworkFakeHttp, Preserves308RangeForResumableDecision) {
    youtube_sync::HttpRequest request{
        "PUT", "ignored",
        {{"Content-Type", "video/*"},
         {"Content-Range", "bytes 0-524287/1000000"}},
        std::string(524288, 'V')};
    const auto result = runFakeRequest(
        "HTTP/1.1 308 Resume Incomplete\r\nRange: bytes=0-524287\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        request);
    EXPECT_EQ(result.status, 308);
    QByteArray range;
    for (const auto &header : result.headers)
        if (header.first.compare("Range", Qt::CaseInsensitive) == 0)
            range = header.second;
    EXPECT_EQ(range, "bytes=0-524287");
    const auto decision = youtube_sync::decide_upload_response(
        result.status, range.toStdString(), result.body.toStdString());
    EXPECT_EQ(decision.action,
        youtube_sync::UploadResponseDecision::Action::NextChunk);
    EXPECT_EQ(decision.next_offset, 524288u);
}
