/*
  Q Light Controller Plus
  trackmanager.h

  Receives track structure and playback position from Beat Link Trigger, derives
  the current section of the track, and fires a "look" for each section.

  A look is one Function per slot, where slots mirror the groups in the rig. A
  slot can be bound to a Function folder and set to RANDOM, so each time the
  section is entered it picks a different Function from that folder.

  Each section also carries a beat division. It is passed to Function::start()
  as an overrideDuration with overrideTempoType Beats, so Ableton Link remains
  the only timing source and we only change how many beats a step lasts.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

#ifndef TRACKMANAGER_H
#define TRACKMANAGER_H

#include <QVariantList>
#include <QJsonObject>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QSet>                    // R459_RM_INC
#include <QJsonArray>
#include <QElapsedTimer>             // R450_TT_INC
#include <QHash>
#include <QList>
#include <QMap>

class QTcpServer;
class QTcpSocket;
class QQuickView;
class TrackEngine;
class QProcess;                          // R465_BLT_LIVE_FWD
class Function;
class Doc;

#define SETTINGS_TRACK_PORT       QStringLiteral("trackmanager/port")
#define SETTINGS_TRACK_BPMLOW     QStringLiteral("trackmanager/bpmlow")
#define SETTINGS_TRACK_BPMHIGH    QStringLiteral("trackmanager/bpmhigh")
#define SETTINGS_TRACK_TRIM       QStringLiteral("trackmanager/trim")
#define SETTINGS_TRACK_QUANTIZE   QStringLiteral("trackmanager/quantize")
#define SETTINGS_TRACK_LOOKS      QStringLiteral("trackmanager/looks")
#define SETTINGS_TRACK_RANDOM     QStringLiteral("trackmanager/random")
#define SETTINGS_TRACK_INTENSITY  QStringLiteral("trackmanager/intensity")
#define SETTINGS_TRACK_DIVISION   QStringLiteral("trackmanager/division")
#define SETTINGS_TRACK_SLOTNAMES  QStringLiteral("trackmanager/slotnames")
#define SETTINGS_TRACK_SLOTDIRS   QStringLiteral("trackmanager/slotdirs")
#define SETTINGS_TRACK_SLOTSPEED  QStringLiteral("trackmanager/slotspeed")
#define SETTINGS_TRACK_DROPKICK   QStringLiteral("trackmanager/dropkick")
#define SETTINGS_TRACK_BREAKKICK  QStringLiteral("trackmanager/breakkick")
#define SETTINGS_TRACK_SHOWRAN    QStringLiteral("trackmanager/showran")   // R184_START_SCENE: the night it ran

#define TRACK_DEFAULT_PORT     9998
#define TRACK_DEFAULT_BPM_LOW  80
#define TRACK_DEFAULT_BPM_HIGH 140
#define TRACK_SLOT_COUNT       6

/* Roles replace slots for automatic operation. A role is what a look DOES
 * in the rig, not which fixture it lives on, so the engine can pick a
 * function for each role on its own. */
#define TRACK_ROLE_COUNT       5
#define TRACK_ROLE_COLOR       0
#define TRACK_ROLE_MOVEMENT    1
#define TRACK_ROLE_BEAM        2
#define TRACK_ROLE_STROBE      3
#define TRACK_ROLE_DARK        4

#define SETTINGS_TRACK_ROLES    QStringLiteral("trackmanager/roles")
#define SETTINGS_TRACK_ROLEMODE QStringLiteral("trackmanager/rolemode")
#define SETTINGS_TRACK_FORCED   QStringLiteral("trackmanager/forcedroles")
#define SETTINGS_TRACK_ROLEOFF  QStringLiteral("trackmanager/roleoff")

class TrackManager : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY(TrackManager)

    Q_PROPERTY(int listenPort READ listenPort WRITE setListenPort NOTIFY listenPortChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(bool bltRunning READ bltRunning NOTIFY bltRunningChanged)   // R465_BLT_LIVE_PROP
    Q_PROPERTY(bool bltKnown READ bltKnown NOTIFY bltRunningChanged)       // R468_BLT_LOOK_PROP
    Q_PROPERTY(int bltStartState READ bltStartState NOTIFY bltStartChanged)   // R468_START_BLT_PROP: 0, 1 starting, 2 it did not
    // R474_UPDATE_PROP: the release's newest rig build, the one update-qlc.cmd installed last,
    // update-qlc.cmd where it can be run - and UPDATE's step: 0, 1 started, 2 no script, 3 SHOW OFF first
    Q_PROPERTY(QString updateLatest READ updateLatest NOTIFY updateChanged)
    Q_PROPERTY(QString updateInstalled READ updateInstalled NOTIFY updateChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateChanged)
    Q_PROPERTY(QString updateScript READ updateScript NOTIFY updateChanged)
    Q_PROPERTY(int updateStep READ updateStep NOTIFY updateChanged)
    /** No position from BLT for a while although a track was playing:
     *  the engine holds its last look instead of guessing. */
    Q_PROPERTY(bool linkStale READ linkStale NOTIFY linkChanged)

    Q_PROPERTY(QString title READ title NOTIFY trackChanged)
    Q_PROPERTY(int beatCount READ beatCount NOTIFY trackChanged)
    Q_PROPERTY(int downbeat READ downbeat NOTIFY trackChanged)   // DOWNBEAT_R104
    Q_PROPERTY(QVariantList waveform READ waveform NOTIFY trackChanged)
    /** What the analysis saw, per beat 0..255: bass, highs, kick. */
    Q_PROPERTY(QVariantList lowCurve READ lowCurve NOTIFY trackChanged)
    Q_PROPERTY(QVariantList highCurve READ highCurve NOTIFY trackChanged)
    Q_PROPERTY(QVariantList kickCurve READ kickCurve NOTIFY trackChanged)
    Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)
    Q_PROPERTY(bool canUndoMarkers READ canUndoMarkers NOTIFY markersChanged)
    Q_PROPERTY(bool mixing READ mixing NOTIFY mixChanged)
    Q_PROPERTY(QString nextTitle READ nextTitle NOTIFY mixChanged)
    Q_PROPERTY(int nextFirstDrop READ nextFirstDrop NOTIFY mixChanged)
    Q_PROPERTY(QVariantList cacheList READ cacheList NOTIFY cacheChanged)

    Q_PROPERTY(int currentBeat READ currentBeat NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY positionChanged)
    Q_PROPERTY(int positionMs READ positionMs NOTIFY positionChanged)
    Q_PROPERTY(int durationMs READ durationMs NOTIFY trackChanged)

    Q_PROPERTY(QString currentState READ currentState NOTIFY stateChanged)
    Q_PROPERTY(QString overrideState READ overrideState WRITE setOverrideState NOTIFY stateChanged)
    Q_PROPERTY(QString runningLook READ runningLook NOTIFY stateChanged)

    Q_PROPERTY(bool autoRun READ autoRun WRITE setAutoRun NOTIFY autoRunChanged)
    Q_PROPERTY(int quantize READ quantize WRITE setQuantize NOTIFY quantizeChanged)
    Q_PROPERTY(int slotCount READ slotCount CONSTANT)
    Q_PROPERTY(bool roleMode READ roleMode WRITE setRoleMode NOTIFY looksChanged)
    Q_PROPERTY(int roleCount READ roleCount CONSTANT)
    Q_PROPERTY(QVariantList roleActivity READ roleActivity NOTIFY engineChanged)

    Q_PROPERTY(int liveBpm READ liveBpm NOTIFY energyChanged)
    Q_PROPERTY(double engineTempo READ engineBpm NOTIFY tempoChanged)      // R308_TEMPO_SOURCE R313_TEMPO_NOTIFY
    Q_PROPERTY(QString tempoSource READ tempoSource NOTIFY tempoChanged)
    Q_PROPERTY(qreal energy READ energy NOTIFY energyChanged)
    Q_PROPERTY(qreal appliedEnergy READ appliedEnergy NOTIFY energyChanged)
    Q_PROPERTY(int energyTrim READ energyTrim WRITE setEnergyTrim NOTIFY energyChanged)
    Q_PROPERTY(int bpmLow READ bpmLow WRITE setBpmLow NOTIFY energyChanged)
    Q_PROPERTY(int bpmHigh READ bpmHigh WRITE setBpmHigh NOTIFY energyChanged)
    Q_PROPERTY(bool testTrack READ testTrack NOTIFY testTrackChanged)   // R450_TT_PROP
    Q_PROPERTY(qreal testBpm READ testBpm NOTIFY testTrackChanged)

public:
    TrackManager(QQuickView *view, Doc *doc, QObject *parent = nullptr);
    ~TrackManager();

    static QStringList stateNames();

    int listenPort() const;
    void setListenPort(int port);
    bool connected() const;
    bool bltRunning() const;             // R465_BLT_LIVE_GET
    bool bltKnown() const;               // R468_BLT_LOOK_GET
    int bltStartState() const;           // R468_START_BLT_GET
    /** START BLT on the empty track: Beat Link Trigger from the taskbar's own shortcut */
    Q_INVOKABLE void startBlt();
    QString updateLatest() const;        // R474_UPDATE_GET
    QString updateInstalled() const;
    bool updateAvailable() const;
    QString updateScript() const;
    int updateStep() const;
    Q_INVOKABLE void startUpdate();
    bool linkStale() const;

    QString title() const;
    int beatCount() const;
    int downbeat() const { return m_downbeat; }      // DOWNBEAT_R104_GET
    QVariantList waveform() const;
    QVariantList lowCurve() const;
    QVariantList highCurve() const;
    QVariantList kickCurve() const;
    QVariantList markers() const;
    int currentBeat() const;
    bool playing() const;
    int positionMs() const;
    int durationMs() const;

    QString currentState() const;
    QString overrideState() const;
    void setOverrideState(QString state);
    QString runningLook() const;

    bool autoRun() const;
    void setAutoRun(bool enable);
    int quantize() const;
    void setQuantize(int beats);
    int slotCount() const;

    int liveBpm() const;
    qreal energy() const;
    qreal appliedEnergy() const;
    int energyTrim() const;
    void setEnergyTrim(int percent);
    int bpmLow() const;
    void setBpmLow(int bpm);
    int bpmHigh() const;
    void setBpmHigh(int bpm);

    Q_INVOKABLE void moveMarker(int index, int beat);
    /** Flags by hand: a flag on the bar nearest beat, a flag removed, a
     *  flag's type changed. Each goes to BLT's cache as manual and teaches
     *  the second pass what a drop and a break sound like here. */
    Q_INVOKABLE void addMarker(int beat, QString type);
    Q_INVOKABLE void removeMarker(int index);
    Q_INVOKABLE void setMarkerType(int index, QString type);
    Q_INVOKABLE bool markersManual() const;
    /** One step back: flags and the lesson they taught. */
    Q_INVOKABLE void undoMarkers();
    bool canUndoMarkers() const;
    bool mixing() const;
    QString nextTitle() const;
    QVariantList nextMarkers() const;
    int nextFirstDrop() const;
    /** BLT's analysis cache, on request: { title, flags, manual, version }. */
    QVariantList cacheList() const;
    Q_INVOKABLE void requestCache();
    Q_INVOKABLE void forgetTrack(QString title);
    Q_INVOKABLE void forgetAutomatic();
    Q_INVOKABLE QString stateAtBeat(int beat) const;

    /* ---- slots ---- */
    Q_INVOKABLE QString slotName(int slot) const;
    Q_INVOKABLE QString slotFolder(int slot) const;
    Q_INVOKABLE void setSlotFolder(int slot, QString folder);
    /** Does this slot follow the section's beat division? */
    Q_INVOKABLE bool slotFollowsSpeed(int slot) const;
    Q_INVOKABLE void setSlotFollowsSpeed(int slot, bool follow);

    Q_INVOKABLE QVariantList folderList() const;
    Q_INVOKABLE QVariantList slotFunctions(int slot) const;

    /* ---- looks ---- */
    Q_INVOKABLE void setLookFunction(QString state, int slot, quint32 fid);
    Q_INVOKABLE quint32 lookFunction(QString state, int slot) const;
    Q_INVOKABLE void setLookRandom(QString state, int slot, bool random);
    Q_INVOKABLE bool lookRandom(QString state, int slot) const;

    Q_INVOKABLE void setStateIntensity(QString state, int percent);
    Q_INVOKABLE int stateIntensity(QString state) const;

    /** Beat division for a section, in milliBEATs. 1000 = one beat per step,
     *  500 = half a beat, 250 = a quarter. 0 leaves the Function's own tempo
     *  alone. */
    Q_INVOKABLE void setStateDivision(QString state, int milliBeats);
    Q_INVOKABLE int stateDivision(QString state) const;

    Q_INVOKABLE void reroll();
    Q_INVOKABLE void clear();
    /** R450_TT_API: TEST TRACK - a made-up track when the CDJs have none;
     *  learns nothing, loops, a real track takes over */
    bool testTrack() const;
    qreal testBpm() const;
    Q_INVOKABLE void startTestTrack();
    Q_INVOKABLE void stopTestTrack();
    /** R459_RM_API: the phone remote - one line "<command>|<args>" from the
     *  page; the answer is the page's state ("TRACKSTATE|{json}") */
    Q_INVOKABLE QString remoteCommand(const QString &line);
    QString remoteState();

    /* ---- roles ---- */
    bool roleMode() const;
    void setRoleMode(bool enable);
    Q_INVOKABLE int roleCount() const;
    Q_INVOKABLE QString roleName(int role) const;
    Q_INVOKABLE QString roleHint(int role) const;

    /** Every assignable function with the role it currently sits in.
     *  Entries: { id, name, path, role }. role is -1 when unassigned. */
    Q_INVOKABLE QVariantList roleTable() const;
    Q_INVOKABLE QVariantList roleFunctions(int role) const;
    Q_INVOKABLE int roleOf(quint32 fid) const;
    Q_INVOKABLE void assignRole(quint32 fid, int role);

    /** Guess a role for every function from its channels and its name.
     *  force=false only fills in what has not been assigned yet. */
    Q_INVOKABLE void autoAssignRoles(bool force);

    /* ---- advanced: pin a role in one section ---- */
    Q_INVOKABLE void setForcedRole(QString state, int role, quint32 fid);
    Q_INVOKABLE quint32 forcedRole(QString state, int role) const;
    Q_INVOKABLE void setRoleEnabled(QString state, int role, bool enable);
    Q_INVOKABLE bool roleEnabled(QString state, int role) const;

    Q_INVOKABLE QString roleReport() const;

    /** One bool per role: is it driving output right now? */
    QVariantList roleActivity() const;
    /** Energy of the section containing beat, 0..1, or -1 if unknown. */
    Q_INVOKABLE qreal sectionEnergy(int beat) const;

signals:
    void testTrackChanged();             // R450_TT_SIG
    void listenPortChanged();
    void connectedChanged();
    void bltRunningChanged();            // R465_BLT_LIVE_SIG
    void bltStartChanged();              // R468_START_BLT_SIG
    void updateChanged();                // R474_UPDATE_SIG
    void linkChanged();
    void tempoChanged();        // R313_TEMPO_SIGNAL: engineTempo / tempoSource
    void trackChanged();
    void markersChanged();
    void mixChanged();
    void cacheChanged();
    void positionChanged();
    void stateChanged();
    void autoRunChanged();
    void quantizeChanged();
    void energyChanged();
    void looksChanged();
    void engineChanged();

protected slots:
    void slotNewConnection();
    void slotReadyRead();
    void slotDisconnected();
    void slotEnergyTick();

protected:
    void restartServer();
    void handleLine(const QByteArray &line);
    void handleTrack(const QJsonObject &obj);
    void handlePosition(const QJsonObject &obj);

    void updateState();
    void applyLook();
    void stopLook();
    void applyEnergy();
    quint32 resolveSlot(QString state, int slot) const;

    /* The busking engine. Called on every beat; decides what each role
     * should be doing right now and rides the intensities. */
    void runEngine(bool sectionChanged);
    void sectionBounds(int beat, int &start, int &end) const;
    void nextSection(int beat, QString &state, int &beatsToNext) const;
    void sendMarkers(bool manual = true);
    /** The second pass over a fresh analysis. True when it changed anything. */
    bool refineMarkers();
    qreal kickMean(int fromBeat, int count, bool needHalf = true) const;
    /** min(waveform, low) / 255 averaged over [fromBeat, toBeat), the way
     *  BLT measures a section's energy; -1 without curves. */
    qreal curveEnergy(int fromBeat, int toBeat) const;
    /** Give every flag whose energy is -1 (or every flag, when all is set)
     *  the energy of its section, measured on the curves. */
    void fillMarkerEnergies(bool all);
    void markersEdited();
    void pushUndo();
    void sendEvent(const QJsonObject &obj);
    void handleExtra(const QString &evt, const QJsonObject &obj);
    void learnFromFlag(const QString &type, int beat, bool added);
    void testStep(quint32 serial);       // R450_TT_STEP: one beat of the test track
    void remoteWatch();                  // R459_RM_PRIV: a quiet phone lets go of what it held
    void remoteHeld(const QString &what, bool on, const QStringList &args);
    quint32 pickRole(QString state, int role, int cursor) const;
    void driveRole(int role, quint32 fid, qreal level, int division,
                   bool hard = false);
    void beginFade(int role);
    void tickFades();
    void stopRole(int role);
    void stopAllRoles();
    int classifyFunction(Function *func) const;
    QString roleKey(QString state, int role) const;
    void saveRoles();
    void loadRoles();

    QString mapKey(QString state, int slot) const;
    void loadSettingsMaps();
    void saveLooks();

private:
    QQuickView *m_view;
    Doc *m_doc;
    TrackEngine *m_engine;

    QTcpServer *m_server;
    QList<QTcpSocket *> m_clients;
    QHash<QTcpSocket *, QByteArray> m_buffers;

    int m_port;

    QString m_title;
    qreal m_bpm;
    int m_beatCount;
    int m_downbeat = 0;          // DOWNBEAT_R104_MEMBER: the first bar starts on beat m_downbeat + 1
    int snapBar(int beat) const;
    QVariantList m_waveform;
    QVariantList m_low;
    QVariantList m_high;
    QVariantList m_kick;
    QVariantList m_kickRaw;     // R323_RAW_CURVES: per beat, unsmoothed (BLT 323+)
    QVariantList m_highRaw;
    QVariantList m_markers;
    bool m_markersManual;     // the flags are the operator's, or a cached correction
    qreal m_dropKick;         // learned: the kick a drop needs
    qreal m_breakKick;        // learned: the kick a break stays under
    QList<QVariantMap> m_undo;   // snapshots: flags + thresholds
    int m_lastMoveIndex;         // a drag is one undo step
    qint64 m_lastMoveMs;
    bool m_mixing;
    QString m_nextTitle;
    QString m_key;                       // this track's key from rekordbox, or empty
    QVariantList m_nextMarkers;
    QVariantList m_cacheList;

    int m_currentBeat;
    bool m_playing;
    int m_trackTimeMs;
    int m_durationMs;

    QString m_analysedState;
    QString m_overrideState;

    bool m_autoRun;
    int m_quantize;

    QStringList m_slotNames;
    QStringList m_slotFolders;
    QList<bool> m_slotSpeed;

    QMap<QString, quint32> m_lookFunctions;
    QMap<QString, bool> m_lookRandom;
    // LEVEL and STEP per section were retired in runde 150 - see
    // TrackManager::stateIntensity(). The maps are gone with them.

    QList<quint32> m_runningFunctions;
    QMap<quint32, int> m_runningAttrIds;

    int m_bpmLow;
    int m_bpmHigh;
    int m_energyTrim;
    // R184_START_SCENE_MEMBERS
    QString m_showRanNight;      // R185_SHOWRAN_NIGHT: the night the show ran with ENERGY above 0 (nightKey)
    bool m_startAuto = false;    // the start scene on stage was put up by SHOW ON
    /** SHOW ON with ENERGY above 0: the show has run tonight, and a start
     *  scene SHOW ON put up comes down. */
    void noteShowRunning();
    int m_liveBpm;
    qreal m_posBpm = 0.0;        // R301_POS_BPM: the deck's effective tempo, from BLT's pos
    qint64 m_posBpmMs = 0;
    qreal engineBpm() const;     // what the engine times on (runde 301)
    QString tempoSource() const; // LINK / DECK / TRACK / CLOCK (runde 308)

    QTimer m_energyTimer;
    qint64 m_stopSinceMs = 0;    // STOP_GRACE_R105: when a playing track said stop
    void forgetLinkState();      // LINK_FORGET_FJ3_DECL: NEXT and the mix, as BLT forgets them

    qint64 m_lastPosMs;
    qint64 m_beatChangedMs = 0;  // R187_LAGGING_BEAT: when the beat last moved
    int m_trackPlayer = -1;      // R188_TRACK_PLAYER: the deck the track came from (-1: not sent)
    bool m_linkStale;
    QString m_tempoShown;       // R313_TEMPO_SHOWN: what the tempo line last showed

    /* ---- roles ---- */
    bool m_roleMode;
    QList<QList<quint32> > m_roleFunctions;
    QMap<QString, quint32> m_forcedRole;
    QMap<QString, bool> m_roleOff;

    QMap<int, quint32> m_roleActive;
    QMap<int, int> m_roleAttr;
    QMap<int, qreal> m_roleLevel;

    /* functions on their way out: fid -> attribute id / current level */
    QMap<quint32, int> m_fadeAttr;
    QMap<quint32, qreal> m_fadeLevel;

    int m_colorCursor;
    int m_lastColorBar;
    int m_lastEngineBeat;
    int m_lastSecStart;                   // the section the engine was last told about
    int m_lastSecEnd;
    int m_loopTop = -1;                   // R235_DJ_LOOP: the top of a running DJ loop, -1 none
    int m_jumpFrom = -100;                // the last short step back: from
    int m_jumpTo = -100;                  // ... and to

    /* The movement pick is sticky: it only changes when a break starts,
     * inside a one-beat dark gap. Never at a drop, never mid-section. */
    quint32 m_movePick;

    /* R450_TT_MEM: the test track */
    bool m_testTrack = false;
    qreal m_testBpm = 128.0;
    int m_testBeats = 0;                 // its length
    int m_testBeat = 0;                  // the beat it is on
    qint64 m_testTicks = 0;              // beats since it started (the clock's count)
    quint32 m_testSerial = 0;            // a stop or a new test cancels the beat on its way
    QElapsedTimer m_testClock;

    /* R459_RM_MEM: the phone remote */
    QSet<QString> m_remoteHeld;          // blackout / stutter / flash held by a finger on a phone
    QHash<QString, qint64> m_remoteHeldMs;   // R463_RM_HELD: when its phone last named it
    int m_lastStageSeen = 0;             // R464_ZERO_AFTER
    void probeBlt();                     // R465_BLT_LIVE_MEM
    QTimer m_bltProbeTimer;
    QProcess *m_bltProbe = nullptr;
    qint64 m_bltProbeMs = 0;
    bool m_bltRunning = false;
    bool m_bltKnown = false;             // R468_BLT_LOOK_MEM: a look that ran to the end said so
    void runEngineSynced();              // R470_SYNC_MEM
    void syncEarlyBeat();
    void syncSawBeat(int beat);
    void syncPlanEarly(int beat, int afterMs);
    QTimer m_syncLate, m_syncEarly;
    bool m_syncTimersOn = false;
    int m_syncRealBeat = -1;             // the last beat BLT said
    int m_syncEarlyFor = -1;             // the early timer runs the beat after this one
    int m_syncAhead = -1;                // the beat run early, before BLT said it
    bool m_syncInPos = false;            // R472_SYNC_LOOK: updateState() from a BLT packet
    bool m_syncLateSection = false;      // ... and the section it turned waits with the beat
    qint64 m_bltChainMs = 0;             // R472_BLT_FRESH: when the chain of looks began
    qint64 m_bltLaunchAskedMs = 0;       // ... and when START BLT was tapped
    bool m_bltReprobed = false;
    int m_bltLook = 0;                   // 0 the names, 1 java's windows, 2 every window
    int m_bltStartState = 0;             // R468_START_BLT_MEM
    bool m_bltLaunchPending = false;
    QTimer m_bltStartTimer;
    // R474_UPDATE_MEM (track_runtime/remote.inc.cpp)
    QString m_updateLatest, m_updateInstalled, m_updateScript;
    QStringList m_updateRolledBack;
    int m_updateStep = 0;
    QProcess *m_updateProbe = nullptr;
    QTimer m_updateTimer;
    static QString findUpdateScript();
    void readUpdateInstalled();
    void setUpdateStep(int state);
    void updateCheck();
    void bltProbed(bool running);
    void setBltStart(int state);
    static QString bltShortcut();
    qint64 m_remoteSeenMs = 0;           // the last line from a phone
    QTimer m_remoteTimer;
    bool m_remoteTimerOn = false;
    QJsonArray m_remoteGroups;           // the groups for the phone, every 2 s
    qint64 m_remoteGroupsMs = 0;
};

#endif // TRACKMANAGER_H
