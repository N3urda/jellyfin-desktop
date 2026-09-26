#ifndef EXTERNALPLAYERCOMPONENT_H
#define EXTERNALPLAYERCOMPONENT_H

#include "ComponentManager.h"
#include "ExternalPlayerController.h"

// Keeps executable selection in native settings, separate from web playback data.
class ExternalPlayerComponent : public ComponentBase
{
  Q_OBJECT
  DEFINE_SINGLETON(ExternalPlayerComponent);

public:
  explicit ExternalPlayerComponent(QObject* parent = nullptr);
  const char* componentName() override { return "externalPlayer"; }
  bool componentExport() override { return true; }
  bool componentInitialize() override;

  Q_INVOKABLE bool load(const QString& sessionId, const QString& url, const QVariantMap& options);
  Q_INVOKABLE void stop(const QString& sessionId) { m_controller.stop(sessionId); }
  Q_INVOKABLE void pause(const QString& sessionId) { m_controller.pause(sessionId); }
  Q_INVOKABLE void play(const QString& sessionId) { m_controller.play(sessionId); }
  Q_INVOKABLE void seekTo(const QString& sessionId, qint64 ms) { m_controller.seekTo(sessionId, ms); }
  Q_INVOKABLE void setVolume(const QString& sessionId, int volume) { m_controller.setVolume(sessionId, volume); }
  Q_INVOKABLE void setMuted(const QString& sessionId, bool muted) { m_controller.setMuted(sessionId, muted); }

Q_SIGNALS:
  void updated(const QString& sessionId, qint64 position, qint64 duration, const QString& state);
  void ended(const QString& sessionId, qint64 position, qint64 duration, bool completed);
  void failed(const QString& sessionId, const QString& message);

private:
  ExternalPlayerController m_controller;
};

#endif
