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
#include <QHash>
#include <QList>
#include <QMap>

class QTcpServer;
class QTcpSocket;
class QQuickView;
class TrackEngine;
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
    /** No position from BLT for a while although a track was playing:
     *  the engine holds its last look instead of guessing. */
    Q_PROPERTY(bool linkStale READ linkStale NOTIFY linkChanged)

    Q_PROPERTY(QString title READ title NOTIFY trackChanged)
    Q_PROPERTY(int beatCount READ beatCount NOTIFY trackChanged)
    Q_PROPERTY(QVariantList waveform READ waveform NOTIFY trackChanged)
    Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)

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
    Q_PROPERTY(qreal energy READ energy NOTIFY energyChanged)
    Q_PROPERTY(qreal appliedEnergy READ appliedEnergy NOTIFY energyChanged)
    Q_PROPERTY(int energyTrim READ energyTrim WRITE setEnergyTrim NOTIFY energyChanged)
    Q_PROPERTY(int bpmLow READ bpmLow WRITE setBpmLow NOTIFY energyChanged)
    Q_PROPERTY(int bpmHigh READ bpmHigh WRITE setBpmHigh NOTIFY energyChanged)

public:
    TrackManager(QQuickView *view, Doc *doc, QObject *parent = nullptr);
    ~TrackManager();

    static QStringList stateNames();

    int listenPort() const;
    void setListenPort(int port);
    bool connected() const;
    bool linkStale() const;

    QString title() const;
    int beatCount() const;
    QVariantList waveform() const;
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
    void listenPortChanged();
    void connectedChanged();
    void linkChanged();
    void trackChanged();
    void markersChanged();
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
    void sendMarkers();
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
    QVariantList m_waveform;
    QVariantList m_markers;

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
    QMap<QString, int> m_stateIntensity;
    QMap<QString, int> m_stateDivision;

    QList<quint32> m_runningFunctions;
    QMap<quint32, int> m_runningAttrIds;

    int m_bpmLow;
    int m_bpmHigh;
    int m_energyTrim;
    int m_liveBpm;

    QTimer m_energyTimer;

    qint64 m_lastPosMs;
    bool m_linkStale;

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

    /* The movement pick is sticky: it only changes when a break starts,
     * inside a one-beat dark gap. Never at a drop, never mid-section. */
    quint32 m_movePick;
};

#endif // TRACKMANAGER_H
