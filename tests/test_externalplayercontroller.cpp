#include "ExternalPlayerController.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QSaveFile>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrlQuery>

static QByteArray headerValue(const QByteArray& request, const QByteArray& name)
{
  for (const auto& line : request.split('\n')) {
    const int separator = line.indexOf(':');
    if (separator >= 0 && line.left(separator).trimmed().compare(name, Qt::CaseInsensitive) == 0)
      return line.mid(separator + 1).trimmed();
  }
  return {};
}

// A separate process speaks VLC's real loopback HTTP protocol. It lets the tests
// cover QProcess ownership, authentication, controls and asynchronous teardown.
static int fakeVlc(QCoreApplication& app)
{
  quint16 port = 0;
  QString password;
  for (const auto& arg : app.arguments()) {
    if (arg.startsWith("--http-port=")) port = arg.mid(12).toUShort();
    if (arg.startsWith("--http-password=")) password = arg.mid(16);
  }
  QFile log(qEnvironmentVariable("EXTERNAL_PLAYER_TEST_LOG"));
  if (!log.open(QIODevice::Append)) return 2;
  log.write(QJsonDocument(QJsonObject{{"arguments", QJsonArray::fromStringList(app.arguments())}}).toJson(QJsonDocument::Compact) + '\n');
  log.flush();
  QTcpServer server;
  if (!server.listen(QHostAddress::LocalHost, port)) return 3;
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
    while (server.hasPendingConnections()) {
      auto* socket = server.nextPendingConnection();
      QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
        auto request = socket->property("request").toByteArray() + socket->readAll();
        socket->setProperty("request", request);
        if (!request.contains("\r\n\r\n")) return;
        socket->disconnect(socket, &QTcpSocket::readyRead, nullptr, nullptr);
        const auto target = request.split(' ').value(1);
        const bool authenticated = headerValue(request, "Authorization") == "Basic " + (":" + password.toUtf8()).toBase64();
        log.write(QJsonDocument(QJsonObject{{"target", QString::fromUtf8(target)}, {"authenticated", authenticated}}).toJson(QJsonDocument::Compact) + '\n');
        log.flush();
        if (!authenticated) {
          socket->write("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
          socket->disconnectFromHost();
          return;
        }
        QFile stateFile(qEnvironmentVariable("EXTERNAL_PLAYER_TEST_STATE"));
        if (!stateFile.open(QIODevice::ReadOnly)) { app.exit(4); return; }
        auto state = QJsonDocument::fromJson(stateFile.readAll()).object();
        if (state.value("hang").toBool()) return;
        if (state.value("exit").toBool()) { app.quit(); return; }
        const auto query = QUrlQuery(QUrl(QString::fromUtf8(target)));
        const auto command = query.queryItemValue("command");
        if (command == "pl_forcepause") state["state"] = "paused";
        if (command == "pl_forceresume") state["state"] = "playing";
        if (command == "seek") state["time"] = query.queryItemValue("val").toDouble();
        const auto body = QJsonDocument(state).toJson(QJsonDocument::Compact);
        const auto response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        QTimer::singleShot(state.value("responseDelayMilliseconds").toInt(), socket, [socket, response] {
          socket->write(response);
          socket->disconnectFromHost();
        });
      });
    }
  });
  return app.exec();
}

class ExternalPlayerControllerTest : public QObject
{
  Q_OBJECT
private:
  QTemporaryDir dir;
  QVariantMap options(const QString& id = "session-one") const { return {{"sessionId", id}, {"durationMilliseconds", 100000}}; }
  void state(qint64 seconds, const QString& playback = "playing", const QJsonObject& extra = {}) {
    QJsonObject data{{"time", seconds}, {"length", 100}, {"state", playback}, {"currentplid", 4}};
    for (auto it = extra.begin(); it != extra.end(); ++it) data[it.key()] = it.value();
    QSaveFile file(dir.filePath("state.json"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(data).toJson());
    QVERIFY(file.commit());
  }
  QByteArray log() const { QFile file(dir.filePath("log.jsonl")); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
  bool start(ExternalPlayerController& c, const QString& id = "session-one") {
    return c.start("vlc", QCoreApplication::applicationFilePath(), "https://example.test/movie?api_key=secret", options(id));
  }
private slots:
  void httpHeaderNamesAreCaseInsensitive() {
    for (const auto& name : {QByteArray("Authorization"), QByteArray("authorization"), QByteArray("AUTHORIZATION")}) {
      QCOMPARE(headerValue("GET / HTTP/1.1\r\n" + name + ": Basic OnRlc3Q=\r\n\r\n", "Authorization"), QByteArray("Basic OnRlc3Q="));
    }
    QVERIFY(headerValue("GET /?Authorization=secret HTTP/1.1\r\nHost: localhost\r\n\r\n", "Authorization").isEmpty());
  }
  void init() {
    QVERIFY(dir.isValid());
    QFile::remove(dir.filePath("log.jsonl"));
    qputenv("EXTERNAL_PLAYER_TEST_STATE", dir.filePath("state.json").toUtf8());
    qputenv("EXTERNAL_PLAYER_TEST_LOG", dir.filePath("log.jsonl").toUtf8());
    state(12);
  }
  void muteThenChangeVolumePreservesBackendVolume() {
    // PotPlayer keeps an independent mute flag. Changing volume while muted
    // must not replace the stored volume with zero before SET_MUTE clears it.
    QCOMPARE(ExternalPlayerController::volumeArgument("potplayer", 50, true), 50);
    QCOMPARE(ExternalPlayerController::volumeArgument("potplayer", 50, false), 50);
    // VLC has no independent mute command in this adapter: defer its audible
    // volume until unmute and restore the selected 50% as VLC's 128/256.
    QCOMPARE(ExternalPlayerController::volumeArgument("vlc", 50, true), 0);
    QCOMPARE(ExternalPlayerController::volumeArgument("vlc", 50, false), 128);
  }
  void rejectsUnsafeInput_data() {
    QTest::addColumn<QString>("url"); QTest::addColumn<QString>("subtitle"); QTest::addColumn<QString>("agent");
    QTest::newRow("scheme") << "javascript:alert(1)" << "" << "";
    QTest::newRow("argument") << "--extraintf=evil" << "" << "";
    QTest::newRow("newline") << "https://example.test/a\nfoo" << "" << "";
    QTest::newRow("subtitle") << "https://example.test/a" << "vlc://quit" << "";
    QTest::newRow("header") << "https://example.test/a" << "" << "Jellyfin\r\nAuthorization: evil";
  }
  void rejectsUnsafeInput() {
    QFETCH(QString, url); QFETCH(QString, subtitle); QFETCH(QString, agent);
    ExternalPlayerController c; QSignalSpy failed(&c, &ExternalPlayerController::failed);
    auto opts = options(); opts["subtitleUrl"] = subtitle; opts["userAgent"] = agent;
    QVERIFY(!c.start("vlc", QCoreApplication::applicationFilePath(), url, opts));
    QCOMPARE(failed.size(), 1);
    QVERIFY(!failed.first().at(1).toString().contains("Authorization"));
  }
  void missingExecutableIsTerminal() {
    ExternalPlayerController c; QSignalSpy failed(&c, &ExternalPlayerController::failed); QSignalSpy ended(&c, &ExternalPlayerController::ended);
    QVERIFY(!c.start("vlc", dir.filePath("missing"), "https://example.test/movie", options()));
    QCOMPARE(failed.size(), 1); QCOMPARE(ended.size(), 0);
    QVERIFY(start(c)); c.stop("session-one"); QTRY_COMPARE(ended.size(), 1);
  }
  void reportsAuthenticatedProgressAndControls() {
    // Receiving/logging a command in the player is earlier than the controller
    // receiving its response. Keep that gap explicit to exercise async waits.
    state(12, "playing", {{"responseDelayMilliseconds", 100}});
    ExternalPlayerController c; QSignalSpy updated(&c, &ExternalPlayerController::updated); QSignalSpy ended(&c, &ExternalPlayerController::ended);
    QVERIFY(start(c)); QTRY_VERIFY_WITH_TIMEOUT(!updated.isEmpty(), 5000);
    QCOMPARE(updated.last().at(1).toLongLong(), 12000); QCOMPARE(updated.last().at(2).toLongLong(), 100000);
    QVERIFY(log().contains("\"authenticated\":true"));
    c.pause("session-one"); QTRY_COMPARE(updated.last().at(3).toString(), QString("paused"));
    c.play("session-one"); QTRY_COMPARE(updated.last().at(3).toString(), QString("playing"));
    c.seekTo("session-one", 42500); QTRY_VERIFY(log().contains("command=seek&val=42"));
    QTRY_COMPARE(updated.last().at(1).toLongLong(), 42000);
    c.setVolume("session-one", 40); QTRY_VERIFY(log().contains("command=volume&val=102"));
    c.setMuted("session-one", true); QTRY_VERIFY(log().contains("command=volume&val=0"));
    c.stop("session-one"); QTRY_COMPARE(ended.size(), 1);
  }
  void stopSnapshotsLatestPositionExactlyOnce() {
    ExternalPlayerController c; QSignalSpy updated(&c, &ExternalPlayerController::updated); QSignalSpy ended(&c, &ExternalPlayerController::ended);
    QVERIFY(start(c)); QTRY_VERIFY(!updated.isEmpty()); state(54);
    c.stop("session-one"); c.stop("session-one"); QTRY_COMPARE(ended.size(), 1);
    QCOMPARE(ended.first().at(1).toLongLong(), 54000); QVERIFY(!ended.first().at(3).toBool());
    QTest::qWait(700); QCOMPARE(ended.size(), 1);
  }
  void stoppedResetPreservesLastPosition() {
    state(99); ExternalPlayerController c; QSignalSpy updated(&c, &ExternalPlayerController::updated); QSignalSpy ended(&c, &ExternalPlayerController::ended);
    QVERIFY(start(c)); QTRY_VERIFY(!updated.isEmpty()); state(0, "stopped", {{"length", 0}});
    QTRY_COMPARE(ended.size(), 1); QCOMPARE(ended.first().at(1).toLongLong(), 99000); QVERIFY(ended.first().at(3).toBool());
  }
  void manualExitDoesNotCompleteMedia_data() {
    QTest::addColumn<int>("lastSeconds");
    QTest::newRow("middle") << 12;
    QTest::newRow("near-end") << 99;
  }
  void manualExitDoesNotCompleteMedia() {
    QFETCH(int, lastSeconds); state(lastSeconds);
    ExternalPlayerController c; QSignalSpy updated(&c, &ExternalPlayerController::updated); QSignalSpy ended(&c, &ExternalPlayerController::ended);
    QVERIFY(start(c)); QTRY_VERIFY(!updated.isEmpty()); state(0, "stopped", {{"exit", true}});
    QTRY_COMPARE(ended.size(), 1); QCOMPARE(ended.first().at(1).toLongLong(), lastSeconds * 1000); QVERIFY(!ended.first().at(3).toBool());
  }
  void staleSessionControlsCannotAffectReplacement() {
    ExternalPlayerController c; QSignalSpy updated(&c, &ExternalPlayerController::updated); QSignalSpy ended(&c, &ExternalPlayerController::ended);
    QVERIFY(start(c)); c.stop("session-one"); QTRY_COMPARE(ended.size(), 1);
    QVERIFY(start(c, "session-two")); QTRY_VERIFY(!updated.isEmpty() && updated.last().at(0) == "session-two");
    c.stop("session-one"); c.pause("session-one"); QTest::qWait(600); QCOMPARE(ended.size(), 1);
    c.stop("session-two"); QTRY_COMPARE(ended.size(), 2);
  }
  void unrelatedMediaEndsWithoutReportingItsPosition() {
    ExternalPlayerController c; QSignalSpy updated(&c, &ExternalPlayerController::updated); QSignalSpy ended(&c, &ExternalPlayerController::ended);
    QVERIFY(start(c)); QTRY_VERIFY(!updated.isEmpty()); state(75, "playing", {{"currentplid", 9}});
    QTRY_COMPARE(ended.size(), 1); QCOMPARE(ended.first().at(1).toLongLong(), 12000); QVERIFY(!ended.first().at(3).toBool());
  }
  void resumeDoesNotPublishTransientZero() {
    state(0); ExternalPlayerController c; QSignalSpy updated(&c, &ExternalPlayerController::updated);
    QSignalSpy ended(&c, &ExternalPlayerController::ended);
    auto opts = options(); opts["startMilliseconds"] = 45000;
    QVERIFY(c.start("vlc", QCoreApplication::applicationFilePath(), "https://example.test/movie", opts));
    QTRY_VERIFY(!updated.isEmpty());
    QCOMPARE(updated.first().at(1).toLongLong(), 45000);
    c.stop("session-one"); QTRY_COMPARE(ended.size(), 1);
  }
  void failedLaunchAllowsReplacementImmediately() {
#ifndef Q_OS_WIN
    QFile invalid(dir.filePath("invalid-player")); QVERIFY(invalid.open(QIODevice::WriteOnly));
    invalid.write("This is not an executable.\n"); invalid.close();
    QVERIFY(invalid.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    ExternalPlayerController c; QSignalSpy failed(&c, &ExternalPlayerController::failed);
    QSignalSpy ended(&c, &ExternalPlayerController::ended); QSignalSpy updated(&c, &ExternalPlayerController::updated);
    QVERIFY(c.start("vlc", invalid.fileName(), "https://example.test/movie", options()));
    QTRY_COMPARE(failed.size(), 1); QCOMPARE(ended.size(), 0);
    QVERIFY(start(c, "replacement")); QTRY_VERIFY(!updated.isEmpty());
    QCOMPARE(updated.first().at(0).toString(), QString("replacement"));
    c.stop("replacement"); QTRY_COMPARE(ended.size(), 1);
#endif
  }
  void startupAndControlTimeoutAreTerminal_data() {
    QTest::addColumn<bool>("afterPlayback");
    QTest::newRow("startup") << false;
    QTest::newRow("control") << true;
  }
  void startupAndControlTimeoutAreTerminal() {
    QFETCH(bool, afterPlayback);
    ExternalPlayerController c; QSignalSpy failed(&c, &ExternalPlayerController::failed);
    QSignalSpy ended(&c, &ExternalPlayerController::ended); QSignalSpy updated(&c, &ExternalPlayerController::updated);
    if (!afterPlayback) state(0, "stopped", {{"hang", true}});
    QVERIFY(start(c));
    if (afterPlayback) { QTRY_VERIFY(!updated.isEmpty()); state(12, "playing", {{"hang", true}}); }
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, afterPlayback ? 13000 : 28000);
    QCOMPARE(ended.size(), 0);
    state(15); QVERIFY(start(c, "replacement"));
    QTRY_VERIFY(!updated.isEmpty() && updated.last().at(0).toString() == "replacement");
    c.stop("replacement"); QTRY_COMPARE(ended.size(), 1);
    QCOMPARE(failed.size(), 1);
  }
  void realVlcSmoke() {
    const auto media = qEnvironmentVariable("EXTERNAL_PLAYER_VLC_SMOKE");
    if (media.isEmpty()) QSKIP("Set EXTERNAL_PLAYER_VLC_SMOKE to a local media file for real VLC validation.");
    QVERIFY(QFileInfo::exists(media));
    ExternalPlayerController c; QSignalSpy updated(&c, &ExternalPlayerController::updated);
    QSignalSpy ended(&c, &ExternalPlayerController::ended); QSignalSpy failed(&c, &ExternalPlayerController::failed);
    auto opts = options("real-vlc"); opts["startMilliseconds"] = 3000;
#ifdef Q_OS_MACOS
    const QString executable = "/Applications/VLC.app";
#else
    const QString executable;
#endif
    QVERIFY(c.start("vlc", executable, QUrl::fromLocalFile(media).toString(), opts));
    QTRY_VERIFY_WITH_TIMEOUT(!updated.isEmpty() || !failed.isEmpty(), 28000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().at(1).toString()));
    QVERIFY(updated.first().at(1).toLongLong() >= 2000);
    c.pause("real-vlc"); QTRY_COMPARE(updated.last().at(3).toString(), QString("paused"));
    c.seekTo("real-vlc", 8000); QTRY_VERIFY(updated.last().at(1).toLongLong() >= 7000);
    c.play("real-vlc"); QTRY_COMPARE(updated.last().at(3).toString(), QString("playing"));
    c.setVolume("real-vlc", 35); c.setMuted("real-vlc", true); c.setMuted("real-vlc", false);
    QTest::qWait(600); c.stop("real-vlc"); QTRY_COMPARE(ended.size(), 1);
    QVERIFY(ended.first().at(1).toLongLong() >= 7000); QVERIFY(!ended.first().at(3).toBool());
    QVERIFY(failed.isEmpty());
    const auto actualDuration = ended.first().at(2).toLongLong();
    QVERIFY(actualDuration > 5000);
    auto eofOptions = options("real-vlc-eof");
    eofOptions["startMilliseconds"] = actualDuration - 2500;
    eofOptions["durationMilliseconds"] = actualDuration;
    QVERIFY(c.start("vlc", executable, QUrl::fromLocalFile(media).toString(), eofOptions));
    QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 2, 15000);
    QVERIFY(ended.last().at(3).toBool());
    QVERIFY(ended.last().at(1).toLongLong() >= actualDuration - 2000);
    QVERIFY(failed.isEmpty());
  }
  void unresponsiveStopIsBounded() {
    ExternalPlayerController c; QSignalSpy updated(&c, &ExternalPlayerController::updated); QSignalSpy ended(&c, &ExternalPlayerController::ended);
    QVERIFY(start(c)); QTRY_VERIFY(!updated.isEmpty()); state(0, "stopped", {{"hang", true}});
    c.stop("session-one"); QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 3500); QCOMPARE(ended.first().at(1).toLongLong(), 12000);
  }
};
int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);
  if (app.arguments().contains("--extraintf=http")) return fakeVlc(app);
  ExternalPlayerControllerTest test;
  return QTest::qExec(&test, argc, argv);
}
#include "test_externalplayercontroller.moc"
