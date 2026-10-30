#pragma once

// Local synthetic HTTP fixture shared by the GUI smoke and network regressions.
#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

class FakeYouTubeApi final : public QObject {
public:
    explicit FakeYouTubeApi(QObject *parent = nullptr, bool deferHandlerUntilBuffered = false)
        : QObject(parent) {
        connect(&server_, &QTcpServer::newConnection, this, [this, deferHandlerUntilBuffered]() {
            while (auto *socket = server_.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                    buffers_.remove(socket);
                    socket->deleteLater();
                });
                const auto attach = [this, guarded = QPointer<QTcpSocket>(socket)]() {
                    auto *socket = guarded.data();
                    if (!socket) return;
                    if (socket->bytesAvailable() > 0) ++bufferedConnections_;
                    connect(socket, &QTcpSocket::readyRead, this,
                            [this, socket]() { consume(socket); });
                    // readyRead may have fired before newConnection was handled.
                    // Drain existing bytes as well as listening for future ones.
                    consume(socket);
                };
                if (deferHandlerUntilBuffered && socket->bytesAvailable() == 0) {
                    // Deterministically exercise the missed-signal ordering in
                    // the regression, without relying on a wall-clock delay.
                    connect(socket, &QTcpSocket::readyRead, this, attach,
                            Qt::SingleShotConnection);
                } else attach();
            }
        });
    }

    bool start() {
        return server_.listen(QHostAddress::LocalHost, 0);
    }

    int bufferedConnections() const { return bufferedConnections_; }

    QString apiBase() const {
        return QStringLiteral("http://127.0.0.1:%1/youtube/v3")
            .arg(server_.serverPort());
    }

    QString uploadBase() const {
        return QStringLiteral("http://127.0.0.1:%1/upload/youtube/v3")
            .arg(server_.serverPort());
    }

    bool complete(const int expectedParts) const {
        return playlistCreates_ == 1 && sessionCreates_ == expectedParts &&
            completedUploads_ == expectedParts &&
            playlistInserts_ == expectedParts && processingChecks_ == 1 &&
            authorizationValid_ && metadataPrivate_;
    }

private:
    void consume(QTcpSocket *socket) {
        auto &buffer = buffers_[socket];
        buffer += socket->readAll();
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QByteArray headers = buffer.left(headerEnd + 4);
        const QRegularExpression contentLength(
            QStringLiteral("(?:^|\\r\\n)Content-Length:\\s*(\\d+)"),
            QRegularExpression::CaseInsensitiveOption);
        const auto match = contentLength.match(QString::fromLatin1(headers));
        const qint64 bodyLength = match.hasMatch()
            ? match.captured(1).toLongLong() : 0;
        if (buffer.size() < headerEnd + 4 + bodyLength) return;
        const QByteArray body = buffer.mid(headerEnd + 4, bodyLength);
        const QList<QByteArray> requestLine = headers.left(
            headers.indexOf("\r\n")).split(' ');
        if (requestLine.size() < 2) {
            respond(socket, 400, "Bad Request", "{}");
            return;
        }
        const QByteArray method = requestLine.at(0);
        const QByteArray target = requestLine.at(1);
        authorizationValid_ = authorizationValid_ &&
            headers.contains("Authorization: Bearer gui-e2e-access");
        if (method == "POST" && target.startsWith(
                "/youtube/v3/playlists?")) {
            ++playlistCreates_;
            respond(socket, 200, "OK",
                R"({"id":"PL_FAKE_SYNC","status":{"privacyStatus":"unlisted"}})");
        } else if (method == "POST" && target.startsWith(
                       "/upload/youtube/v3/videos?")) {
            if (body.contains("source.bin") || body.contains("\\\\") ||
                body.contains(":/"))
                metadataPrivate_ = false;
            const int session = ++sessionCreates_;
            respond(socket, 200, "OK", {}, {{"Location",
                QStringLiteral("http://127.0.0.1:%1/session/%2")
                    .arg(server_.serverPort()).arg(session).toLatin1()}});
        } else if (method == "PUT" && target.startsWith("/session/")) {
            const QRegularExpression range(
                QStringLiteral("Content-Range:\\s*bytes\\s+(\\d+)-(\\d+)/(\\d+)"),
                QRegularExpression::CaseInsensitiveOption);
            const auto rangeMatch = range.match(QString::fromLatin1(headers));
            if (!rangeMatch.hasMatch()) {
                respond(socket, 400, "Bad Request", "{}");
                return;
            }
            const quint64 last = rangeMatch.captured(2).toULongLong();
            const quint64 total = rangeMatch.captured(3).toULongLong();
            if (last + 1 < total) {
                respond(socket, 308, "Resume Incomplete", {},
                    {{"Range", QByteArray("bytes=0-") + QByteArray::number(last)}});
            } else {
                const QByteArray session = target.mid(target.lastIndexOf('/') + 1);
                const QByteArray id = "fake-video-" + session;
                uploadedVideos_ << QString::fromLatin1(id);
                ++completedUploads_;
                respond(socket, 201, "Created",
                    QByteArray("{\"id\":\"") + id +
                    "\",\"status\":{\"privacyStatus\":\"unlisted\"}}");
            }
        } else if (method == "POST" && target.startsWith(
                       "/youtube/v3/playlistItems?")) {
            ++playlistInserts_;
            respond(socket, 200, "OK",
                QByteArray("{\"id\":\"playlist-item-") +
                QByteArray::number(playlistInserts_) + "\"}");
        } else if (method == "GET" && target.startsWith(
                       "/youtube/v3/videos?")) {
            ++processingChecks_;
            QByteArray items;
            for (const auto &id : uploadedVideos_) {
                if (!items.isEmpty()) items += ',';
                items += QByteArray("{\"id\":\"") + id.toLatin1() +
                    "\",\"processingDetails\":{\"processingStatus\":\"succeeded\","
                    "\"processingProgress\":{\"partsProcessed\":1,\"partsTotal\":1}},"
                    "\"status\":{\"privacyStatus\":\"unlisted\"}}";
            }
            respond(socket, 200, "OK", QByteArray("{\"items\":[") +
                items + "]}");
        } else {
            respond(socket, 404, "Not Found", "{}");
        }
    }

    static void respond(QTcpSocket *socket, const int status,
                        const QByteArray &reason, const QByteArray &body,
                        const QList<QPair<QByteArray, QByteArray>> &extra = {}) {
        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' +
            reason + "\r\nContent-Type: application/json\r\nConnection: close\r\n";
        for (const auto &[name, value] : extra)
            response += name + ": " + value + "\r\n";
        response += "Content-Length: " + QByteArray::number(body.size()) +
            "\r\n\r\n" + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QTcpServer server_;
    QHash<QTcpSocket *, QByteArray> buffers_;
    QStringList uploadedVideos_;
    int playlistCreates_ = 0;
    int sessionCreates_ = 0;
    int completedUploads_ = 0;
    int playlistInserts_ = 0;
    int processingChecks_ = 0;
    bool authorizationValid_ = true;
    bool metadataPrivate_ = true;
    int bufferedConnections_ = 0;
};
