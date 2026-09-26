#include "ExternalPlayerController.h"

#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QQueue>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <cmath>
#include <memory>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
constexpr int PollMilliseconds = 500;
constexpr int RequestMilliseconds = 2000;
constexpr int StartupMilliseconds = 25000;
constexpr int ControlFailureMilliseconds = 10000;

bool hasControlCharacters(const QString& text)
{
  for (QChar ch : text) if (ch.unicode() < 0x20 || ch.unicode() == 0x7f) return true;
  return false;
}

bool validMediaUrl(const QString& text)
{
  if (text.isEmpty() || hasControlCharacters(text)) return false;
  const QUrl url(text, QUrl::StrictMode);
  if (!url.isValid()) return false;
  const auto scheme = url.scheme().toLower();
  if (scheme == "http" || scheme == "https") return !url.host().isEmpty();
  return scheme == "file" && url.isLocalFile() && !url.path().isEmpty()
         && (url.host().isEmpty() || url.host() == "localhost");
}

QString executableFor(const QString& backend, QString configured)
{
  QStringList candidates;
  if (!configured.isEmpty()) {
#ifdef Q_OS_MACOS
    if (configured.endsWith(".app", Qt::CaseInsensitive)) configured += "/Contents/MacOS/VLC";
#endif
    candidates << configured;
  } else {
#ifdef Q_OS_MACOS
    if (backend == "vlc") candidates << "/Applications/VLC.app/Contents/MacOS/VLC"
      << QDir::homePath() + "/Applications/VLC.app/Contents/MacOS/VLC";
#endif
#ifdef Q_OS_WIN
    const QStringList names = backend == "vlc" ? QStringList{"vlc.exe"} : QStringList{"PotPlayerMini64.exe", "PotPlayerMini.exe"};
    for (const auto& name : names) {
      for (const auto& hive : {QString("HKEY_CURRENT_USER"), QString("HKEY_LOCAL_MACHINE")}) {
        QSettings registry(hive + "\\Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\" + name, QSettings::NativeFormat);
        candidates << registry.value(".").toString();
      }
      for (const auto& base : {qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)"), qEnvironmentVariable("LOCALAPPDATA")}) {
        if (base.isEmpty()) continue;
        candidates << base + (backend == "vlc" ? "/VideoLAN/VLC/" : "/DAUM/PotPlayer/") + name;
        if (backend != "vlc") candidates << base + "/PotPlayer/" + name;
      }
      candidates << QStandardPaths::findExecutable(name);
    }
#else
    if (backend == "vlc") candidates << QStandardPaths::findExecutable("vlc");
#endif
  }
  for (const auto& candidate : candidates) {
    const QFileInfo info(candidate);
    if (!candidate.isEmpty() && info.isFile() && info.isExecutable()) return info.absoluteFilePath();
  }
  return {};
}

// Never destroy a running QProcess: its destructor waits synchronously. Only our
// launched process is retired, with a bounded asynchronous terminate/kill path.
void retireProcess(QProcess* process)
{
  if (!process) return;
  process->disconnect();
  process->setParent(nullptr);
  if (process->state() == QProcess::NotRunning) { process->deleteLater(); return; }
  QObject::connect(process, &QProcess::finished, process, &QObject::deleteLater);
  if (process->state() == QProcess::Starting)
    QObject::connect(process, &QProcess::started, process, [process] { process->terminate(); });
  else process->terminate();
  QTimer::singleShot(1000, process, [process] {
    if (process->state() != QProcess::NotRunning) process->kill();
  });
}

#ifdef Q_OS_WIN
struct PotResult {
  bool found = false;
  bool ok = false;
  qint64 position = 0;
  qint64 duration = 0;
  int state = 0;
};

// PotPlayer's published InternalSimpleCmd.h protocol uses WM_USER and ms.
// https://github.com/ld3l/PotPlayerControl/blob/master/InternalSimpleCmd.h
bool potMessage(HWND window, WPARAM command, LPARAM value, qint64* result = nullptr)
{
  DWORD_PTR response = 0;
  const auto delivered = SendMessageTimeoutW(window, WM_USER, command, value,
    SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 200, &response);
  if (!delivered) return false;
  if (result) *result = static_cast<qint64>(static_cast<LONG_PTR>(response));
  return true;
}

HWND potWindow(qint64 processId)
{
  struct Search { DWORD pid; HWND window; } search{static_cast<DWORD>(processId), nullptr};
  EnumWindows([](HWND window, LPARAM data) -> BOOL {
    auto* search = reinterpret_cast<Search*>(data);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    wchar_t className[128] = {};
    GetClassNameW(window, className, 128);
    const QString windowClass = QString::fromWCharArray(className);
    if (pid == search->pid && (windowClass == "PotPlayer64" || windowClass == "PotPlayer")) {
      search->window = window;
      return FALSE;
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.window;
}
#endif
} // namespace

class ExternalPlayerController::Private
{
public:
  explicit Private(ExternalPlayerController* owner) : q(owner)
  {
    network.setProxy(QNetworkProxy::NoProxy);
    pollTimer.setInterval(PollMilliseconds);
    startupTimer.setSingleShot(true);
    stopTimer.setSingleShot(true);
    QObject::connect(&pollTimer, &QTimer::timeout, q, [this] { poll(); });
    QObject::connect(&startupTimer, &QTimer::timeout, q, [this] {
      fail(backend == "potplayer"
        ? QStringLiteral("PotPlayer did not become ready within 25 seconds. Check the executable and run it at the same privilege level as Jellyfin Desktop.")
        : QStringLiteral("VLC did not become ready within 25 seconds. Check its installation and local HTTP control interface."));
    });
    QObject::connect(&stopTimer, &QTimer::timeout, q, [this] { finish(false); });
  }
  ~Private()
  {
    if (process && process->state() != QProcess::NotRunning) process->kill();
    clear();
  }

  ExternalPlayerController* q;
  QNetworkAccessManager network;
  QTimer pollTimer, startupTimer, stopTimer;
  QElapsedTimer lastResponse;
  QPointer<QProcess> process;
  QPointer<QNetworkReply> reply;
  QQueue<QPair<QString, QString>> commands;
  QString session, backend, password;
  quint16 port = 0;
  quint64 generation = 0;
  qint64 position = 0, duration = 0, resume = 0;
  qint64 playlistId = -1;
  bool ready = false, sawPlayback = false, stopping = false, resumeSent = false;
  bool windowsBusy = false, muted = false, finishing = false;
  int volume = 100;

  bool matches(const QString& id) const { return !session.isEmpty() && id == session; }
  bool completed() const { return duration > 0 && position >= duration - 2000 && position > 0; }

  void clear()
  {
    ++generation;
    pollTimer.stop(); startupTimer.stop(); stopTimer.stop();
    session.clear(); commands.clear();
    if (reply) { auto* old = reply.data(); reply.clear(); old->abort(); old->deleteLater(); }
    auto* oldProcess = process.data(); process.clear(); retireProcess(oldProcess);
    ready = sawPlayback = stopping = resumeSent = windowsBusy = muted = finishing = false;
    playlistId = -1; volume = 100;
  }
  void fail(const QString& message) { finish(false, message); }
  void finish(bool didComplete, const QString& failureMessage = {})
  {
    if (session.isEmpty() || finishing) return;
    finishing = stopping = true;
    pollTimer.stop(); startupTimer.stop(); stopTimer.stop(); commands.clear();
    const auto serial = ++generation;
    if (reply) { auto* old = reply.data(); reply.clear(); old->abort(); old->deleteLater(); }
    const auto id = session;
    const auto finalPosition = position, finalDuration = duration;
    const auto report = [this, serial, id, finalPosition, finalDuration, didComplete, failureMessage] {
      if (generation != serial) return;
      clear();
      if (failureMessage.isEmpty()) emit q->ended(id, finalPosition, finalDuration, didComplete);
      else emit q->failed(id, failureMessage);
    };
    if (!process || process->state() == QProcess::NotRunning) { report(); return; }
    auto* owned = process.data();
    QObject::disconnect(owned, nullptr, q, nullptr);
    QObject::connect(owned, &QProcess::finished, q, report);
    QObject::connect(owned, &QProcess::errorOccurred, q, [report](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) report();
    });
    if (owned->state() == QProcess::Starting)
      QObject::connect(owned, &QProcess::started, owned, [owned] { owned->terminate(); });
    else owned->terminate();
    QTimer::singleShot(1000, owned, [owned] {
      if (owned->state() != QProcess::NotRunning) owned->kill();
    });
  }
  void enqueue(const QString& command, const QString& value = {})
  {
    if (session.isEmpty() || stopping) return;
    commands.enqueue({command, value});
    if (ready) poll();
  }
  void observe(qint64 nextPosition, qint64 nextDuration, const QString& state, qint64 nextPlaylist = -1)
  {
    if (state != "playing" && state != "paused" && state != "buffering" && state != "stopped") return;
    lastResponse.restart();
    if (state == "stopped") {
      // VLC clears both time and length before returning its stopped state.
      if (sawPlayback || stopping) finish(!stopping && completed());
      return;
    }
    if (nextPlaylist >= 0 && playlistId >= 0 && nextPlaylist != playlistId) {
      finish(false);
      return;
    }
    if (nextPlaylist >= 0) playlistId = nextPlaylist;
    if (nextDuration > 0) duration = nextDuration;
    ready = true;
    if (resume > 0 && !resumeSent && state != "buffering") {
      resumeSent = true;
      if (duration > 0) resume = qMin(resume, qMax<qint64>(0, duration - 1000));
      commands.prepend({"seek", QString::number(resume / 1000.0, 'f', 3)});
      return;
    }
    // At startup VLC may say playing/time=0 before applying --start-time or seek.
    // Do not overwrite the saved resume position with that transient observation.
    if (resume > 0 && nextPosition + 2000 < resume && !sawPlayback && !stopping) return;
    if (nextPosition >= 0) position = nextPosition;
    if (state == "playing" || state == "paused") {
      sawPlayback = true;
      startupTimer.stop();
    }
    if (stopping) { finish(false); return; }
    emit q->updated(session, position, duration, state);
  }
  void poll()
  {
    if (session.isEmpty() || !process || process->state() != QProcess::Running) return;
    if (sawPlayback && !stopping && lastResponse.isValid() && lastResponse.elapsed() > ControlFailureMilliseconds) {
      fail(QStringLiteral("The external player's control interface stopped responding. Playback was stopped to protect progress reporting."));
      return;
    }
    if (backend == "vlc") pollVlc();
#ifdef Q_OS_WIN
    else pollPot();
#endif
  }
  void pollVlc()
  {
    if (reply) return;
    QUrl endpoint(QStringLiteral("http://127.0.0.1:%1/requests/status.json").arg(port));
    if (ready && !stopping && !commands.isEmpty()) {
      const auto command = commands.dequeue();
      QUrlQuery query;
      query.addQueryItem("command", command.first);
      // VLC's Lua parsetime accepts integer seconds: "8.000" means zero.
      if (!command.second.isEmpty()) query.addQueryItem("val", command.first == "seek"
        ? QString::number(static_cast<qint64>(command.second.toDouble())) : command.second);
      endpoint.setQuery(query);
    }
    QNetworkRequest request(endpoint);
    request.setRawHeader("Authorization", "Basic " + (":" + password.toUtf8()).toBase64());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(RequestMilliseconds);
    auto* pending = network.get(request);
    reply = pending;
    const auto serial = generation;
    QObject::connect(pending, &QNetworkReply::finished, q, [this, pending, serial] {
      pending->deleteLater();
      if (generation != serial || session.isEmpty()) return;
      reply.clear();
      if (pending->error() == QNetworkReply::NoError) {
        const auto object = QJsonDocument::fromJson(pending->readAll()).object();
        if (object.value("time").isDouble() && object.value("length").isDouble()) {
          const double time = object.value("time").toDouble();
          const double length = object.value("length").toDouble();
          if (std::isfinite(time) && std::isfinite(length) && time >= 0 && length >= 0
              && time < 1e12 && length < 1e12) {
            if (object.contains("volume") && !muted) volume = qBound(0, qRound(object.value("volume").toDouble() * 100 / 256), 100);
            observe(qRound64(time * 1000), qRound64(length * 1000), object.value("state").toString(), object.value("currentplid").toInteger(-1));
          }
        }
      }
      if (generation != serial) return;
      if (stopping) { finish(false); return; }
      if (ready && !commands.isEmpty()) poll();
    });
  }
#ifdef Q_OS_WIN
  void pollPot()
  {
    if (windowsBusy) return;
    windowsBusy = true;
    const auto serial = generation;
    const auto pid = process->processId();
    QPair<QString, QString> command;
    if (ready && !stopping && !commands.isEmpty()) command = commands.dequeue();
    auto result = std::make_shared<PotResult>();
    // A hung or elevated player cannot block the UI; each message is bounded,
    // and only a top-level window owned by our exact QProcess PID is addressed.
    auto* worker = QThread::create([pid, command, result] {
      const HWND window = potWindow(pid);
      if (!window) return;
      result->found = true;
      bool ok = true;
      if (command.first == "pl_forcepause") ok = potMessage(window, 0x5007, 1);
      else if (command.first == "pl_forceresume") ok = potMessage(window, 0x5007, 2);
      else if (command.first == "seek") ok = potMessage(window, 0x5005, static_cast<LPARAM>(qRound64(command.second.toDouble() * 1000)));
      else if (command.first == "volume") ok = potMessage(window, 0x5001, command.second.toInt());
      else if (command.first == "mute") ok = potMessage(window, 0x5012, command.second.toInt());
      qint64 status = 0;
      result->ok = ok && potMessage(window, 0x5006, 0, &status)
        && potMessage(window, 0x5004, 0, &result->position) && potMessage(window, 0x5002, 0, &result->duration);
      result->state = static_cast<int>(status);
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(worker, &QThread::finished, q, [this, result, serial] {
      if (generation != serial || session.isEmpty()) return;
      windowsBusy = false;
      if (result->ok) observe(result->position, result->duration, result->state == 1 ? "paused" : result->state == 2 ? "playing" : "stopped");
      if (generation != serial) return;
      if (stopping) finish(false);
      else if (ready && !commands.isEmpty()) poll();
    });
    worker->start();
  }
#endif
};

ExternalPlayerController::ExternalPlayerController(QObject* parent) : QObject(parent), d(new Private(this)) {}
ExternalPlayerController::~ExternalPlayerController() { delete d; }

bool ExternalPlayerController::start(const QString& backend, const QString& executable, const QString& url, const QVariantMap& options)
{
  const QString id = options.value("sessionId").toString();
  const auto reject = [this, &id](const QString& message) { emit failed(id, message); return false; };
  if (!d->session.isEmpty()) return reject(QStringLiteral("Stop the current external playback before starting another."));
  if (id.isEmpty()) return reject(QStringLiteral("External playback requires a session identifier."));
  if (backend != "vlc" && backend != "potplayer") return reject(QStringLiteral("Select VLC or PotPlayer as the external player."));
#ifndef Q_OS_WIN
  if (backend == "potplayer") return reject(QStringLiteral("PotPlayer is only supported on Windows. Select VLC on this platform."));
#endif
  const auto subtitle = options.value("subtitleUrl").toString();
  const auto userAgent = options.value("userAgent").toString();
  if (!validMediaUrl(url) || (!subtitle.isEmpty() && !validMediaUrl(subtitle)))
    return reject(QStringLiteral("External playback accepts only valid HTTP, HTTPS, or local file media and subtitle URLs."));
  if (hasControlCharacters(userAgent)) return reject(QStringLiteral("The external player user agent contains invalid control characters."));
  if (hasControlCharacters(executable)) return reject(QStringLiteral("The external player executable path is invalid."));
  const auto program = executableFor(backend, executable);
  if (program.isEmpty()) return reject(QStringLiteral("The external player executable was not found. Install the player or set its executable path in settings."));
  d->clear();
  d->session = id; d->backend = backend;
  d->position = qMax<qint64>(0, options.value("startMilliseconds").toLongLong());
  d->duration = qMax<qint64>(0, options.value("durationMilliseconds").toLongLong());
  d->resume = d->position;
  QStringList arguments;
  if (backend == "vlc") {
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, 0)) { d->fail(QStringLiteral("Could not reserve a local VLC control port.")); return false; }
    d->port = reservation.serverPort();
    d->password = QUuid::createUuid().toString(QUuid::WithoutBraces);
    arguments << "--extraintf=http" << "--http-host=127.0.0.1" << QString("--http-port=%1").arg(d->port)
      << "--http-password=" + d->password << "--no-loop" << "--no-repeat" << "--no-play-and-exit";
#ifndef Q_OS_MACOS
    arguments << "--no-one-instance";
#endif
#ifdef Q_OS_MACOS
    arguments << "--no-macosx-recentitems" << "--macosx-continue-playback=2";
#endif
#ifdef Q_OS_WIN
    arguments << "--no-qt-recentplay" << "--qt-continue=0";
#endif
    if (d->resume > 0) arguments << "--start-time=" + QString::number(d->resume / 1000.0, 'f', 3);
    if (!subtitle.isEmpty()) arguments << "--sub-file=" + subtitle;
    if (!userAgent.isEmpty()) arguments << "--http-user-agent=" + userAgent;
    arguments << "--" << url;
  } else {
    arguments << url << "/new";
    if (!subtitle.isEmpty()) arguments << "/sub=" + subtitle;
  }
  auto* process = new QProcess(this);
  d->process = process;
  process->setProgram(program);
  process->setArguments(arguments);
  // Player output may include authenticated stream URLs; never pipe it to logs.
  process->setStandardOutputFile(QProcess::nullDevice());
  process->setStandardErrorFile(QProcess::nullDevice());
  const auto serial = d->generation;
  connect(process, &QProcess::started, this, [this, serial] {
    if (d->generation != serial) return;
    d->lastResponse.start(); d->pollTimer.start(); d->poll();
  });
  connect(process, &QProcess::errorOccurred, this, [this, serial](QProcess::ProcessError error) {
    if (d->generation != serial) return;
    if (error == QProcess::FailedToStart) d->fail(QStringLiteral("The external player could not be started. Check the executable and its permissions."));
  });
  connect(process, &QProcess::finished, this, [this, serial](int exitCode, QProcess::ExitStatus status) {
    if (d->generation != serial) return;
    if (d->stopping) d->finish(false);
    else if (!d->sawPlayback || status == QProcess::CrashExit || exitCode != 0)
      d->fail(QStringLiteral("The external player exited before playback could finish. Check that the player can open this media."));
    else d->finish(false); // A process exit is a manual close, not an observed EOF.
  });
  d->startupTimer.start(StartupMilliseconds);
  process->start();
  return true;
}

void ExternalPlayerController::stop(QString sessionId)
{
  if (!d->matches(sessionId) || d->stopping) return;
  d->stopping = true; d->commands.clear(); d->startupTimer.stop();
  d->stopTimer.start(RequestMilliseconds);
  if (!d->ready) { d->finish(false); return; }
  d->poll();
}
void ExternalPlayerController::pause(QString sessionId) { if (d->matches(sessionId)) d->enqueue("pl_forcepause"); }
void ExternalPlayerController::play(QString sessionId) { if (d->matches(sessionId)) d->enqueue("pl_forceresume"); }
void ExternalPlayerController::seekTo(QString sessionId, qint64 milliseconds)
{
  if (!d->matches(sessionId)) return;
  milliseconds = qMax<qint64>(0, milliseconds);
  if (d->duration > 0) milliseconds = qMin(milliseconds, d->duration);
  d->enqueue("seek", QString::number(milliseconds / 1000.0, 'f', 3));
}
int ExternalPlayerController::volumeArgument(const QString& backend, int percent, bool muted)
{
  const int value = backend == "vlc" && muted ? 0 : qBound(0, percent, 100);
  return backend == "vlc" ? qRound(value * 256.0 / 100) : value;
}
void ExternalPlayerController::setVolume(QString sessionId, int percent)
{
  if (!d->matches(sessionId)) return;
  d->volume = qBound(0, percent, 100);
  d->enqueue("volume", QString::number(volumeArgument(d->backend, d->volume, d->muted)));
}
void ExternalPlayerController::setMuted(QString sessionId, bool muted)
{
  if (!d->matches(sessionId)) return;
  d->muted = muted;
  if (d->backend == "potplayer") d->enqueue("mute", muted ? "1" : "0");
  else d->enqueue("volume", QString::number(muted ? 0 : qRound(d->volume * 256.0 / 100)));
}
