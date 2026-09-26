const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const vm = require('node:vm');

const sourcePath = path.join(__dirname, '../native/externalVideoPlayer.js');
const tick = () => new Promise(resolve => setImmediate(resolve));

function signal() {
    const listeners = new Set();
    return {
        connect: fn => listeners.add(fn),
        disconnect: fn => listeners.delete(fn),
        emit: (...args) => [...listeners].forEach(fn => fn(...args))
    };
}

function element(tag) {
    return {
        tag, children: [], style: {}, textContent: '', disabled: false,
        setAttribute(name, value) { this[name] = value; },
        appendChild(child) { this.children.push(child); child.parentNode = this; return child; },
        addEventListener(name, fn) { this[name] = fn; },
        remove() { if (this.parentNode) this.parentNode.children = this.parentNode.children.filter(x => x !== this); }
    };
}

function harness({ accept = true, autoClose = true, integratedEvents = false, savedVolume } = {}) {
    const calls = [];
    const events = [];
    const bridge = { updated: signal(), ended: signal(), failed: signal() };
    bridge.load = (id, url, options, callback) => {
        calls.push({ method: 'load', id, url, options, callback });
        if (accept !== null) callback(accept);
    };
    bridge.stop = id => {
        calls.push({ method: 'stop', id });
        if (autoClose) bridge.ended.emit(id, 0, 0, false);
    };
    for (const method of ['pause', 'play', 'seekTo', 'setVolume', 'setMuted']) {
        bridge[method] = (...args) => calls.push({ method, args });
    }
    const document = { body: element('body'), createElement: element };
    const window = { api: { externalPlayer: bridge }, jmpInfo: { settings: { main: { externalVideoPlayer: 'vlc' } }, userAgent: 'Jellyfin Desktop' } };
    const context = vm.createContext({ window, document, console, setTimeout, clearTimeout, Promise, Date, Math, Error });
    if (fs.existsSync(sourcePath)) vm.runInContext(fs.readFileSync(sourcePath, 'utf8'), context);
    assert.equal(typeof window._externalVideoPlayer, 'function', 'external player plugin is exported');
    let manualStops = 0;
    let nextEnabled = true;
    const manager = {
        stop(player) { manualStops++; nextEnabled = false; return player.stop(true); }
    };
    let listener;
    const callbacks = new Map();
    const eventBus = {
        trigger(target, name, args = []) {
            events.push({ name, args, time: target.currentTime(), options: target._currentPlayOptions, nextEnabled });
            if (listener) listener(target, name, args);
            for (const callback of [...(callbacks.get(name) || [])]) callback.call(target, { type: name }, ...args);
        }
    };
    if (integratedEvents) {
        eventBus.on = (target, name, fn) => callbacks.set(name, [...(callbacks.get(name) || []), fn]);
        eventBus.off = (target, name, fn) => callbacks.set(name, (callbacks.get(name) || []).filter(x => x !== fn));
    }
    const player = new window._externalVideoPlayer({
        events: eventBus,
        loading: { hide() { calls.push({ method: 'hide' }); }, show() {} },
        playbackManager: manager,
        appHost: { getDeviceProfile() { return Promise.resolve({ DirectPlayProfiles: [{ Type: 'Video' }], TranscodingProfiles: [{ Type: 'Video' }], SubtitleProfiles: [{ Format: 'srt', Method: 'External' }] }); } },
        appSettings: { get() { return savedVolume; }, set() {} }
    });
    return { player, bridge, calls, events, eventBus, document, window, get manualStops() { return manualStops; }, listen(fn) { listener = fn; } };
}

// jellyfin-web calls onPlaybackStarted even when play() rejects, then invokes
// onPlaybackError from a later timer. Let tests explicitly release that callback.
function upstreamStartup(h) {
    const captures = [];
    const delayedErrors = [];
    return {
        captures, delayedErrors,
        play(opts) {
            // getPlayerData(player) returns the player. Network responses write
            // streamInfo before calling the plugin, even for overlapping requests.
            h.player.streamInfo = opts;
            const pending = h.player.play(opts);
            pending.then(() => {
                h.player.streamInfo = opts;
                captures.push({ phase: 'start', item: opts.item.Id, options: h.player._currentPlayOptions, position: h.player.currentTime() });
            }, () => {
                h.player.streamInfo = opts;
                captures.push({ phase: 'failed-start', item: opts.item.Id, options: h.player._currentPlayOptions, position: h.player.currentTime() });
                delayedErrors.push(() => {
                    const current = h.player.streamInfo;
                    captures.push({ phase: 'failed-stop', item: current?.item?.Id, options: h.player._currentPlayOptions, position: h.player.currentTime() });
                    h.eventBus.trigger(h.player, 'playbackstop', [{ NowPlayingItem: current?.item }]);
                    h.player.streamInfo = null;
                    // The real manager destroys synchronously after playbackstop.
                    h.player.destroy();
                });
            });
            return pending;
        }
    };
}

function options(overrides = {}) {
    return {
        url: 'https://media.example/videos/1/stream?api_key=secret',
        playerStartPositionTicks: 120000000,
        item: { Id: 'item-1', Name: 'Episode One', RunTimeTicks: 6000000000 },
        mediaSource: { RunTimeTicks: 6000000000, SupportsTranscoding: true, DefaultSubtitleStreamIndex: 4, MediaStreams: [{ Index: 4, Type: 'Subtitle', DeliveryMethod: 'External', DeliveryUrl: 'https://media.example/subtitle.vtt?api_key=secret' }] },
        ...overrides
    };
}

async function start(h, opts = options()) {
    const pending = h.player.play(opts);
    await tick();
    const load = h.calls.filter(x => x.method === 'load').at(-1);
    h.bridge.updated.emit(load.id, 12000, 600000, 'playing');
    await pending;
    return load;
}

test('launch sends stream-relative resume and external subtitle, waits for actual playback', async () => {
    const h = harness();
    let resolved = false;
    const pending = h.player.play(options()).then(() => { resolved = true; });
    await tick();
    const load = h.calls.find(x => x.method === 'load');
    assert.equal(load.options.startMilliseconds, 12000);
    assert.equal(load.options.durationMilliseconds, 600000);
    assert.equal(load.options.subtitleUrl, options().mediaSource.MediaStreams[0].DeliveryUrl);
    assert.equal(resolved, false);
    h.bridge.updated.emit(load.id, 12000, 600000, 'playing');
    await pending;
    assert.equal(h.player.currentTime(), 12000);
    assert.equal(h.player.duration(), 600000);
    assert.equal(h.document.body.children.length, 1);
    await h.player.stop(true);
});

test('observed pause, progress and resume become Jellyfin events; controls keep session identity', async () => {
    const h = harness();
    const load = await start(h);
    h.bridge.updated.emit(load.id, 13000, 600000, 'paused');
    assert.equal(h.player.paused(), true);
    h.player.unpause();
    h.player.currentTime(50000);
    h.bridge.updated.emit(load.id, 50000, 600000, 'playing');
    assert.equal(h.player.paused(), false);
    assert.ok(h.events.some(e => e.name === 'pause'));
    assert.ok(h.events.some(e => e.name === 'unpause'));
    assert.ok(h.events.some(e => e.name === 'timeupdate' && e.time === 50000));
    assert.deepEqual(h.calls.find(x => x.method === 'seekTo').args, [load.id, 50000]);
    await h.player.stop(true);
});

test('manual player close disables auto-next and preserves final position while stopped listeners run', async () => {
    const h = harness();
    const original = options();
    const load = await start(h, original);
    h.bridge.updated.emit(load.id, 45000, 600000, 'playing');
    h.bridge.ended.emit(load.id, 0, 0, false);
    await tick();
    const stopped = h.events.filter(e => e.name === 'stopped');
    assert.equal(h.manualStops, 1);
    assert.equal(stopped.length, 1);
    assert.equal(stopped[0].time, 45000);
    assert.equal(stopped[0].options, original);
    assert.equal(stopped[0].nextEnabled, false);
    assert.equal(h.document.body.children.length, 0);
});

test('natural completion allows auto-next and duplicate native terminal events do not repeat stop', async () => {
    const h = harness();
    const load = await start(h);
    h.bridge.ended.emit(load.id, 600000, 600000, true);
    h.bridge.ended.emit(load.id, 600000, 600000, true);
    h.bridge.failed.emit(load.id, 'late failure');
    assert.equal(h.manualStops, 0);
    const stopped = h.events.filter(e => e.name === 'stopped');
    assert.equal(stopped.length, 1);
    assert.equal(stopped[0].time, 600000);
    assert.equal(stopped[0].nextEnabled, true);
});

test('replacement waits for native shutdown and ignores old process callbacks', async () => {
    const h = harness({ autoClose: false });
    const first = await start(h);
    const secondPromise = h.player.play(options({ url: 'https://media.example/second' }));
    await tick();
    assert.equal(h.calls.filter(x => x.method === 'load').length, 1);
    h.bridge.ended.emit(first.id, 20000, 600000, false);
    await tick();
    const second = h.calls.filter(x => x.method === 'load').at(-1);
    assert.notEqual(second.id, first.id);
    h.bridge.updated.emit(first.id, 599000, 600000, 'paused');
    h.bridge.failed.emit(first.id, 'stale');
    h.bridge.updated.emit(second.id, 14000, 600000, 'playing');
    await secondPromise;
    assert.equal(h.player.currentTime(), 14000);
    assert.equal(h.player.currentSrc(), 'https://media.example/second');
    const closing = h.player.stop(true);
    h.bridge.ended.emit(second.id, 14000, 600000, false);
    await closing;
});

test('rejected launch rejects play and prevents retrying a missing executable through transcoding', async () => {
    const h = harness({ accept: false });
    const original = options();
    const oldSource = original.mediaSource;
    await assert.rejects(h.player.play(original));
    assert.equal(original.mediaSource.SupportsTranscoding, false);
    assert.equal(oldSource.SupportsTranscoding, true);
    assert.equal(h.events.filter(e => e.name === 'playing').length, 0);
    assert.ok(h.calls.some(x => x.method === 'hide'));
    await h.player.stop(true);
});

test('runtime error reports final state once and disables automatic transcoding retry', async () => {
    const h = harness();
    const load = await start(h);
    h.bridge.updated.emit(load.id, 55000, 600000, 'playing');
    h.bridge.failed.emit(load.id, 'VLC control connection lost');
    h.bridge.failed.emit(load.id, 'duplicate');
    const errors = h.events.filter(e => e.name === 'error');
    assert.equal(errors.length, 1);
    assert.equal(errors[0].time, 55000);
    assert.equal(errors[0].args[0].streamInfo.mediaSource.SupportsTranscoding, false);
    const serialized = JSON.stringify(errors[0].args[0]);
    assert.equal(serialized.includes('secret'), false);
    assert.equal(serialized.includes('media.example'), false);
    assert.equal(serialized.includes('api_key'), false);
    assert.equal(errors[0].args[0].streamInfo.url, undefined);
    assert.equal(errors[0].args[0].streamInfo.headers, undefined);
    await h.player.stop(true);
});

test('plugin advertises video with progress and no unsupported advanced controls', async () => {
    const h = harness();
    assert.equal(h.player.canPlayMediaType('Video'), true);
    assert.equal(h.player.canPlayMediaType('Audio'), false);
    assert.equal(h.player.supports('PlaybackRate'), false);
    assert.equal(h.player.supports('SetAspectRatio'), false);
    assert.equal(h.player.canSetAudioStreamIndex(), false);
    assert.equal(h.player.syncPlayWrapAs, undefined);
    assert.equal(h.player.useFullSubtitleUrls, true);
});

test('playback observations arriving before load acknowledgment are retained', async () => {
    const h = harness({ accept: null });
    const pending = h.player.play(options());
    await tick();
    const load = h.calls.find(x => x.method === 'load');
    h.bridge.updated.emit(load.id, 24000, 600000, 'paused');
    load.callback(true);
    await pending;
    await tick();
    assert.equal(h.player.currentTime(), 24000);
    assert.equal(h.player.paused(), true);
    assert.equal(h.events.filter(e => e.name === 'pause').length, 1);
    await h.player.stop(true);
});

test('failure before the load callback remains terminal and accepts a new session', async () => {
    const h = harness({ accept: null });
    const pending = h.player.play(options());
    const rejected = assert.rejects(pending, /not installed/);
    await tick();
    const first = h.calls.find(x => x.method === 'load');
    h.bridge.failed.emit(first.id, 'VLC is not installed');
    first.callback(true);
    h.bridge.updated.emit(first.id, 12000, 600000, 'playing');
    await rejected;
    const nextPromise = h.player.play(options({ url: 'https://media.example/next' }));
    await tick();
    const next = h.calls.filter(x => x.method === 'load').at(-1);
    next.callback(true);
    h.bridge.updated.emit(next.id, 16000, 600000, 'playing');
    await nextPromise;
    assert.equal(h.events.filter(e => e.name === 'playing').length, 1);
    assert.equal(h.player.currentTime(), 16000);
    await h.player.stop(true);
});

test('repeated stop commands wait for one native exit and emit one final report', async () => {
    const h = harness({ autoClose: false });
    const load = await start(h);
    const first = h.player.stop(true);
    const second = h.player.stop(true);
    assert.equal(h.calls.filter(x => x.method === 'stop').length, 1);
    h.bridge.ended.emit(load.id, 67000, 600000, false);
    await Promise.all([first, second]);
    assert.equal(h.events.filter(e => e.name === 'stopped').length, 1);
    assert.equal(h.events.find(e => e.name === 'stopped').time, 67000);
});

test('external playback panel controls pause and stop without replacing the browser document', async () => {
    const h = harness();
    const existingContent = element('main');
    h.document.body.appendChild(existingContent);
    const load = await start(h);
    const panel = h.document.body.children.find(x => x.className === 'externalPlaybackPanel');
    const buttons = panel.children.filter(x => x.tag === 'button');
    assert.match(panel.children[0].textContent, /VLC.*Episode One/);
    buttons[0].click();
    assert.deepEqual(h.calls.find(x => x.method === 'pause').args, [load.id]);
    buttons[1].click();
    await tick();
    assert.equal(h.manualStops, 1);
    assert.deepEqual(h.document.body.children, [existingContent]);
});

test('compatibility subtitle methods give a visible unsupported hint without pretending to control tracks', async () => {
    const h = harness();
    await start(h);
    h.player.setSubtitleStreamIndex(1);
    h.player.setSecondarySubtitleStreamIndex(-1);
    const panel = h.document.body.children[0];
    assert.ok(panel.children.some(x => /external player.*subtitle/i.test(x.textContent)));
    assert.equal(h.calls.filter(x => x.method === 'load').length, 1);
    await h.player.stop(true);
});

test('canceling before playback starts rejects launch without emitting a duplicate stopped event', async () => {
    const h = harness({ accept: null });
    const pending = h.player.play(options());
    const rejected = assert.rejects(pending, /canceled/);
    await tick();
    const load = h.calls.find(x => x.method === 'load');
    await h.player.stop(true);
    load.callback(true);
    h.bridge.updated.emit(load.id, 12000, 600000, 'playing');
    await rejected;
    assert.equal(h.events.filter(e => e.name === 'stopped').length, 0);
    assert.equal(h.events.filter(e => e.name === 'playing').length, 0);
});

test('transcode offset is displayed but not added to the stream-relative clock', async () => {
    const h = harness();
    const load = await start(h, options({ playerStartPositionTicks: 1200000000, transcodingOffsetTicks: 1200000000 }));
    h.bridge.updated.emit(load.id, 5000, 480000, 'playing');
    assert.equal(load.options.startMilliseconds, 0);
    assert.equal(load.options.durationMilliseconds, 480000);
    assert.equal(h.player.seekable(), false);
    assert.equal(h.player.currentTime(), 5000);
    assert.ok(h.document.body.children[0].children.some(x => x.textContent === '2:05 / 10:00'));
    await h.player.stop(true);
});

test('replacement waits for the old rejected startup to finish Jellyfin reporting before launching', async () => {
    const h = harness({ accept: null, integratedEvents: true });
    const upstream = upstreamStartup(h);
    const firstOptions = options();
    const firstPromise = upstream.play(firstOptions);
    const firstRejected = assert.rejects(firstPromise, /canceled/);
    await tick();
    const first = h.calls.find(x => x.method === 'load');
    const secondOptions = options({ url: 'https://media.example/second', item: { Id: 'item-2', Name: 'Episode Two' } });
    const secondPromise = upstream.play(secondOptions);
    await firstRejected;
    await tick();
    assert.equal(h.calls.filter(x => x.method === 'load').length, 1);
    assert.equal(upstream.captures[0].options, firstOptions);
    assert.equal(h.player.currentTime(), 12000);
    assert.equal(upstream.delayedErrors.length, 1);
    upstream.delayedErrors.shift()();
    await tick();
    assert.equal(upstream.captures.find(x => x.phase === 'failed-stop').options, firstOptions);
    const second = h.calls.filter(x => x.method === 'load').at(-1);
    assert.notEqual(second.id, first.id);
    second.callback(true);
    h.bridge.updated.emit(second.id, 20000, 600000, 'playing');
    await secondPromise;
    assert.equal(h.player.currentSrc(), secondOptions.url);
    assert.equal(h.calls.filter(x => x.method === 'stop' && x.id === second.id).length, 0);
    assert.equal(h.document.body.children.length, 1);
    await h.player.stop(true);
});

test('a superseded request before native launch still waits for upstream rejection cleanup', async () => {
    const h = harness({ accept: null, integratedEvents: true });
    const upstream = upstreamStartup(h);
    const firstOptions = options();
    const firstPromise = upstream.play(firstOptions);
    const firstRejected = assert.rejects(firstPromise, /canceled/);
    const secondOptions = options({ item: { Id: 'item-2' } });
    const secondPromise = upstream.play(secondOptions);
    await firstRejected;
    await tick();
    assert.equal(h.calls.filter(x => x.method === 'load').length, 0);
    assert.equal(upstream.captures[0].options, firstOptions);
    upstream.delayedErrors.shift()();
    await tick();
    const second = h.calls.find(x => x.method === 'load');
    second.callback(true);
    h.bridge.updated.emit(second.id, 13000, 600000, 'playing');
    await secondPromise;
    assert.equal(h.player.currentTime(), 13000);
    await h.player.stop(true);
});

test('native startup failure waits for upstream failure acknowledgment before a retry', async () => {
    const h = harness({ accept: null, integratedEvents: true });
    const upstream = upstreamStartup(h);
    const firstPromise = upstream.play(options());
    const firstRejected = assert.rejects(firstPromise, /not installed/);
    await tick();
    const first = h.calls.find(x => x.method === 'load');
    h.bridge.failed.emit(first.id, 'VLC is not installed');
    await firstRejected;
    const secondOptions = options({ item: { Id: 'item-2' } });
    const secondPromise = upstream.play(secondOptions);
    await tick();
    assert.equal(h.calls.filter(x => x.method === 'load').length, 1);
    assert.equal(h.player.streamInfo.item.Id, 'item-1');
    assert.equal(secondOptions.item.Id, 'item-2');
    assert.equal(secondOptions.mediaSource.SupportsTranscoding, true);
    upstream.delayedErrors.shift()();
    assert.equal(upstream.captures.find(x => x.phase === 'failed-stop').item, 'item-1');
    await tick();
    const second = h.calls.filter(x => x.method === 'load').at(-1);
    assert.equal(h.player.streamInfo, secondOptions);
    second.callback(true);
    h.bridge.updated.emit(second.id, 17000, 600000, 'playing');
    await secondPromise;
    assert.equal(h.player.currentTime(), 17000);
    assert.equal(h.calls.filter(x => x.method === 'stop' && x.id === second.id).length, 0);
    await h.player.stop(true);
});

test('multiple queued replacements retain the correct shared streamInfo for each rejected request', async () => {
    const h = harness({ accept: null, integratedEvents: true });
    const upstream = upstreamStartup(h);
    const firstPromise = upstream.play(options());
    const firstRejected = assert.rejects(firstPromise, /not installed/);
    await tick();
    const first = h.calls.find(x => x.method === 'load');
    h.bridge.failed.emit(first.id, 'VLC is not installed');
    await firstRejected;
    const secondOptions = options({ item: { Id: 'item-2' } });
    const secondPromise = upstream.play(secondOptions);
    const secondRejected = assert.rejects(secondPromise, /canceled/);
    const thirdOptions = options({ item: { Id: 'item-3' } });
    const thirdPromise = upstream.play(thirdOptions);
    assert.equal(h.player.streamInfo.item.Id, 'item-1');
    upstream.delayedErrors.shift()();
    await secondRejected;
    await tick();
    assert.equal(h.calls.filter(x => x.method === 'load').length, 1);
    assert.equal(h.player.streamInfo, secondOptions);
    upstream.delayedErrors.shift()();
    await tick();
    assert.deepEqual(upstream.captures.filter(x => x.phase === 'failed-stop').map(x => x.item), ['item-1', 'item-2']);
    const third = h.calls.filter(x => x.method === 'load').at(-1);
    assert.equal(h.player.streamInfo, thirdOptions);
    third.callback(true);
    h.bridge.updated.emit(third.id, 15000, 600000, 'playing');
    await thirdPromise;
    assert.equal(h.player.currentTime(), 15000);
    await h.player.stop(true);
});

test('a bridge launch exception participates in the upstream reporting barrier', async () => {
    const h = harness({ accept: null, integratedEvents: true });
    const upstream = upstreamStartup(h);
    const originalLoad = h.bridge.load;
    h.bridge.load = () => { throw new Error('Bridge disconnected'); };
    const firstPromise = upstream.play(options());
    await assert.rejects(firstPromise, /control could not launch/);
    h.bridge.load = originalLoad;
    const nextPromise = upstream.play(options({ item: { Id: 'item-2' } }));
    await tick();
    assert.equal(h.calls.filter(x => x.method === 'load').length, 0);
    upstream.delayedErrors.shift()();
    await tick();
    const next = h.calls.find(x => x.method === 'load');
    next.callback(true);
    h.bridge.updated.emit(next.id, 13000, 600000, 'playing');
    await nextPromise;
    assert.equal(h.player.currentTime(), 13000);
    await h.player.stop(true);
});

test('canceling while the WebChannel connects cannot launch the canceled media later', async () => {
    const h = harness();
    const api = h.window.api;
    delete h.window.api;
    let connect;
    h.window.apiPromise = new Promise(resolve => { connect = resolve; });
    const pending = h.player.play(options());
    const rejected = assert.rejects(pending, /canceled/);
    await tick();
    await h.player.stop(true);
    connect(api);
    await rejected;
    await tick();
    assert.equal(h.calls.filter(x => x.method === 'load').length, 0);
});

test('missing saved volume does not mute first playback while an explicit zero is preserved', async () => {
    for (const savedVolume of [null, undefined, '']) {
        const h = harness({ savedVolume });
        await start(h);
        assert.equal(h.calls.some(x => x.method === 'setVolume' && x.args[1] === 0), false);
        await h.player.stop(true);
    }
    const h = harness({ savedVolume: '0' });
    await start(h);
    assert.equal(h.calls.some(x => x.method === 'setVolume' && x.args[1] === 0), true);
    await h.player.stop(true);
});
