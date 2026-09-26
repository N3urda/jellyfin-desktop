#include "ExternalPlayerComponent.h"
#include "settings/SettingsComponent.h"

ExternalPlayerComponent::ExternalPlayerComponent(QObject* parent)
  : ComponentBase(parent)
{
  connect(&m_controller, &ExternalPlayerController::updated, this, &ExternalPlayerComponent::updated);
  connect(&m_controller, &ExternalPlayerController::ended, this, &ExternalPlayerComponent::ended);
  connect(&m_controller, &ExternalPlayerController::failed, this, &ExternalPlayerComponent::failed);
}

bool ExternalPlayerComponent::componentInitialize()
{
  return true;
}

bool ExternalPlayerComponent::load(const QString& sessionId, const QString& url, const QVariantMap& options)
{
  auto& settings = SettingsComponent::Get();
  const QString backend = settings.value(SETTINGS_SECTION_MAIN, "externalVideoPlayer").toString();
  if (backend != "vlc" && backend != "potplayer")
  {
    emit failed(sessionId, QStringLiteral("Select VLC or PotPlayer in Client Settings, then restart Jellyfin Desktop."));
    return false;
  }
  const QString pathKey = backend == "vlc" ? "vlcPath" : "potPlayerPath";
  QVariantMap launchOptions = options;
  launchOptions.insert("sessionId", sessionId);
  return m_controller.start(backend, settings.value(SETTINGS_SECTION_MAIN, pathKey).toString(), url, launchOptions);
}
