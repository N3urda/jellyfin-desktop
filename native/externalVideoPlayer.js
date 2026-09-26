(function () {
    let nextSession = 0;
    const milliseconds = ticks => Math.max(0, Number(ticks) / 10000 || 0);
    const clock = value => {
        const seconds = Math.floor(Math.max(0, value || 0) / 1000);
        return `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')}`;
    };

    class externalVideoPlayer {
        constructor({ events, loading, playbackManager, appHost, appSettings }) {
            this.events = events;
            this.loading = loading;
            this.playbackManager = playbackManager;
            this.appHost = appHost;
            this.appSettings = appSettings;
            this.name = 'External Video Player';
            this.type = 'mediaplayer';
            this.id = 'externalvideoplayer';
            this.priority = -2;
            this.isLocalPlayer = true;
            this.supportsProgress = true;
            this.useFullSubtitleUrls = true;
            this._currentTime = 0;
            this._duration = 0;
            this._paused = false;
            this._volume = 100;
            this._muted = false;
            this._generation = 0;
            this._transition = Promise.resolve();
        }

        async _connect() {
            if (this._bridge) return;
            const api = window.api || await window.apiPromise;
            if (!api?.externalPlayer) throw new Error('External player control is unavailable. Restart Jellyfin Desktop.');
            this._bridge = api.externalPlayer;
            this._bridge.updated.connect((...args) => this._updated(...args));
            this._bridge.ended.connect((...args) => this._ended(...args));
            this._bridge.failed.connect((id, message) => {
                const session = this._session;
                if (session?.id === id) this._failed(session, message);
            });
        }

        play(options) {
            // jellyfin-web's getPlayerData() returns this player and writes
            // streamInfo before calling play(). Restore the retiring session now:
            // its delayed error handler must report that item, not this request.
            if (this._session) this.streamInfo = this._session.options;
            const generation = ++this._generation;
            // Serialize shutdown and launch, without holding the queue until playback starts.
            // QWebChannel callbacks and native signals can arrive in either order.
            return new Promise((resolve, reject) => {
                let session;
                const launch = async () => {
                    await this._stopSession(false);
                    this.streamInfo = options;
                    this._currentPlayOptions = options;
                    this._currentSrc = options.url;
                    this._currentTime = Math.max(0, milliseconds(options.playerStartPositionTicks) - milliseconds(options.transcodingOffsetTicks));
                    this._duration = Math.max(0, milliseconds(options.mediaSource?.RunTimeTicks || options.item?.RunTimeTicks) - milliseconds(options.transcodingOffsetTicks));
                    this._paused = false;
                    session = {
                        id: `external-${Date.now()}-${++nextSession}`,
                        resolve, reject, options, accepted: false, started: false,
                        terminal: false, reported: false, stopRequested: false,
                        nativeStarted: false, nativeEnded: false, state: 'starting'
                    };
                    session.closed = new Promise(done => { session.resolveClosed = done; });
                    this._session = session;
                    // Even a superseded request needs its own reporting lifetime:
                    // jellyfin-web runs its failure handler after play() rejects.
                    if (generation !== this._generation) {
                        this._failed(session, 'External playback was canceled.');
                        return;
                    }
                    await this._connect();
                    if (session.terminal) return;
                    if (generation !== this._generation) {
                        this._failed(session, 'External playback was canceled.');
                        return;
                    }
                    this._createPanel(options);
                    const subtitle = (options.mediaSource?.MediaStreams || []).find(stream => (
                        stream.Index === options.mediaSource?.DefaultSubtitleStreamIndex &&
                        stream.Type === 'Subtitle' && stream.DeliveryMethod === 'External'
                    ));
                    session.nativeStarted = true;
                    this._bridge.load(session.id, options.url, {
                        startMilliseconds: this._currentTime,
                        durationMilliseconds: this._duration,
                        subtitleUrl: subtitle?.DeliveryUrl || '',
                        userAgent: window.jmpInfo?.userAgent || ''
                    }, accepted => {
                        if (this._session !== session || session.terminal) return;
                        if (!accepted) {
                            this._failed(session, 'The external player could not start. Check its installation and executable path in Client Settings.');
                            return;
                        }
                        session.accepted = true;
                        this._started(session);
                    });
                };
                this._transition = this._transition.then(launch, launch).catch(error => {
                    if (session) {
                        this._failed(session, 'External player control could not launch playback. Restart Jellyfin Desktop.');
                    } else {
                        this._preventTranscoding(options);
                        this.loading?.hide();
                        reject(error);
                    }
                });
            });
        }

        _started(session) {
            if (!session.accepted || session.started || session.stopRequested || session.terminal ||
                !['playing', 'paused'].includes(session.observedState)) return;
            session.started = true;
            this.loading?.hide();
            const storedVolume = this.appSettings?.get('volume');
            const savedVolume = Number(storedVolume);
            if (storedVolume != null && storedVolume !== '' && Number.isFinite(savedVolume) && savedVolume >= 0 && savedVolume <= 1) {
                this.setVolume(savedVolume * 100, false);
            }
            session.resolve();
            // playbackManager registers playbackstart listeners after play() resolves.
            Promise.resolve().then(() => {
                if (this._session !== session || session.terminal) return;
                this._emitState(session, session.observedState);
                this.events.trigger(this, 'timeupdate');
            });
        }

        _updated(id, position, duration, state) {
            const session = this._session;
            if (session?.id !== id || session.terminal || session.stopRequested) return;
            if (Number.isFinite(position) && position >= 0) this._currentTime = position;
            if (Number.isFinite(duration) && duration > 0) this._duration = duration;
            session.observedState = state;
            if (!session.started) this._started(session);
            else {
                this._emitState(session, state);
                this.events.trigger(this, 'timeupdate');
            }
            this._updatePanel();
        }

        _emitState(session, state) {
            if (session.state === state) return;
            session.state = state;
            if (state === 'paused') {
                this._paused = true;
                this.events.trigger(this, 'pause');
            } else if (state === 'playing') {
                if (this._paused) {
                    this._paused = false;
                    this.events.trigger(this, 'unpause');
                }
                this.events.trigger(this, 'playing');
            } else if (state === 'buffering') {
                this.events.trigger(this, 'waiting');
            }
            this._updatePanel();
        }

        _ended(id, position, duration, completed) {
            const session = this._session;
            if (session?.id !== id || session.terminal) return;
            // Some players reset their clock to zero as they close.
            if (Number.isFinite(position) && position > 0) this._currentTime = position;
            if (Number.isFinite(duration) && duration > 0) this._duration = duration;
            session.nativeEnded = true;
            session.resolveClosed();
            if (session.stopRequested || completed) {
                this._finish(session);
            } else {
                // A stopped payload cannot suppress Jellyfin's queue auto-advance.
                // Its public stop() method clears that flag before calling our stop().
                this._requestStop();
            }
        }

        _requestStop() {
            if (this.playbackManager?.stop) return this.playbackManager.stop(this);
            // playbackManager is also exposed by inputPlugin on older web clients.
            if (window.playbackManager?.stop) return window.playbackManager.stop(this);
            return this.stop(true);
        }

        _preventTranscoding(options) {
            // A missing executable or a failed control channel cannot be repaired by
            // transcoding. Replace this stream's descriptor without mutating the item.
            if (options?.mediaSource) {
                options.mediaSource = { ...options.mediaSource, SupportsTranscoding: false };
            }
        }

        _waitForManager(session) {
            if (session.managerFinalized) return session.managerFinalized;
            // Server-item startup rejection is followed by a deferred Jellyfin
            // error handler. Non-server URLs use an immediate stop path instead.
            if (!this.events.on || !session.options.item?.Id) return Promise.resolve();
            session.managerFinalized = new Promise(resolve => {
                const onStop = (event, state) => {
                    const stoppedItem = state?.NowPlayingItem?.Id;
                    if (stoppedItem && stoppedItem !== session.options.item.Id) return;
                    this.events.off?.(this, 'playbackstop', onStop);
                    // The manager emits playbackstop before destroying the player.
                    // Let that entire synchronous handler finish before reusing it.
                    Promise.resolve().then(() => {
                        this._clearSession(session);
                        resolve();
                    });
                };
                this.events.on(this, 'playbackstop', onStop);
            });
            return session.managerFinalized;
        }

        _rejectStartup(session, message) {
            if (session.startupRejected) return;
            session.startupRejected = true;
            this._waitForManager(session);
            session.reject(new Error(message));
        }

        _clearSession(session) {
            if (this._session === session) {
                this._session = null;
                this._currentSrc = null;
                this._currentPlayOptions = null;
            }
        }

        _failed(session, message) {
            if (session.terminal || this._session !== session) return;
            session.terminal = true;
            session.nativeEnded = true; // failed is a native shutdown acknowledgment.
            session.resolveClosed();
            this._preventTranscoding(session.options);
            this.loading?.hide();
            this._failureMessage = message || 'External playback failed. Check the player path in Client Settings.';
            this._updatePanel();
            if (!session.started) this._rejectStartup(session, this._failureMessage);
            else {
                // Jellyfin's error handler reports stop itself. Do not emit stopped too.
                session.reported = true;
                this._waitForManager(session);
                this.events.trigger(this, 'error', [{
                    type: 'mediadecodeerror',
                    // jellyfin-web logs error payloads. It needs only this flag;
                    // authenticated media/subtitle URLs must stay in session state.
                    streamInfo: { mediaSource: { SupportsTranscoding: false } }
                }]);
            }
        }

        _finish(session) {
            if (session.reported) return;
            session.reported = true;
            session.terminal = true;
            if (!session.started) {
                this._preventTranscoding(session.options);
                this._rejectStartup(session, 'External playback was canceled.');
            }
            this.loading?.hide();
            this._removePanel();
            // Reporting reads currentTime and the options synchronously inside this
            // event. Keep them intact until every stopped listener has consumed them.
            if (session.started) this.events.trigger(this, 'stopped', [{ src: this._currentSrc }]);
            if (session.started) this._clearSession(session);
        }

        _stopSession(removePanel) {
            const session = this._session;
            const panel = this._panel;
            if (!session) {
                if (removePanel) this._removePanel();
                return Promise.resolve();
            }
            if (!session.stopRequested) {
                session.stopRequested = true;
                if (!session.nativeStarted) {
                    session.nativeEnded = true;
                    session.resolveClosed();
                } else if (!session.nativeEnded) this._bridge.stop(session.id);
            }
            if (session.nativeEnded && !session.terminal) this._finish(session);
            return session.closed.then(async () => {
                if (!session.terminal) this._finish(session);
                await session.managerFinalized;
                if (removePanel && this._panel === panel) this._removePanel();
            });
        }

        stop(destroyPlayer) {
            ++this._generation;
            return this._stopSession(!!destroyPlayer);
        }

        destroy() {
            // Retain an actionable native error until the user dismisses it.
            return this._stopSession(!this._failureMessage);
        }

        _createPanel(options) {
            this._removePanel();
            this._failureMessage = '';
            this._trackHint = '';
            const panel = document.createElement('section');
            panel.className = 'externalPlaybackPanel';
            panel.setAttribute('aria-label', 'External video playback');
            panel.style.cssText = 'position:fixed;bottom:1.5rem;left:1rem;right:1rem;max-width:38rem;margin:auto;padding:1rem;background:#202020;color:#fff;border:1px solid #555;border-radius:.5rem;z-index:10000;box-shadow:0 .2rem 1rem #0008;';
            const heading = document.createElement('strong');
            const selected = window.jmpInfo?.settings?.main?.externalVideoPlayer;
            const playerName = selected === 'potplayer' ? 'PotPlayer' : 'VLC';
            heading.textContent = `${playerName} — ${options.item?.Name || 'Video'}`;
            panel.appendChild(heading);
            this._panelStatus = document.createElement('p');
            this._panelStatus.setAttribute('role', 'status');
            panel.appendChild(this._panelStatus);
            this._panelTime = document.createElement('p');
            panel.appendChild(this._panelTime);
            this._panelPause = document.createElement('button');
            this._panelPause.type = 'button';
            this._panelPause.className = 'raised emby-button';
            this._panelPause.style.marginRight = '.75rem';
            this._panelPause.addEventListener('click', () => this._paused ? this.unpause() : this.pause());
            panel.appendChild(this._panelPause);
            this._panelStop = document.createElement('button');
            this._panelStop.type = 'button';
            this._panelStop.className = 'raised emby-button';
            this._panelStop.addEventListener('click', () => {
                if (this._failureMessage) this._removePanel();
                else this._requestStop();
            });
            panel.appendChild(this._panelStop);
            this._panel = panel;
            document.body.appendChild(panel);
            this._updatePanel();
        }

        _updatePanel() {
            if (!this._panel) return;
            const state = this._session?.observedState;
            this._panelStatus.textContent = this._failureMessage || this._trackHint || (
                state === 'playing' ? 'Playing in your external player. Audio and subtitle controls are available there.' :
                state === 'paused' ? 'Paused in your external player.' :
                state === 'buffering' ? 'Buffering in your external player…' : 'Opening your external player…'
            );
            const options = this._currentPlayOptions;
            const displayPosition = this._currentTime + milliseconds(options?.transcodingOffsetTicks);
            const displayDuration = milliseconds(options?.mediaSource?.RunTimeTicks || options?.item?.RunTimeTicks) || this._duration;
            this._panelTime.textContent = `${clock(displayPosition)} / ${displayDuration ? clock(displayDuration) : '—'}`;
            this._panelPause.textContent = this._paused ? 'Resume' : 'Pause';
            this._panelPause.disabled = !!this._failureMessage || !this._session?.started;
            this._panelStop.textContent = this._failureMessage ? 'Dismiss' : 'Stop and return';
        }

        _removePanel() {
            this._panel?.remove();
            this._panel = null;
        }

        _command(method, ...args) {
            const session = this._session;
            if (session && !session.terminal && !session.stopRequested) this._bridge[method](session.id, ...args);
        }

        currentSrc() { return this._currentSrc; }
        currentTime(value) {
            if (value != null) {
                const position = Number(value);
                if (Number.isFinite(position) && position >= 0) this._command('seekTo', position);
                return;
            }
            return this._currentTime;
        }
        currentTimeAsync() { return Promise.resolve(this._currentTime); }
        duration() { return this._duration || null; }
        seekable() {
            // A progressive transcode begins at its server offset. Let Jellyfin
            // request a new stream when seeking, including before that offset.
            return this._duration > 0 && !this._currentPlayOptions?.transcodingOffsetTicks;
        }
        paused() { return this._paused; }
        pause() { this._command('pause'); }
        unpause() { this._command('play'); }
        resume() { this.unpause(); }
        getVolume() { return this._volume; }
        setVolume(value, save = true) {
            const number = Number(value);
            if (!Number.isFinite(number)) return;
            this._volume = Math.max(0, Math.min(100, number));
            this._command('setVolume', this._volume);
            if (save) this.appSettings?.set('volume', this._volume / 100);
            this.events.trigger(this, 'volumechange');
        }
        volumeUp() { this.setVolume(this._volume + 2); }
        volumeDown() { this.setVolume(this._volume - 2); }
        setMute(value) {
            this._muted = !!value;
            this._command('setMuted', this._muted);
            this.events.trigger(this, 'volumechange');
        }
        isMuted() { return this._muted; }
        canPlayMediaType(type) { return String(type || '').toLowerCase() === 'video'; }
        canPlayItem(item) { return this.canPlayMediaType(item.MediaType); }
        supportsPlayMethod() { return true; }
        getDeviceProfile(item, options) { return Promise.resolve(this.appHost.getDeviceProfile(item, options)); }
        supports() { return false; }
        canSetAudioStreamIndex() { return false; }
        canSetSubtitleStreamIndex() { return false; }
        // Some web clients call these despite the capability flag. The external
        // window owns track selection; never claim that its active track changed.
        setSubtitleStreamIndex() {
            this._trackHint = 'Use your external player’s subtitle menu to change subtitle tracks.';
            this._updatePanel();
        }
        setSecondarySubtitleStreamIndex(index) {
            if (index >= 0) this.setSubtitleStreamIndex();
        }
        isFullscreen() { return false; }
        isPictureInPictureEnabled() { return false; }
        isAirPlayEnabled() { return false; }
        getBufferedRanges() { return []; }
        getStats() { return Promise.resolve({ categories: [] }); }
    }

    window._externalVideoPlayer = externalVideoPlayer;
})();
