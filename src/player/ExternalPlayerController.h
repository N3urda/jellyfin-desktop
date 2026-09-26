#pragma once

#include <QObject>
#include <QVariantMap>

class ExternalPlayerController : public QObject
{
  Q_OBJECT
public:
  explicit ExternalPlayerController(QObject* parent = nullptr);
  ~ExternalPlayerController() override;
  bool start(const QString& backend, const QString& executable, const QString& url, const QVariantMap& options);
public slots:
  void stop(QString sessionId);
  void pause(QString sessionId);
  void play(QString sessionId);
  void seekTo(QString sessionId, qint64 milliseconds);
  void setVolume(QString sessionId, int percent);
  void setMuted(QString sessionId, bool muted);
signals:
  void updated(QString sessionId, qint64 position, qint64 duration, QString state);
  void ended(QString sessionId, qint64 position, qint64 duration, bool completed);
  void failed(QString sessionId, QString message);
private:
  friend class ExternalPlayerControllerTest;
  static int volumeArgument(const QString& backend, int percent, bool muted);
  class Private;
  Private* d;
};
