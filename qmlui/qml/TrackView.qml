/*
  Q Light Controller Plus
  TrackView.qml

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

// TRACK_LAYOUT_V2 - the layout branch (proposal 8, Tobias 10-07: "byg det og
// tilfoej ogsaa multitouch"). The generator keeps writing this file into a tree
// whose TrackView.qml carries this marker; the classic page stays on the main
// branch. (TRACK_TOUCH_LAYOUT_R137: the generator's legacy normaliser keys on
// this marker, so the old migration patches run on the classic text, not here.)

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

import org.qlcplus.classes 1.0
import "."

Rectangle
{
    id: trackViewRoot
    // TRACK_APP_UI_R548 (track-app branch, Tobias 11-04: "fjern de knapper vi alligevel ikke kan bruge
    // mere", mockup approved): the TRACK app reads the CDJs itself - no Beat Link Trigger on its page,
    // and its update is its own (trackAppCtl, qmlui/trackapp.cpp). Without --track-app: as before
    readonly property bool trackApp: typeof trackAppCtl !== "undefined" && !!trackAppCtl && trackAppCtl.active === true
    // TRACK_APP_UI_R560: the palette's colours no group on stage can show (their tiles toned down) and the
    // clock's ENERGY now / in 15 min - read once a second, both only read
    property var darkColours: []
    property int clockNow: -1
    property int clockSoon: -1
    Timer
    {
        interval: 1000; repeat: true; triggeredOnStart: true
        running: trackViewRoot.trackApp && !!trackEngine && trackEngine.coloursDark !== undefined
        onTriggered:
        {
            trackViewRoot.darkColours = trackEngine.coloursDark()
            trackViewRoot.clockNow = trackEngine.clockTargetAt(0)
            trackViewRoot.clockSoon = trackEngine.clockTargetAt(15)
        }
    }
    readonly property int cdjCount: trackApp ? trackAppCtl.cdjCount : 0
    // TRACK_APP_NOCDJS_R577: "no CDJs" as before (the R575 bltFeeds exception is gone)
    // TRACK_APP_BARS_R552: the POSITION row's page - "heads" (as before) or "bars"
    property string posPage: "heads"
    // TRACK_APP_UI_R561: the animation lasers in his hand (the LASER page) - ON, a side or a size
    readonly property bool aniHand: trackApp && !!trackEngine && trackEngine.aniHand === true
    property bool aniListOpen: false
    readonly property bool barsHand: trackApp && !!trackEngine && (trackEngine.barsOn || trackEngine.barsAim !== "")
    // TRACK_APP_SETUP_R550 (Tobias 11-04: "FULL AUTO (skal altid vaere paa, saa unoedvendig)"): kept on
    Timer
    {
        interval: 2000; repeat: true; triggeredOnStart: true
        running: trackViewRoot.trackApp && !!trackEngine
        onTriggered: if (trackEngine && trackEngine.fullAuto === false) trackEngine.fullAuto = true
    }
    // SAVED_LOOKS_UI: approved 21/10, including touch scroll and outside-to-close.
    readonly property bool savedUiAvailable: true
    readonly property bool savedUndoAvailable: !!trackEngine && trackEngine.canUndoLook
    property bool savedSaveFeedback: false
    property bool savedFavoritesOpen: false
    property bool savedRecentOpen: false
    // TRACK_APP_LIVEFREE_R576: the LIVE card's left edge (the popups' closers stop there) and its tiles' presses
    function liveLeftNow() { return liveCard ? Math.max(0, liveCard.mapToItem(trackViewRoot, 0, 0).x - trackViewRoot.g / 2) : width }
    function liveTouched() { aniListOpen = false; savedRecentOpen = false; bltGuideOpen = false }
    function hookLive(item)
    {
        if (!item || !item.children)
            return
        for (var i = 0; i < item.children.length; i++)
        {
            var c = item.children[i]
            if (c.holdEnabled !== undefined && c.pressedAt !== undefined)
                c.pressedAt.connect(trackViewRoot.liveTouched)
            else
                hookLive(c)
        }
    }
    // TRACK_APP_FIX_R575 (review): an open LASER list / RECENT SAVED ate the next press on BLACKOUT or FLASH and
    // stayed open all night - they close by themselves after 12 s, the list when its page goes
    Timer { interval: 12000; running: trackViewRoot.aniListOpen; onTriggered: trackViewRoot.aniListOpen = false }
    Timer { interval: 12000; running: trackViewRoot.savedRecentOpen; onTriggered: trackViewRoot.savedRecentOpen = false }
    onPosPageChanged: aniListOpen = false
    readonly property var savedRows: trackEngine ? trackEngine.savedLooks : []
    readonly property var savedRecentRows: trackEngine ? trackEngine.recentSavedLooks : []
    readonly property bool savedRecentEmpty: savedRecentRows.length === 0
    onSavedFavoritesOpenChanged: if (savedFavoritesOpen) savedRecentOpen = false
    onSetupOpenChanged: { savedRecentOpen = false; if (!setupOpen) savedFavoritesOpen = false }
    onHelpOpenChanged: if (helpOpen) savedRecentOpen = false
    anchors.fill: parent
    color: "#0A0A0D"                    // R403_TONES: page, card, tile - parted by tone alone

    // ---------------------------------------------------------------------
    // ONE GRID (proposal 8). Drawn for the lys-PC - 3072x1920 at 150 % leaves
    // the page 2048 x 1160 - and scaled from there: ks for heights and type,
    // kw for the column widths. Every control in the deck is one height (R),
    // never under the touch height, with one gap (g) between them.
    // ---------------------------------------------------------------------
    readonly property real ks: Math.max(0.6, Math.min(width / 2048, height / 1160))
    readonly property real kw: Math.max(0.6, width / 2048)
    property real touchH: Math.max(UISettings.iconSizeMedium * 1.4, 50)
    readonly property bool compactLayout: height < 950
    readonly property real rowH: Math.max(touchH, Math.round(62 * ks))
    readonly property real g: Math.max(6, Math.round(12 * ks))
    readonly property real mx: Math.round(20 * kw)            // page side margin
    readonly property real gapB: Math.max(8, Math.round(12 * ks))   // between the page's blocks
    readonly property real cardPad: Math.max(10, Math.round(18 * ks))
    readonly property real headH: Math.max(20, Math.round(28 * ks))
    readonly property real topH: Math.max(50, Math.round(92 * ks)) + (trackApp ? appTabsH : 0)   // R481_CLOCK_BAR: was 64 - the big clock
    // TRACK_APP_TOP_R549 (Tobias 11-04: "saet track/vs knapperne i midten lige over uret saa vi kan fjerne
    // hele toppen"): no kiosk strip on the TRACK app's Track page - TRACK / VIRTUAL CONSOLE over the clock
    readonly property real appTabsH: Math.round(30 * ks)
    // R481_CLOCK (Tobias 10-19, mockup B): the time, big, in the middle of the top bar
    property string clockHM: Qt.formatTime(new Date(), "hh:mm")
    property string clockS: Qt.formatTime(new Date(), "ss")
    Timer
    {
        interval: 1000; running: true; repeat: true
        onTriggered: { var d = new Date(); trackViewRoot.clockHM = Qt.formatTime(d, "hh:mm"); trackViewRoot.clockS = Qt.formatTime(d, "ss") }
    }
    readonly property real footH: Math.max(28, Math.round(36 * ks))
    readonly property real railH: Math.max(56, Math.round(68 * ks))
    readonly property real deckH: cardPad * 2 + headH + 6 * rowH + 5 * g
    readonly property real rigW: Math.round(540 * kw)
    readonly property real liveW: Math.round(340 * kw)
    function fs(px) { return Math.max(10, Math.round(px * ks)) }

    // colours: a quiet page, colour only where something is ON
    readonly property color cCard:    "#141418"
    readonly property color cEdge:    "#24242A"
    readonly property color cBtn:     "#202027"
    readonly property color cBtnEdge: "#2E2E36"
    readonly property color cText:    "#E9E9EE"
    readonly property color cMute:    "#8C8C96"
    readonly property color cDim:     "#5E5E68"
    readonly property color cGreen:   "#6ECD82"
    readonly property color cGold:    "#E3B44F"
    readonly property color cBlue:    "#4FA3E3"
    // kept for TrackSetup and older bindings
    readonly property color cPanel:  "#141418"
    readonly property color cBtnHi:  "#2A2A30"
    readonly property color cLine:   "#2E2E36"

    property int beatCount: trackManager ? trackManager.beatCount : 0
    readonly property bool testOn: !!trackManager && trackManager.testTrack === true   // R450_TEST_TRACK
    property int currentBeat: trackManager ? trackManager.currentBeat : 0
    property string liveState: trackManager ? trackManager.currentState : "normal"
    property var states: [ "normal", "break", "build", "drop" ]

    property var divValues: [ 0, 4000, 2000, 1000, 500, 250, 125 ]
    property var divLabels: [ "-", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8" ]

    property bool setupOpen: false
    property bool helpOpen: false
    // R436_SHOW_CHECK: the show file lacks what this build counts on
    readonly property bool oldShow: trackEngine && trackEngine.showMissing ? trackEngine.showMissing.length > 0 : false
    property bool oldShowOpen: false     // the chip asked for the dialog again
    property bool oldShowMore: false
    // R402_MOTION (runde 402): the page's motion - presses, fades, the beat, the
    // faders' glide. The Qt check turns it off to read every end state at once
    property bool animate: true
    // the beat in the bar, 0..3, from the track's own downbeat (as barOf)
    readonly property int beatInBar: currentBeat > 0
        ? (((currentBeat - 1 - (trackManager ? (trackManager.downbeat || 0) : 0)) % 4) + 4) % 4 : -1

    // ALL AUTO also restores HAZE/FAN AUTO when the show has a hazer (Tobias 20-10).
    readonly property bool allAutoOn: trackEngine && trackManager
        && trackEngine.roomAuto
        && (!trackEngine.hazeAvailable || trackEngine.hazeAuto)
        && trackManager.overrideState === ""
        && trackEngine.positionMode === ""
        && !trackViewRoot.barsHand                 // TRACK_APP_ALLAUTO_R556: the laser bars by hand
        && !trackViewRoot.aniHand                  // TRACK_APP_FIX_R570: and the animation lasers
        && trackEngine.colourMode === 0 && (trackEngine.colourOverride === "" || trackEngine.colourOverrides.length > 1)
    function allAuto()
    {
        if (!trackEngine || !trackManager) return
        trackEngine.roomAuto = true
        if (trackEngine.hazeAvailable) trackEngine.hazeAuto = true
        trackManager.overrideState = ""
        trackEngine.positionMode = ""
        trackEngine.colourOverride = ""        // AUTO colour - and FADE / CHASE off with it
        if (trackViewRoot.trackApp && trackViewRoot.barsHand) trackEngine.barsAuto()   // TRACK_APP_ALLAUTO_R556
        if (trackViewRoot.trackApp && trackViewRoot.aniHand) trackEngine.aniAuto()     // TRACK_APP_FIX_R570
    }             // runde 393: the guide over the page
    // the pencil: the flag tools stay out until it is tapped again (Tobias 10-07)
    property bool markerEdit: false
    property string r594Title: ""        // TRACK_APP_FIX_R594: the track the MARKERS pencil was opened on

    property int dragIndex: -1
    // runde 396: the bar a dragged flag would land on - drawn while the finger moves,
    // moved ONCE on release (one undo step, one note to BLT, no section flipping live
    // while the flag passes the playhead); a cancelled drag moves nothing
    property int previewBeat: -1
    function markerBeat(i)
    {
        if (i === dragIndex && previewBeat > 0) return previewBeat
        var mk = trackManager ? trackManager.markers : []
        return (i >= 0 && i < mk.length) ? mk[i].beat : 0
    }
    function barOf(beat)
    {
        var db = trackManager ? (trackManager.downbeat || 0) : 0
        return Math.max(1, Math.floor((beat - 1 - db) / 4) + 1)
    }
    property real dragX: 0
    property real dragOffset: 0
    property bool zoomActive: false
    property int zoomCenter: 1
    property int zoomSpan: 64

    // R502_M_STATE (Tobias 10-25, mockup approved: "Hvis man bare trykker skal den bare saette hvor
    // man 'er', men hvis man hiver skal den goere som du foreslaar"): a + TYPE chip DRAGGED onto
    // the track lands on the bar under the finger - a dashed line in the type's colour, snapped
    // as a moved tab snaps (snapBeat), with the bar and its distance to the playhead in a bubble.
    // Let go over the track: addMarker() once; anywhere else: nothing. A TAP is as before.
    property string flagDragType: ""        // the chip a finger drags ("": none) - one at a time
    property real flagDragX: 0              // the finger, on the page
    property real flagDragY: 0
    property int flagDragBeat: -1           // the bar it would land on; -1: not over the track
    property string flagSetText: ""         // "DROP SET · BAR 129" - 1.5 s after it landed
    property int flagSetBeat: -1
    function flagDragMove(type, item, x, y)
    {
        if (flagDragType !== type) return
        var p = item.mapToItem(trackViewRoot, x, y)
        flagDragX = p.x
        flagDragY = p.y
        var q = item.mapToItem(wfCanvas, x, y)
        if (beatCount <= 0 || q.x < 0 || q.y < 0 || q.x > wfCanvas.width || q.y > wfCanvas.height)
            flagDragBeat = -1
        else
            flagDragBeat = Math.max(1, Math.min(snapBeat(wfArea.beatAt(q.x)), beatCount))
        // R503_UI_LAST_BAR: snapping rounds up, so the last beats clamped to beatCount - mid-bar, beside
        // a flag already on that bar's line (addMarker's same-bar check missed it: two flags on one bar).
        // Never past the last bar line: the line and the flag stand where the bubble's BAR says.
        var lastBar = snapBeat(beatCount) > beatCount ? snapBeat(beatCount) - 4 : snapBeat(beatCount)
        if (flagDragBeat > lastBar && lastBar >= 1) flagDragBeat = lastBar
    }
    function flagDragEnd(type, commit)
    {
        if (flagDragType !== type) return
        var b = flagDragBeat
        flagDragType = ""
        flagDragBeat = -1
        if (commit !== true || b <= 0 || !trackManager) return
        trackManager.addMarker(b, type)
        flagSetBeat = b
        flagSetText = type.toUpperCase() + " " + qsTr("SET") + " · " + qsTr("BAR") + " " + barOf(b)
        flagSetOff.restart()
    }
    function barsWord(n) { return n + " " + (n === 1 ? qsTr("BAR") : qsTr("BARS")) }
    readonly property string flagDragText:
    {
        if (flagDragType === "" || flagDragBeat <= 0) return ""
        var s = flagDragType.toUpperCase() + " · " + qsTr("BAR") + " " + barOf(flagDragBeat)
        if (currentBeat <= 0) return s
        var d = barOf(flagDragBeat) - barOf(currentBeat)
        return s + " · " + (d > 0 ? qsTr("IN") + " " + barsWord(d)
                                : (d < 0 ? barsWord(-d) + " " + qsTr("AGO") : qsTr("THIS BAR")))
    }
    Timer { id: flagSetOff; interval: 1500; onTriggered: trackViewRoot.flagSetText = "" }

    // R443_PLAN_LANE (N5 A): the plan of every section on the track - PLAN beside
    // MARKERS turns it on (OFF until he does; the engine remembers it)
    readonly property bool planShown: !!trackEngine && trackEngine.planLane === true && !markerEdit && beatCount > 0
    readonly property real laneH: Math.max(30, Math.round(38 * ks))
    property var planModel: []
    property bool planCardOpen: false
    property int planCardBeat: -1                    // R484_PL_PAGE: the section the card is for (its flag)
    property bool bltGuideOpen: false                // R452_EMPTY: the (i) guide over the empty track
    property var planCardRows: []
    function planRefresh()
    {
        if (!planShown || !trackEngine || !trackManager) { planModel = []; planCardOpen = false; return }
        var mk = wfCanvas.sortedMarkers()
        var secs = []
        for (var i = 0; i < mk.length; i++)
            secs.push({ beat: mk[i].beat, end: i + 1 < mk.length ? mk[i + 1].beat : beatCount + 1, type: mk[i].type })
        planModel = trackEngine.planLaneOf(secs, trackManager.currentBeat)
        var nx = planCardSec()          // R484_PL_REFRESH: the card's own section, next or later
        if (planCardOpen && (nx === null || !planEditable(nx)))
            planCardOpen = false
        if (planCardOpen)
            planCardRows = planRowsOf(nx)
    }
    // R445_DROP_HIT: the next DROP flag ahead of the track (-1: none)
    function nextDropBeat()
    {
        var mk = trackManager ? trackManager.markers : []
        var cur = trackManager ? trackManager.currentBeat : 0
        var best = -1
        for (var i = 0; i < mk.length; i++)
            if (mk[i].type === "drop" && mk[i].beat > cur && (best < 0 || mk[i].beat < best)) best = mk[i].beat
        return best
    }
    function hitName(kind) { return kind === 1 ? qsTr("FLASH") : (kind === 2 ? qsTr("STROBE") : (kind === 3 ? qsTr("DARK \u2192 BANG") : "")) }
    function planNext()
    {
        for (var j = 0; j < planModel.length; j++)
            if (planModel[j].next) return planModel[j]
        return null
    }
    // the next section is the RIG's plan; R484_PL_EDITABLE (Tobias 10-19: "goer det muligt at man
    // kan aendre planen gennem hele tracket fremad"): a later one ("ahead") is his to pick too -
    // the engine plays it when it comes. A section that has begun is not
    function planEditable(sec) { return sec !== null && (sec.ahead === true || (sec.next && (sec.mode === "next" || sec.mode === "his" || sec.mode === "mem" || sec.mode === "saved"))) }
    function planCardSec()
    {
        for (var j = 0; j < planModel.length; j++)
            if (planModel[j].beat === planCardBeat) return planModel[j]
        return null
    }
    function planRowsOf(sec) { return sec.ahead === true ? trackEngine.planRowsAt(sec.beat, sec.type) : trackEngine.planRows() }
    function planOpenCard(sec)
    {
        if (!trackEngine || !planEditable(sec)) return
        planCardBeat = sec.beat
        planCardRows = planRowsOf(sec)
        planCardOpen = true
    }
    // a tap on a section: its card - on the open one's section, CLOSE
    function planTap(sec) { if (planCardOpen && planCardBeat === sec.beat) planCardOpen = false; else planOpenCard(sec) }
    function planToggle(key)
    {
        var sec = planCardSec()
        if (!trackEngine || sec === null) return
        if (sec.ahead === true) trackEngine.togglePlanAt(sec.beat, sec.type, key)
        else trackEngine.toggleDropGroup(key)
    }
    function planDecides()
    {
        var sec = planCardSec()
        if (!trackEngine || sec === null) return
        if (sec.ahead === true) trackEngine.planEngineDecidesAt(sec.beat, sec.type)
        else trackEngine.planEngineDecides()
    }
    function kindColour(kind)
    {
        if (kind === "bars")   return "#E64646"
        if (kind === "strobe") return "#EBEBF0"
        if (kind === "ani")    return "#3CC85A"
        if (kind === "mini")   return "#C878E6"
        if (kind === "head")   return "#E0921A"
        return "#4F8CE3"
    }
    onPlanShownChanged: { planRefresh(); wfCanvas.requestPaint() }
    Timer { interval: 1000; repeat: true; running: trackViewRoot.planShown; onTriggered: trackViewRoot.planRefresh() }
    Connections
    {
        target: trackEngine
        enabled: trackViewRoot.planShown
        function onDropSetChanged() { trackViewRoot.planRefresh() }
        function onPlanLaneChanged() { trackViewRoot.planRefresh() }
    }

    onMarkerEditChanged: { if (!markerEdit) { wfArea.release(); wfCanvas.selected = -1 } wfCanvas.requestPaint() }

    function markerColor(type)
    {
        if (type === "drop")  return "#E23B3B"
        if (type === "build") return "#E0921A"
        if (type === "break") return "#2F7FD0"
        if (type === "intro") return "#5FB37A"
        if (type === "outro") return "#8C6BB1"
        if (type === "drive") return "#C2566E"   // between a groove's grey and a drop's red
        return "#9AA0A6"
    }

    function swatch(name)
    {
        switch (name)
        {
        case "red":     return "#E03030"
        case "green":   return "#30C050"
        case "blue":    return "#3060E0"
        case "cyan":    return "#30C0D0"
        case "magenta": return "#D040C0"
        case "yellow":  return "#E0D030"
        case "orange":  return "#E08030"
        case "amber":   return "#E0A040"
        case "uv":      return "#7030C0"
        case "white":   return "#E8E8E8"
        }
        return "#4A4A4A"
    }

    function fmtTime(ms)
    {
        if (ms <= 0) return "--:--"
        var t = Math.floor(ms / 1000)
        var m = Math.floor(t / 60)
        var s = t % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }

    // the nearest bar line of THIS track, as TrackManager::snapBar(): the
    // first bar starts on beat downbeat + 1 (BACKLOG 104), and a beat before it
    // goes to beat 1 when that is nearer
    function snapBeat(b)
    {
        var db = trackManager.downbeat || 0
        var x = Math.max(1, b - db)
        var shifted = Math.max(1, Math.floor((x - 1 + 2) / 4) * 4 + 1) + db
        return (db > 0 && b - 1 < shifted - b) ? 1 : shifted
    }

    function viewCount()
    {
        if (beatCount <= 0) return 1
        return zoomActive ? Math.min(zoomSpan, beatCount) : beatCount
    }

    function viewFirst()
    {
        if (!zoomActive || beatCount <= 0) return 1
        var vc = viewCount()
        var f = Math.round(zoomCenter - vc / 2)
        if (f < 1) f = 1
        if (f > beatCount - vc + 1) f = beatCount - vc + 1
        return f
    }

    function nextMarker()
    {
        if (!trackManager) return null
        var mk = trackManager.markers
        var best = null
        for (var i = 0; i < mk.length; i++)
            if (mk[i].beat > currentBeat && (best === null || mk[i].beat < best.beat))
                best = mk[i]
        return best
    }

    Connections
    {
        target: trackManager
        // TRACK_APP_FIX_R594: a NEW track closes the MARKERS pencil. Left open after a flag was fixed (DONE never
        // tapped) it hid the PLAN lane, the armed HIT tag and LAST's outro band on every track that followed -
        // all armed in the engine, none of it on the page. The same track sent again leaves it open
        function onTrackChanged()
        {
            wfArea.release(); wfCanvas.selected = -1
            const t = trackManager ? trackManager.title : ""
            if (t !== trackViewRoot.r594Title)
            {
                if (trackViewRoot.r594Title !== "")
                    trackViewRoot.markerEdit = false
                trackViewRoot.r594Title = t
            }
            wfCanvas.requestPaint()
        }
        function onMarkersChanged()
        {
            // runde 397: the list changed under a drag (the other hand on UNDO,
            // DELETE or + TYPE): the index may point at another flag now - drop
            // the drag, move nothing
            if (trackViewRoot.dragIndex >= 0 || wfArea.pressIndex >= 0)
                wfArea.release(false)
            // only drop the selection when the flag is actually gone: RETYPE
            // must keep the flag it just retyped
            if (wfCanvas.selected >= trackManager.markers.length)
                wfCanvas.selected = -1
            wfCanvas.requestPaint()
        }
        // positionChanged arrives once per beat (runde 144), so this repaints once per beat
        function onPositionChanged() { wfCanvas.requestPaint() }
    }
    Connections
    {
        target: trackEngine
        // the canvas reads one thing from the engine - the colour of the played
        // part. liveChanged fires on every frame of a fader drag (runde 204)
        function onLiveChanged()
        {
            var c = trackEngine ? trackEngine.currentColour : ""
            if (c !== wfCanvas.paintedColour)
            {
                wfCanvas.paintedColour = c
                wfCanvas.requestPaint()
            }
        }
    }

    Timer
    {
        id: panTimer
        interval: 40
        repeat: true
        running: false
        property int dir: 0

        onTriggered:
        {
            if (trackViewRoot.dragIndex < 0) { running = false; return }
            var step = Math.max(1, Math.round(trackViewRoot.viewCount() / 32))
            trackViewRoot.zoomCenter =
                Math.max(1, Math.min(trackViewRoot.beatCount,
                                     trackViewRoot.zoomCenter + dir * step))
            wfArea.preview(wfArea.beatAt(trackViewRoot.dragX) + trackViewRoot.dragOffset)
            wfCanvas.requestPaint()
        }
    }

    // =====================================================================
    //  BUILDING BLOCKS
    // =====================================================================

    // MULTITOUCH, the way QLC+ does it in VCButtonItem.qml: a MouseArea for the
    // mouse and, over it, a MultiPointTouchArea (mouseEnabled: false). A finger
    // is then this control's own touch point - not the one synthesised mouse the
    // whole window shares - so FLASH can be held while the other hand pulls
    // ENERGY, or two faders move at once.
    // Runde 390: NOT maximumTouchPoints: 1 as in VCButtonItem - with one allowed,
    // a second finger (or a palm) on the same control makes Qt drop the first:
    // no release, no cancel, and a held FLASH stayed on after both let go. The
    // area takes every finger but follows only the first (its pointId).
    component TouchInput: Item
    {
        id: ti
        anchors.fill: parent
        property bool down: mouseIn.pressed || touchIn.held
        property bool holdEnabled: false
        property int holdMs: 800
        signal pressedAt(real x, real y)
        signal movedTo(real x, real y)
        signal releasedAt(real x, real y, bool inside)
        signal canceled()
        signal held()

        function inside(x, y) { return x >= 0 && y >= 0 && x <= width && y <= height }

        MouseArea
        {
            id: mouseIn
            anchors.fill: parent
            // a Flickable under this must not steal the press: a stolen grab
            // fires onCanceled and would leave a held FLASH or BLACKOUT on
            preventStealing: true
            onPressed: (m) => { if (touchIn.held) { m.accepted = false; return } ti.pressedAt(m.x, m.y); if (ti.holdEnabled) holdTimer.restart() }
            onPositionChanged: (m) => { if (pressed) ti.movedTo(m.x, m.y) }
            onReleased: (m) => { holdTimer.stop(); ti.releasedAt(m.x, m.y, ti.inside(m.x, m.y)) }
            onCanceled: { holdTimer.stop(); ti.canceled() }
        }
        MultiPointTouchArea
        {
            id: touchIn
            anchors.fill: parent
            mouseEnabled: false
            maximumTouchPoints: 10
            property int pid: -1                // the finger this control follows
            readonly property bool held: pid >= 0
            function mine(pts) { for (var i = 0; i < pts.length; i++) if (pts[i].pointId === pid) return pts[i]; return null }
            onPressed: (pts) =>
            {
                if (pid >= 0 || mouseIn.pressed) return     // already held: a second finger is ignored
                pid = pts[0].pointId
                ti.pressedAt(pts[0].x, pts[0].y)
                if (ti.holdEnabled) holdTimer.restart()
            }
            onUpdated: (pts) => { var p = mine(pts); if (p) ti.movedTo(p.x, p.y) }
            onReleased: (pts) =>
            {
                var p = mine(pts)
                if (!p) return
                pid = -1
                holdTimer.stop()
                ti.releasedAt(p.x, p.y, ti.inside(p.x, p.y))
            }
            onCanceled: (pts) => { if (pid < 0) return; pid = -1; holdTimer.stop(); ti.canceled() }
            // disabled or hidden under the finger (SETUP / HELP): forget it, let go (runde 395)
            function drop() { if (pid < 0) return; pid = -1; holdTimer.stop(); ti.canceled() }
            onEnabledChanged: if (!enabled) drop()
            onVisibleChanged: if (!visible) drop()
        }
        Timer { id: holdTimer; interval: ti.holdMs; onTriggered: ti.held() }
    }

    // Small vector icons: no font symbols that change between Windows and macOS.
    component ControlIcon: Canvas {
        property string kind: ""
        property color ink: "#DDDDDD"
        width: 22; height: 22
        onKindChanged: requestPaint()
        onInkChanged: requestPaint()
        onPaint: {
            var c = getContext("2d"); c.reset(); c.scale(width / 24, height / 24)
            c.strokeStyle = ink; c.fillStyle = ink; c.lineWidth = 1.8
            c.lineCap = "round"; c.lineJoin = "round"
            function line(x,y,x2,y2) { c.beginPath(); c.moveTo(x,y); c.lineTo(x2,y2); c.stroke() }
            function circle(x,y,r) { c.beginPath(); c.arc(x,y,r,0,Math.PI*2); c.stroke() }
            if (kind === "calm") {
                c.beginPath(); c.moveTo(5,19); c.bezierCurveTo(1,9,15,10,19,3); c.bezierCurveTo(22,16,13,21,5,19); c.stroke(); line(4,21,15,11)
            } else if (kind === "hold") { c.fillRect(6,4,4,16); c.fillRect(14,4,4,16) }
            else if (kind === "savedLooks") {
                c.beginPath(); c.moveTo(6,3); c.lineTo(18,3); c.lineTo(18,22)
                c.lineTo(12,17); c.lineTo(6,22); c.closePath(); c.stroke()
            }
            else if (kind === "nextLook") { c.beginPath(); c.moveTo(4,4); c.lineTo(16,12); c.lineTo(4,20); c.closePath(); c.fill(); line(19,4,19,20) }
            else if (kind === "blackout") { circle(12,12,9); line(6,18,18,6) }
            // "give it back to the music": an arrow curving back to where it
            // started (Tobias, 2026-09-22) - SECTION and POSITION
            else if (kind === "revert") {
                c.beginPath(); c.arc(12, 12.5, 7, Math.PI * 0.78, Math.PI * 2.25); c.stroke()
                c.beginPath(); c.moveTo(5.2, 8.4); c.lineTo(5.0, 14.2); c.lineTo(10.6, 12.4)
                c.closePath(); c.fill()
            }
            else if (kind === "flash" || kind === "autoColour") {
                circle(12,12,4)
                for(var i=0;i<8;i++){var a=i*Math.PI/4;line(12+7*Math.cos(a),12+7*Math.sin(a),12+10*Math.cos(a),12+10*Math.sin(a))}
            } else if (kind === "setupSwitch") {
                circle(12,12,6); circle(12,12,2)
                for(var j=0;j<8;j++){var b=j*Math.PI/4;line(12+6*Math.cos(b),12+6*Math.sin(b),12+9*Math.cos(b),12+9*Math.sin(b))}
            } else if (kind === "showSwitch" || kind === "stop") { c.fillRect(5,5,14,14) }
            else if (kind === "deck") { circle(12,12,9.5); circle(12,12,5); c.beginPath(); c.arc(12,12,1.6,0,Math.PI*2); c.fill() }   // R452_EMPTY
            else if (kind === "moon") { c.beginPath(); c.arc(12,12,9,0,Math.PI*2); c.fill(); c.globalCompositeOperation = "destination-out"; c.beginPath(); c.arc(17,8,8,0,Math.PI*2); c.fill(); c.globalCompositeOperation = "source-over" }   // R458_LAST_BTN
            else if (kind === "play") { c.beginPath(); c.moveTo(7,4); c.lineTo(20,12); c.lineTo(7,20); c.closePath(); c.fill() }   // R450_TEST_TRACK
            else if (kind === "laser") { c.strokeRect(3,18,18,4); line(5,15,2,4); line(10,15,8,2); line(15,15,16,2); line(20,15,23,4) }
            else if (kind === "strobe") { c.strokeRect(3,2,18,20); circle(8,7,2); circle(16,7,2); circle(8,17,2); circle(16,17,2) }
            else if (kind === "eyes") { circle(12,6,5); circle(6,17,5); circle(18,17,5); circle(12,6,1.5); circle(6,17,1.5); circle(18,17,1.5) }
            else if (kind === "fadeColour") {
                circle(9,12,6.5); circle(15,12,6.5)
                c.globalAlpha = 0.45; c.beginPath(); c.arc(15,12,6.5,0,Math.PI*2); c.fill(); c.globalAlpha = 1.0
            }
            else if (kind === "chaseColour") {
                c.beginPath(); c.arc(5,9,3,0,Math.PI*2); c.fill(); circle(12,9,3); circle(19,9,3)
                line(3,18,21,18); line(17,15,21,18); line(17,21,21,18)
            }
            else if (kind === "animation") {
                circle(12,12,2)
                for(var k=0;k<4;k++){c.save();c.translate(12,12);c.rotate(k*Math.PI/2);c.beginPath();c.moveTo(0,-3);c.bezierCurveTo(-7,-13,6,-13,3,-3);c.stroke();c.restore()}
            }
            else if (kind === "bolt") { c.beginPath(); c.moveTo(14,2); c.lineTo(5,13.5); c.lineTo(11,13.5); c.lineTo(9,22); c.lineTo(19,9.5); c.lineTo(13,9.5); c.lineTo(15.5,2); c.closePath(); c.fill() }   // R445_DROP_HIT
            else if (kind === "plan") { c.fillRect(3,5,17,3); c.fillRect(3,11,11,3); c.fillRect(3,17,14,3) }   // R443_PLAN_LANE
            else if (kind === "pencil") { c.beginPath(); c.moveTo(4,20); c.lineTo(5,15); c.lineTo(15.5,4.5); c.lineTo(19.5,8.5); c.lineTo(9,19); c.closePath(); c.stroke(); line(13,7,17,11) }
            else if (kind === "check") { c.beginPath(); c.moveTo(4,12.5); c.lineTo(9.5,18); c.lineTo(20,6.5); c.stroke() }
            else if (kind === "lock") { c.strokeRect(5,10.5,14,10); c.beginPath(); c.moveTo(8,10.5); c.lineTo(8,7.5); c.arc(12,7.5,4,Math.PI,0); c.lineTo(16,10.5); c.stroke() }
            else if (kind === "help") { circle(12,12,9.5); c.font = "bold 15px sans-serif"; c.textAlign = "center"; c.textBaseline = "middle"; c.fillText("?", 12, 13) }
            else if (kind === "warn") { c.beginPath(); c.moveTo(12,3); c.lineTo(22,21); c.lineTo(2,21); c.closePath(); c.stroke(); line(12,10,12,15); line(12,18,12,18.5) }
            else { circle(12,8,6); c.strokeRect(3,21,18,2); c.beginPath(); c.moveTo(3,8); c.lineTo(3,18); c.lineTo(21,18); c.lineTo(21,8); c.stroke() }
        }
    }

    // THE ONE BUTTON. active = lit in its tone; tone = the colour of "on".
    component Btn: Rectangle
    {
        id: btn
        property string text: ""
        property string icon: ""
        property bool active: false
        property color tone: trackViewRoot.cGreen
        property bool autoKind: false        // an AUTO: its icon is green even when off
        property bool solid: true             // R402_ONE_ON: every tile that is ON is a full fill in its tone (flat)
        property bool idleTint: false         // its tone shows when off too, as a bar along the foot (sections, colours)
        property bool ring: false             // a white frame: pinned by hand
        property real fontPx: trackViewRoot.fs(14)
        property bool fit: false              // TRACK_APP_FIX_R570: the text ends with "..." inside the tile
        property alias input: btnInput
        property bool touchInputEnabled: true
        signal tapped()
        radius: Math.round(10 * trackViewRoot.ks)
        color: btnInput.down ? Qt.lighter(base, 1.35) : base
        // R402_PRESS: the tile gives under the finger; R402_FADE: colours fade over
        scale: btnInput.down ? 0.965 : 1.0
        Behavior on scale { enabled: trackViewRoot.animate; NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
        Behavior on color { enabled: trackViewRoot.animate; ColorAnimation { duration: btnInput.down ? 60 : 150 } }
        readonly property bool full: solid && active
        // dark ink on a light fill, white on a dark one
        readonly property bool lightFill: 0.299 * tone.r + 0.587 * tone.g + 0.114 * tone.b > 0.5
        property color base: full ? tone
                             : (active ? Qt.tint(trackViewRoot.cBtn, Qt.rgba(tone.r, tone.g, tone.b, 0.24)) : trackViewRoot.cBtn)
        border.width: ring ? 3 : 0       // R403_NO_LINES: the white ring - pinned by hand - is the one line
        border.color: "#FFFFFF"
        opacity: enabled ? 1.0 : 0.3
        // R403_IDLE_BAR: at rest a colour or section tile is dark like the rest,
        // its colour a flat bar along the foot; ON fills the tile
        Rectangle
        {
            objectName: btn.objectName !== "" ? btn.objectName + ":bar" : ""
            visible: btn.idleTint && !btn.full
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: Math.round(14 * trackViewRoot.ks); anchors.rightMargin: anchors.leftMargin
            anchors.bottomMargin: Math.round(7 * trackViewRoot.ks)
            height: Math.max(3, Math.round(5 * trackViewRoot.ks)); radius: height / 2
            color: btn.tone
        }
        Row
        {
            anchors.centerIn: parent
            spacing: Math.round(9 * trackViewRoot.ks)
            ControlIcon
            {
                visible: btn.icon !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: Math.round(18 * trackViewRoot.ks); height: width
                kind: btn.icon
                ink: btn.full ? (btn.lightFill ? "#101010" : "#FFFFFF")
                     : (btn.autoKind ? trackViewRoot.cGreen : label.color)
            }
            Text
            {
                id: label
                anchors.verticalCenter: parent.verticalCenter
                width: btn.fit ? Math.min(implicitWidth, btn.width - Math.round(16 * trackViewRoot.ks)) : implicitWidth
                elide: btn.fit ? Text.ElideRight : Text.ElideNone
                text: btn.text
                color: btn.full ? (btn.lightFill ? "#101010" : "#FFFFFF")
                       : ((btn.active || btn.idleTint) ? Qt.lighter(btn.tone, 1.45) : "#D6D6DC")
                Behavior on color { enabled: trackViewRoot.animate; ColorAnimation { duration: 150 } }
                font.bold: true
                font.pixelSize: btn.fontPx
                font.letterSpacing: btn.fontPx * 0.08
            }
        }
        TouchInput
        {
            id: btnInput
            enabled: btn.touchInputEnabled
            onReleasedAt: (x, y, inside) => { if (inside) btn.tapped() }
        }
    }

    // WHAT MAKES A BAR LOOK LIKE A SLIDER (runde 140): the ridged GRIP, the
    // quarter TICKS and the empty REST above the level - one fader type for
    // ENERGY, MASTER DIMMER, the group trims and HAZE / FAN SPEED.
    component SliderGrip: Item {
        property color ink: "#EEEEEE"
        property bool pressed: false
        width: Math.max(14, Math.round(18 * trackViewRoot.ks))
        Rectangle {
            anchors.fill: parent
            anchors.topMargin: 2
            anchors.bottomMargin: 2
            radius: 5
            color: parent.pressed ? "#FFFFFF" : parent.ink
            border.width: 1
            border.color: "#0E0E0E"
            Column {
                anchors.centerIn: parent
                spacing: 3
                Repeater {
                    model: 3
                    Rectangle { width: 9; height: 2; radius: 1; color: "#1A1A1A"; opacity: 0.75 }
                }
            }
        }
    }

    component SliderTicks: Item {
        property real level: 0
        Repeater {
            model: [ 0.25, 0.5, 0.75 ]
            Item {
                x: 3 + (parent.width - 6) * modelData - 1
                width: 2
                height: parent.height
                Rectangle { y: 0; width: 2; height: 7; color: parent.parent.level > modelData ? Qt.rgba(0,0,0,0.38) : "#34343C" }
                Rectangle { y: parent.height - 7; width: 2; height: 7; color: parent.parent.level > modelData ? Qt.rgba(0,0,0,0.38) : "#34343C" }
            }
        }
    }

    // THE ONE FADER. level 0..1, the value never under the grip: near the top
    // it moves in front of it, onto the fill.
    component Fader: Rectangle
    {
        id: fd
        property real level: 0
        property color fill: trackViewRoot.cBlue
        property color gripInk: "#EEEEEE"
        property string name: ""
        property color nameInk: "#101012"
        property string valueText: Math.round(level * 100) + "%"
        property color valueInk: "#ECECF0"          // on the empty track
        property color valueOnFill: "#0A1622"       // on the fill, near the top
        property alias input: fdInput
        property alias inputName: fdInput.objectName
        signal setLevel(real v)
        signal pressed()
        // R484_HZ_REST_PROP (Tobias 10-20: "haze +70 procent ikke vaere en knap ... faderen bliver 'elastisk' ... ryger tilbage til 70 naar man slipper faderen"):
        // an elastic slider that rests at most here (HAZE: 0.7) - a thin line marks it.
        // -1 = none: every other fader as before
        property real rest: -1
        // R402_GLIDE: what the fader SHOWS glides to its level when something
        // other than the finger moves it (ENERGY's AUTO clock, a VC fader) -
        // under the finger it follows at once. The value is never animated
        property real shown: level
        // set in the press itself: the press's own level comes before 'down' turns true
        property bool dragging: false
        Behavior on shown { enabled: trackViewRoot.animate && !fd.dragging && !fdInput.down; NumberAnimation { duration: 320; easing.type: Easing.OutCubic } }
        radius: Math.round(10 * trackViewRoot.ks)
        color: "#0A0A0D"
        border.width: 0                 // R403_NO_LINES
        clip: true
        function at(x) { return Math.max(0, Math.min(1, (x - 3) / (width - 6))) }

        // R484_HZ_REST_ZONE: where an elastic slider rests - one thin line, under the fill, no text
        // and no box (Tobias 10-20: "ikke vaere en knap")
        Rectangle
        {
            id: fdRest
            objectName: fd.objectName + "Rest"
            visible: fd.rest >= 0 && fd.rest < 1
            x: 3 + (parent.width - 6) * Math.max(0, fd.rest) - 1
            y: 3
            width: 2
            height: parent.height - 6
            color: "#3E3E46"
        }
        Rectangle
        {
            visible: fd.shown > 0
            x: 3; y: 3
            height: parent.height - 6
            width: (parent.width - 6) * fd.shown
            radius: Math.max(3, fd.radius - 3)
            color: fd.fill
        }
        SliderTicks { anchors.fill: parent; level: fd.shown }
        Text
        {
            id: fdName
            visible: fd.name !== ""
            // runde 391 (the lys-PC at 0 %: "\u2261AZE"): the grip never covers the
            // name - on the fill left of the grip when it fits there, else just
            // right of the grip on the empty track
            readonly property real start: Math.round(16 * trackViewRoot.ks)
            readonly property real gap: Math.round(12 * trackViewRoot.ks)
            readonly property bool onFill: fdGrip.x >= start + implicitWidth + gap
            x: onFill ? start : fdGrip.x + fdGrip.width + gap
            anchors.verticalCenter: parent.verticalCenter
            text: fd.name
            color: onFill ? fd.nameInk : "#B8B8C0"
            font.bold: true
            font.pixelSize: trackViewRoot.fs(12)
            font.letterSpacing: (trackViewRoot.fs(12)) * 0.15
        }
        SliderGrip
        {
            id: fdGrip
            x: Math.max(1, Math.min(parent.width - width - 1, 3 + (parent.width - 6) * fd.shown - width / 2))
            height: parent.height
            ink: fd.gripInk
            pressed: fdInput.down
        }
        Text
        {
            id: fdValue
            // at the right end of the empty track - and in front of the grip, on
            // the fill, as soon as the grip would touch it there (runde 391: a
            // fixed 80 % let the grip cover "75%" on the narrow trims)
            readonly property real rightX: parent.width - width - Math.round(18 * trackViewRoot.ks)
            readonly property bool onFill: fdGrip.x + fdGrip.width + Math.round(10 * trackViewRoot.ks) > rightX
            anchors.verticalCenter: parent.verticalCenter
            x: onFill ? fdGrip.x - width - Math.round(12 * trackViewRoot.ks) : rightX
            text: fd.valueText
            color: onFill ? fd.valueOnFill : fd.valueInk
            font.bold: true
            font.pixelSize: trackViewRoot.fs(17)
        }
        TouchInput
        {
            id: fdInput
            onPressedAt: (x, y) => { fd.dragging = true; fd.pressed(); fd.setLevel(fd.at(x)) }
            onMovedTo: (x, y) => fd.setLevel(fd.at(x))
            onReleasedAt: (x, y, inside) => fd.dragging = false
            onCanceled: fd.dragging = false
        }
        // R402_BUBBLE: the value over the finger while it drags - the finger
        // covers the fader's own number. On the page, not in the fader (it clips)
        Rectangle
        {
            id: fdBubble
            objectName: "faderBubble"
            parent: trackViewRoot
            z: 300
            visible: fdInput.down
            property point p: Qt.point(0, 0)
            function place() { p = fd.mapToItem(trackViewRoot, fdGrip.x + fdGrip.width / 2, 0) }
            onVisibleChanged: if (visible) place()
            Connections { target: fdGrip; function onXChanged() { if (fdBubble.visible) fdBubble.place() } }
            width: fdBubbleText.implicitWidth + Math.round(28 * trackViewRoot.ks)
            height: Math.round(44 * trackViewRoot.ks)
            x: p.x - width / 2
            y: p.y - height - Math.round(10 * trackViewRoot.ks)
            radius: Math.round(10 * trackViewRoot.ks)
            color: "#F2F2F5"
            Text
            {
                id: fdBubbleText
                anchors.centerIn: parent
                text: fd.valueText
                color: "#101012"
                font.bold: true
                font.pixelSize: trackViewRoot.fs(20)
            }
        }
    }

    component Card: Rectangle
    {
        property string title: ""
        property real titleBoxW: 0        // TRACK_APP_LABELS_R555: > 0 = the title centred over a column this wide
        color: trackViewRoot.cCard
        radius: Math.round(14 * trackViewRoot.ks)
        border.width: 0                 // R403_NO_LINES
        Text
        {
            x: trackViewRoot.cardPad + 2
               + (parent.titleBoxW > 0 ? Math.round((parent.titleBoxW - implicitWidth + font.letterSpacing) / 2) : 0)
            y: trackViewRoot.cardPad
            text: parent.title
            color: trackViewRoot.cMute
            font.bold: true
            font.pixelSize: trackViewRoot.fs(12)
            font.letterSpacing: (trackViewRoot.fs(12)) * 0.2
        }
    }

    component RowLabel: Text
    {
        color: trackViewRoot.cMute
        // TRACK_APP_LABELS_R555: centred in the label column in the TRACK app (the letter spacing trails the
        // last letter - the same on the left keeps the word in the middle)
        horizontalAlignment: trackViewRoot.trackApp ? Text.AlignHCenter : Text.AlignLeft
        leftPadding: trackViewRoot.trackApp ? font.letterSpacing : 0
        font.bold: true
        font.pixelSize: trackViewRoot.fs(12)
        font.letterSpacing: (trackViewRoot.fs(12)) * 0.15
        lineHeight: 1.15
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.WordWrap
    }

    // =====================================================================
    //  THE PAGE
    // =====================================================================
    ColumnLayout
    {
        anchors.fill: parent
        anchors.leftMargin: trackViewRoot.mx
        anchors.rightMargin: trackViewRoot.mx
        anchors.topMargin: Math.round(14 * trackViewRoot.ks)
        spacing: trackViewRoot.gapB

        // ============ 1 · STATUS: what plays, what comes, the show switch ============
        Item
        {
            id: topBar
            Layout.fillWidth: true
            Layout.preferredHeight: trackViewRoot.topH

            // R481_CLOCK_ITEM: centred - left of LAST TRACK / STOP TEST / OLD SHOW FILE while one
            // shows (they sit where the middle is); the title ends at it
            Row
            {
                id: bigClock
                objectName: "bigClock"
                readonly property real edge: lastBtn.visible ? lastBtn.x : (testStop.visible ? testStop.x
                                             : (oldShowChip.visible ? oldShowChip.x : statusRight.x))
                x: Math.round(Math.min((parent.width - width) / 2, edge - width - 22 * trackViewRoot.ks))
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: trackViewRoot.trackApp ? Math.round(trackViewRoot.appTabsH / 2) : 0
                spacing: Math.round(4 * trackViewRoot.ks)
                Text
                {
                    id: bigClockHM
                    objectName: "bigClockHM"
                    text: trackViewRoot.clockHM
                    color: "#FFFFFF"
                    font.bold: true
                    font.pixelSize: trackViewRoot.fs(70)
                    font.family: "DejaVu Sans Mono"
                }
                Text
                {
                    anchors.baseline: bigClockHM.baseline
                    visible: !trackViewRoot.trackApp   // TRACK_APP_TOP_R553: no seconds - the clock (HH:MM) centres alone
                    text: ":" + trackViewRoot.clockS
                    color: trackViewRoot.cDim
                    font.bold: true
                    font.pixelSize: trackViewRoot.fs(30)
                    font.family: "DejaVu Sans Mono"
                }
            }

            Rectangle
            {
                id: sectionPill
                anchors.verticalCenter: parent.verticalCenter
                width: Math.round(136 * trackViewRoot.ks)
                height: Math.round(trackViewRoot.topH * 0.72)
                radius: Math.round(10 * trackViewRoot.ks)
                color: trackViewRoot.markerColor(trackViewRoot.liveState)
                Behavior on color { enabled: trackViewRoot.animate; ColorAnimation { duration: 150 } }     // R402_FADE
                // a section pinned by hand: a white ring
                border.width: trackManager && trackManager.overrideState !== "" ? 3 : 0
                border.color: "#FFFFFF"
                Text
                {
                    anchors.centerIn: parent
                    text: trackViewRoot.liveState.toUpperCase()
                    color: "#141005"
                    font.bold: true
                    font.pixelSize: trackViewRoot.fs(19)
                    font.letterSpacing: (trackViewRoot.fs(19)) * 0.1
                }
            }

            Column
            {
                anchors.left: sectionPill.right
                anchors.leftMargin: Math.round(22 * trackViewRoot.ks)
                anchors.right: bigClock.left   // R481_CLOCK_TITLE: the clock sits left of LAST TRACK & co. (R458_LAST_BTN)
                anchors.rightMargin: Math.round(22 * trackViewRoot.ks)
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3
                Row
                {
                    width: parent.width
                    spacing: Math.round(12 * trackViewRoot.ks)
                    Text
                    {
                        width: Math.min(implicitWidth, parent.width - (testChip.visible ? testChip.width + parent.spacing : 0))
                        elide: Text.ElideRight
                        text: trackManager && trackManager.title !== "" ? trackManager.title : qsTr("No track loaded")
                        color: trackViewRoot.cText
                        font.pixelSize: trackViewRoot.fs(23)
                    }
                    // R450_TEST_TRACK: the made-up track says so
                    Rectangle
                    {
                        id: testChip
                        objectName: "testChip"
                        visible: trackViewRoot.testOn
                        anchors.verticalCenter: parent.verticalCenter
                        width: testChipText.implicitWidth + Math.round(20 * trackViewRoot.ks)
                        height: Math.round(28 * trackViewRoot.ks)
                        radius: Math.round(5 * trackViewRoot.ks)
                        color: trackViewRoot.cGold
                        Text
                        {
                            id: testChipText
                            anchors.centerIn: parent
                            text: qsTr("TEST") + " \u00B7 " + (trackManager ? Math.round(trackManager.testBpm) : 0) + " BPM"
                            color: "#1A1408"
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(14)
                        }
                    }
                }
                Text
                {
                    width: parent.width
                    elide: Text.ElideRight
                    property var nm: trackViewRoot.nextMarker()
                    text:
                    {
                        if (nm === null) return qsTr("No further points")
                        var d = nm.beat - trackViewRoot.currentBeat
                        // bars rounded UP, like the countdown in the waveform
                        var bars = Math.ceil(d / 4)
                        return qsTr("Next") + ": " + nm.type.toUpperCase()
                               + " " + qsTr("in") + " " + d + " " + (d === 1 ? qsTr("beat") : qsTr("beats"))
                               + "  ·  " + bars + " " + (bars === 1 ? qsTr("bar") : qsTr("bars"))
                    }
                    color: nm === null ? trackViewRoot.cDim : trackViewRoot.markerColor(nm.type)
                    font.pixelSize: trackViewRoot.fs(15)
                }
            }

            // R458_LAST_BTN (Tobias 10-15, mockup A/B/C approved): from ten minutes
            // before closing (03:00, New Year's night 05:00 - the closing's clock). A
            // tap asks SURE?, the second makes this track the last - or undoes it
            Rectangle
            {
                id: lastBtn
                objectName: "lastTrack"
                readonly property int stage: trackEngine ? trackEngine.lastTrack : 0
                property bool sure: false
                readonly property bool filled: sure || stage > 0
                visible: !!trackEngine && trackEngine.lastTrackShown === true && !trackViewRoot.testOn
                anchors.right: testStop.visible ? testStop.left : (oldShowChip.visible ? oldShowChip.left : statusRight.left)
                anchors.rightMargin: Math.round(22 * trackViewRoot.ks)
                anchors.verticalCenter: parent.verticalCenter
                width: Math.round(252 * trackViewRoot.ks)
                height: Math.round(trackViewRoot.topH * 0.70)
                radius: Math.round(8 * trackViewRoot.ks)
                color: filled ? trackViewRoot.cGold : (lastIn.down ? "#2A2418" : trackViewRoot.cBtn)
                border.width: filled ? 0 : 2
                border.color: trackViewRoot.cGold
                onStageChanged: sure = false
                Timer { id: lastSureOff; interval: 4000; onTriggered: lastBtn.sure = false }
                // R481_LAST_BLINK (Tobias 10-19: "giv last track knappen noget blink ... saa man ligger
                // maerke til den"): shown and not armed, the gold fill breathes - flat, no glow
                Rectangle
                {
                    id: lastBreath
                    objectName: "lastBreath"
                    anchors.fill: parent
                    radius: parent.radius
                    color: trackViewRoot.cGold
                    opacity: 0
                    readonly property bool on: lastBtn.visible && lastBtn.stage === 0 && !lastBtn.sure && trackViewRoot.animate
                    onOnChanged: if (!on) opacity = 0
                    SequentialAnimation on opacity
                    {
                        running: lastBreath.on
                        loops: Animation.Infinite
                        NumberAnimation { from: 0; to: 0.45; duration: 800; easing.type: Easing.InOutSine }
                        NumberAnimation { from: 0.45; to: 0; duration: 800; easing.type: Easing.InOutSine }
                    }
                }
                function clock(s) { return Math.floor(s / 60) + ":" + ("0" + (s % 60)).slice(-2) }
                readonly property string sub:
                {
                    if (!trackEngine) return ""
                    if (sure) return stage > 0 ? qsTr("tap again to undo it") : qsTr("tap again - this track ends the night")
                    var left = trackEngine.lastTrackLeft
                    if (stage === 1 || stage === 2) return left >= 0 ? qsTr("ends with the track") + " \u00B7 " + clock(left) : qsTr("ends with the track")
                    if (stage === 3) return qsTr("white light") + " \u00B7 " + qsTr("dark in") + " " + clock(left)
                    if (stage === 4) return qsTr("dark") + " \u00B7 " + qsTr("tap to undo")
                    var m = trackEngine.lastTrackCloseMin
                    return qsTr("closing") + " " + trackEngine.lastTrackCloseAt + (m > 0 ? " \u00B7 " + m + " " + qsTr("min") : "")
                }
                readonly property color ink: filled ? "#1A1408" : trackViewRoot.cGold
                Row
                {
                    anchors.left: parent.left
                    anchors.leftMargin: Math.round(14 * trackViewRoot.ks)
                    anchors.right: parent.right
                    anchors.rightMargin: Math.round(8 * trackViewRoot.ks)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Math.round(10 * trackViewRoot.ks)
                    visible: !lastBtn.sure
                    ControlIcon { id: lastMoon; anchors.verticalCenter: parent.verticalCenter; width: Math.round(20 * trackViewRoot.ks); height: width; kind: "moon"; ink: lastBtn.ink }
                    Column
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - lastMoon.width - parent.spacing
                        spacing: 2
                        Text { width: parent.width; elide: Text.ElideRight; text: qsTr("LAST TRACK"); color: lastBtn.ink; font.bold: true; font.pixelSize: trackViewRoot.fs(15); font.letterSpacing: trackViewRoot.fs(15) * 0.04 }
                        Text { objectName: "lastTrackSub"; width: parent.width; elide: Text.ElideRight; text: lastBtn.sub; color: lastBtn.filled ? "#1A1408" : trackViewRoot.cDim; font.pixelSize: trackViewRoot.fs(11) }
                    }
                }
                Column
                {
                    anchors.centerIn: parent
                    spacing: 2
                    visible: lastBtn.sure
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("SURE?"); color: "#1A1408"; font.bold: true; font.pixelSize: trackViewRoot.fs(16) }
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: lastBtn.sub; color: "#1A1408"; font.pixelSize: trackViewRoot.fs(10) }
                }
                TouchInput
                {
                    id: lastIn
                    onReleasedAt: (x, y, inside) =>
                    {
                        if (!inside || !trackEngine) return
                        if (!lastBtn.sure) { lastBtn.sure = true; lastSureOff.restart(); return }
                        lastBtn.sure = false
                        lastSureOff.stop()
                        trackEngine.toggleLastTrack()
                    }
                }
            }

            // TRACK_APP_TOP_R549: TRACK / VIRTUAL CONSOLE over the clock (MainView's kiosk strip is
            // hidden on this page in the TRACK app; the Virtual Console keeps it, with its TRACK tab)
            Row
            {
                id: appTabs
                objectName: "appTabs"
                visible: trackViewRoot.trackApp
                x: Math.round(bigClock.x + (bigClock.width - width) / 2)
                anchors.bottom: bigClock.top
                anchors.bottomMargin: Math.round(2 * trackViewRoot.ks)
                spacing: Math.round(4 * trackViewRoot.ks)
                Repeater
                {
                    // TRACK_APP_TOP_R551: no TRACK button here - the Virtual Console has its own
                    model: [ { key: "VC", label: qsTr("VIRTUAL CONSOLE") } ]
                    Rectangle
                    {
                        objectName: "appTab" + modelData.key
                        readonly property bool on: modelData.key === "TRACK"
                        width: appTabLabel.implicitWidth + Math.round(28 * trackViewRoot.ks)
                        height: trackViewRoot.appTabsH - Math.round(4 * trackViewRoot.ks)
                        radius: Math.round(6 * trackViewRoot.ks)
                        color: on ? "#2A6FDB" : trackViewRoot.cBtn
                        Text
                        {
                            id: appTabLabel
                            anchors.centerIn: parent
                            text: modelData.label
                            color: parent.on ? "#FFFFFF" : trackViewRoot.cMute
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(12)
                            font.letterSpacing: trackViewRoot.fs(12) * 0.06
                        }
                        TouchInput
                        {
                            onReleasedAt: (x, y, inside) => {
                                if (inside && !parent.on && typeof mainView !== "undefined" && typeof vcEntry !== "undefined")
                                    mainView.switchToContext(vcEntry.ctxName, vcEntry.ctxRes)
                            }
                        }
                    }
                }
            }

            // R450_TEST_TRACK (mockup 4): STOP TEST while the made-up track plays
            Rectangle
            {
                id: testStop
                objectName: "testStop"
                visible: trackViewRoot.testOn
                anchors.right: oldShowChip.visible ? oldShowChip.left : statusRight.left
                anchors.rightMargin: Math.round(22 * trackViewRoot.ks)
                anchors.verticalCenter: parent.verticalCenter
                width: testStopRow.implicitWidth + Math.round(44 * trackViewRoot.ks)
                height: Math.round(trackViewRoot.topH * 0.70)
                radius: Math.round(8 * trackViewRoot.ks)
                color: testStopIn.down ? "#4A3A18" : "#2A2418"
                border.width: 2
                border.color: trackViewRoot.cGold
                Row
                {
                    id: testStopRow
                    anchors.centerIn: parent
                    spacing: Math.round(12 * trackViewRoot.ks)
                    ControlIcon { anchors.verticalCenter: parent.verticalCenter; width: Math.round(18 * trackViewRoot.ks); height: width; kind: "stop"; ink: trackViewRoot.cGold }
                    Text
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("STOP TEST")
                        color: trackViewRoot.cGold
                        font.bold: true
                        font.pixelSize: trackViewRoot.fs(16)
                        font.letterSpacing: trackViewRoot.fs(16) * 0.06
                    }
                }
                TouchInput { id: testStopIn; onReleasedAt: (x, y, inside) => { if (inside && trackManager) trackManager.stopTestTrack() } }
            }

            // R436_SHOW_CHECK (mock 7b): after CONTINUE ANYWAY, until the show file
            // is replaced - a tap shows what is missing again
            Rectangle
            {
                id: oldShowChip
                objectName: "oldShowChip"
                visible: trackViewRoot.oldShow && trackEngine && trackEngine.showAcked
                anchors.right: statusRight.left
                anchors.rightMargin: Math.round(22 * trackViewRoot.ks)
                anchors.verticalCenter: parent.verticalCenter
                width: chipText.implicitWidth + Math.round(36 * trackViewRoot.ks)
                height: Math.round(trackViewRoot.topH * 0.66)
                radius: Math.round(8 * trackViewRoot.ks)
                color: "#3C2E12"
                border.width: 2
                border.color: trackViewRoot.cGold
                Text
                {
                    id: chipText
                    anchors.centerIn: parent
                    text: "⚠  " + qsTr("OLD SHOW FILE") + "  ·  " + qsTr("before r%1").arg(trackEngine ? trackEngine.showWants : 0)
                    color: trackViewRoot.cGold
                    font.bold: true
                    font.pixelSize: trackViewRoot.fs(15)
                }
                TouchInput { onReleasedAt: (x, y, inside) => { if (inside) trackViewRoot.oldShowOpen = true } }
            }

            Row
            {
                id: statusRight
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Math.round(18 * trackViewRoot.ks)

                // R474_UPDATE (Tobias 10-16, mockup approved): a newer rig build - nothing at all
                // until there is one; SURE? (5 s); only with SHOW OFF (the manager says so)
                Rectangle
                {
                    id: updChip
                    objectName: "updateChip"
                    // TRACK_APP_LOADTEST_R605: also when a fetched build cannot start on this PC - it says so, red, and does nothing
                    readonly property string blocked: trackViewRoot.trackApp && trackAppCtl.updateBlocked !== undefined ? trackAppCtl.updateBlocked : ""
                    readonly property bool onlyBlocked: blocked !== "" && !(trackViewRoot.trackApp && trackAppCtl.updateAvailable === true)
                    visible: trackViewRoot.trackApp ? (trackAppCtl.updateAvailable === true || blocked !== "")   // TRACK_APP_UPD_R571: during the show too
                                                    : (trackManager ? trackManager.updateAvailable === true : false)
                    readonly property int st: trackViewRoot.trackApp ? trackAppCtl.updateStep
                                            : (trackManager && trackManager.updateStep !== undefined ? trackManager.updateStep : 0)
                    readonly property bool showOn: trackManager ? trackManager.autoRun === true : false
                    readonly property string latest: trackViewRoot.trackApp ? trackAppCtl.updateLatest
                                                   : (trackManager && trackManager.updateLatest ? trackManager.updateLatest : "")
                    function go() { if (trackViewRoot.trackApp) trackAppCtl.startUpdate(updChip.showOn); else trackManager.startUpdate() }
                    property bool arm: false
                    Timer { id: updArm; interval: 5000; onTriggered: updChip.arm = false }
                    onShowOnChanged: if (showOn) arm = false
                    anchors.verticalCenter: parent.verticalCenter
                    // R475_DESK_UPD: one width - SURE?'s longer line took the track's title away at 1366
                    width: Math.round(236 * trackViewRoot.ks)
                    property double armAt: 0
                    height: Math.max(48, Math.round(48 * trackViewRoot.ks))
                    radius: Math.round(10 * trackViewRoot.ks)
                    color: arm ? trackViewRoot.cGold : "#141418"
                    border.width: arm ? 0 : 2
                    border.color: onlyBlocked ? "#E03030" : trackViewRoot.cGold                    // TRACK_APP_LOADTEST_R605
                    opacity: showOn && !arm && !onlyBlocked ? 0.6 : 1.0
                    Rectangle
                    {
                        visible: !updChip.arm
                        x: Math.round(16 * trackViewRoot.ks); anchors.verticalCenter: parent.verticalCenter
                        width: Math.round(10 * trackViewRoot.ks); height: width; radius: width / 2
                        color: updChip.onlyBlocked ? "#E03030" : trackViewRoot.cGold                 // TRACK_APP_LOADTEST_R605
                    }
                    Column
                    {
                        id: updCol
                        x: Math.round((updChip.arm ? 18 : 36) * trackViewRoot.ks)
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 1
                        width: updChip.width - updCol.x - Math.round(12 * trackViewRoot.ks)   // R475_CHIPTEXT_DESK
                        Text
                        {
                            objectName: "updateChipText"
                            width: parent.width; elide: Text.ElideRight
                            text: updChip.onlyBlocked ? qsTr("UPDATE BLOCKED")                         // TRACK_APP_LOADTEST_R605
                                : updChip.st === 1 ? qsTr("UPDATE STARTED") : updChip.arm ? qsTr("SURE? - TAP AGAIN")
                                : trackViewRoot.trackApp ? qsTr("UPDATE TRACK") : qsTr("UPDATE QLC+")
                            color: updChip.onlyBlocked ? "#FF6A6A" : updChip.arm ? "#1A1408" : trackViewRoot.cGold
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(16)
                            font.letterSpacing: trackViewRoot.fs(16) * 0.06
                        }
                        Text
                        {
                            objectName: "updateChipSub"
                            width: parent.width; elide: Text.ElideRight   // R475_CHIPSUB_DESK: SURE? over a tap's "SHOW OFF first"
                            text: updChip.onlyBlocked ? qsTr("%1 won't start on this PC").arg(updChip.blocked.split("|")[0])   // TRACK_APP_LOADTEST_R605
                                : updChip.st === 1 ? (trackViewRoot.trackApp ? qsTr("TRACK restarts by itself") : qsTr("QLC+ restarts by itself"))
                                : updChip.showOn ? qsTr("SHOW OFF first")
                                : updChip.arm ? (trackViewRoot.trackApp ? qsTr("light out about 10 s") : qsTr("light out about 1 min"))
                                : updChip.st === 2 ? (trackViewRoot.trackApp ? qsTr("did not install - see the log") : qsTr("update-qlc.cmd not found"))
                                : updChip.st === 3 ? qsTr("SHOW OFF first")
                                : trackViewRoot.trackApp && trackAppCtl.updateWhat !== ""
                                  ? trackAppCtl.updateWhat      // TRACK_APP_SHOW_R558
                                  : qsTr("build %1 is ready").arg(updChip.latest)
                            color: updChip.arm ? "#3C2D0A" : trackViewRoot.cMute
                            font.pixelSize: trackViewRoot.fs(12)
                        }
                    }
                    TouchInput
                    {
                        onReleasedAt: (x, y, inside) => {
                            if (!inside || !trackManager || updChip.st === 1 || updChip.onlyBlocked)   // TRACK_APP_LOADTEST_R605
                                return
                            if (updChip.showOn) { updChip.go(); return }   // "SHOW OFF first"
                            if (!updChip.arm) { updChip.arm = true; updChip.armAt = Date.now(); updArm.restart(); return }
                            if (Date.now() - updChip.armAt < 400) return   // R475_CHIPTAP_DESK: a double tap is not SURE? answered
                            updChip.arm = false; updArm.stop()
                            updChip.go()
                        }
                    }
                }

                // R402_BEAT_DOTS (Tobias 10-08: "kun fire beat prikker ved uret"): the beat
                // in the bar - the one playing lit, the downbeat white - and a small
                // pop on each beat (R402_BEAT_PULSE)
                Row
                {
                    objectName: "beatDots"
                    visible: !trackViewRoot.trackApp          // TRACK_APP_TOP_R549: on the time's line instead
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Math.round(8 * trackViewRoot.ks)
                    Repeater
                    {
                        model: 4
                        Rectangle
                        {
                            id: beatDot
                            objectName: "beatDot" + index
                            readonly property bool on: trackManager && trackManager.playing && trackViewRoot.beatInBar === index
                            width: Math.round(12 * trackViewRoot.ks); height: width; radius: width / 2
                            anchors.verticalCenter: parent.verticalCenter
                            color: on ? (index === 0 ? "#FFFFFF" : trackViewRoot.cGreen) : Qt.rgba(1, 1, 1, 0.14)
                            onOnChanged: if (on && trackViewRoot.animate) beatPop.restart()
                            ScaleAnimator { id: beatPop; target: beatDot; from: 1.45; to: 1.0; duration: 180; easing.type: Easing.OutQuad }
                        }
                    }
                }

                Column
                {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3
                    // TRACK_APP_TOP_R549 (Tobias 11-04: "ryk ogsaa de 4 beat-prikker op over paused og bpm, saa
                    // den ikke fylder saa meget i breden"): in the TRACK app the dots are on the time's line
                    Row
                    {
                        anchors.right: parent.right
                        spacing: Math.round(14 * trackViewRoot.ks)
                        Row
                        {
                            objectName: "appBeatDots"
                            visible: trackViewRoot.trackApp
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: Math.round(8 * trackViewRoot.ks)
                            Repeater
                            {
                                model: 4
                                Rectangle
                                {
                                    id: appBeatDot
                                    objectName: "appBeatDot" + index
                                    readonly property bool on: trackManager && trackManager.playing && trackViewRoot.beatInBar === index
                                    width: Math.round(12 * trackViewRoot.ks); height: width; radius: width / 2
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: on ? (index === 0 ? "#FFFFFF" : trackViewRoot.cGreen) : Qt.rgba(1, 1, 1, 0.14)
                                    onOnChanged: if (on && trackViewRoot.animate) appBeatPop.restart()
                                    ScaleAnimator { id: appBeatPop; target: appBeatDot; from: 1.45; to: 1.0; duration: 180; easing.type: Easing.OutQuad }
                                }
                            }
                        }
                        Text
                        {
                            anchors.verticalCenter: parent.verticalCenter
                            text: trackViewRoot.fmtTime(trackManager ? trackManager.positionMs : 0)
                                  + " / " + trackViewRoot.fmtTime(trackManager ? trackManager.durationMs : 0)
                            color: trackViewRoot.cText
                            font.pixelSize: trackViewRoot.fs(21)
                        }
                    }
                    Text
                    {
                        anchors.right: parent.right
                        visible: !appWarn.visible && !appDeckWarn.visible   // TRACK_APP_WARN_R556 / TRACK_APP_UI_R560: a warning takes its place
                        // runde 308: the tempo the engine times on, and from where.
                        // Anything but LINK is amber
                        text: (trackManager && trackManager.playing ? qsTr("PLAYING") : qsTr("PAUSED"))
                              + "  ·  " + (trackManager && trackManager.engineTempo !== undefined ? trackManager.engineTempo.toFixed(1) : "0") + " BPM"
                              + (trackManager && trackManager.tempoSource ? "  ·  " + trackManager.tempoSource : "")
                              + "  ·  " + (trackViewRoot.trackApp ? (trackViewRoot.cdjCount > 0 ? (trackAppCtl.deckText !== "" ? trackAppCtl.deckText : qsTr("CDJs ok")) : qsTr("no CDJs"))
                                           : (trackManager && trackManager.connected ? qsTr("BLT ok") : qsTr("no BLT")))
                              + (trackViewRoot.testOn ? "  ·  " + qsTr("TEST") : "")   // R450_TEST_TRACK
                        color: trackManager && trackManager.playing
                               ? (trackManager.tempoSource === "LINK" ? "#59C36A" : "#E3B44F")
                               : trackViewRoot.cDim
                        font.pixelSize: trackViewRoot.fs(13)
                    }
                    // TRACK_APP_WARN_R556: what is wrong with the CDJs, in red - only with SHOW ON, after 5 s
                    Rectangle
                    {
                        id: appWarn
                        objectName: "appWarn"
                        readonly property string why: !trackViewRoot.trackApp || !trackManager || trackManager.autoRun !== true ? ""
                            : (trackManager.connected !== true ? qsTr("CDJ READER DOWN")
                               : (trackViewRoot.cdjCount <= 0 ? qsTr("NO CDJs")
                                  : (trackManager.linkStale === true ? qsTr("CDJ LINK LOST") : "")))
                        property string shown: ""
                        // TRACK_APP_FIX_R590 (#51): one failure turning into another (READER DOWN -> NO CDJs -> LINK LOST
                        // while the reader restarts) only changes the words - the red pill went out for 5 s at each step
                        onWhyChanged: {
                            if (why === "") { shown = ""; appWarnLate.stop() }
                            else if (shown !== "") shown = why
                            else appWarnLate.restart()
                        }
                        Component.onCompleted: if (why !== "") appWarnLate.restart()
                        Timer { id: appWarnLate; interval: 5000; onTriggered: appWarn.shown = appWarn.why }
                        visible: shown !== ""
                        anchors.right: parent.right
                        width: appWarnText.implicitWidth + Math.round(22 * trackViewRoot.ks)
                        height: Math.round(22 * trackViewRoot.ks)
                        radius: height / 2
                        color: "#E03030"
                        SequentialAnimation on opacity
                        {
                            running: appWarn.visible && trackViewRoot.animate
                            loops: Animation.Infinite
                            NumberAnimation { to: 0.55; duration: 700 }
                            NumberAnimation { to: 1.0; duration: 700 }
                        }
                        Text
                        {
                            id: appWarnText
                            anchors.centerIn: parent
                            text: appWarn.shown
                            color: "#FFFFFF"
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(12)
                            font.letterSpacing: trackViewRoot.fs(12) * 0.08
                        }
                    }
                    // TRACK_APP_UI_R560: amber - another deck the room hears has played for 8 s while the followed one
                    // stands still (TrackApp::deckCheck); with SHOW ON, under the red one
                    Rectangle
                    {
                        id: appDeckWarn
                        objectName: "appDeckWarn"
                        readonly property string shown: trackViewRoot.trackApp && trackManager && trackManager.autoRun === true
                                                         && !appWarn.visible ? trackAppCtl.deckWarn : ""
                        visible: shown !== ""
                        anchors.right: parent.right
                        width: appDeckWarnText.implicitWidth + Math.round(22 * trackViewRoot.ks)
                        height: Math.round(22 * trackViewRoot.ks)
                        radius: height / 2
                        color: "#D6962A"
                        Text
                        {
                            id: appDeckWarnText
                            anchors.centerIn: parent
                            text: appDeckWarn.shown
                            color: "#140C02"
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(12)
                            font.letterSpacing: trackViewRoot.fs(12) * 0.08
                        }
                    }
                }

                // R379_SHOW_SWITCH: one tap on, SURE? off - its off face reads START SHOW
                Rectangle
                {
                    id: showSwitch
                    objectName: "showSwitch"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.round(214 * trackViewRoot.ks)
                    height: Math.max(48, Math.round(48 * trackViewRoot.ks))
                    radius: Math.round(10 * trackViewRoot.ks)
                    property bool checked: trackManager ? trackManager.autoRun : false
                    // runde 296: SHOW OFF asks SURE? and waits 4 s for the second tap
                    property bool armOff: false
                    // TRACK_APP_FIX_R591 (#67): the show switched from somewhere else (the phone, MIDI) while SURE? was up
                    // left it armed - one tap then stopped a running show
                    onCheckedChanged: armOff = false
                    property string text: checked ? (armOff ? qsTr("SURE?") : qsTr("SHOW ON")) : qsTr("START SHOW")
                    Timer { id: showOffArm; interval: 4000; onTriggered: showSwitch.armOff = false }
                    color: armOff ? "#E3B44F" : (checked ? "#3FBF3F" : (trackViewRoot.trackApp ? "#122814" : "#4A1E1E"))   // TRACK_APP_UI_R560: ready, not red
                    border.width: !checked && !armOff && trackViewRoot.trackApp ? 2 : 0
                    border.color: "#59C36A"
                    Behavior on color { enabled: trackViewRoot.animate; ColorAnimation { duration: 150 } }     // R402_FADE
                    scale: showIn.down ? 0.965 : 1.0                                                         // R402_PRESS
                    Behavior on scale { enabled: trackViewRoot.animate; NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
                    function tap()
                    {
                        if (!checked) { trackManager.autoRun = true; return }
                        if (!armOff) { armOff = true; showOffArm.restart(); return }
                        armOff = false
                        trackManager.autoRun = false
                    }
                    Row
                    {
                        anchors.centerIn: parent
                        spacing: 10
                        ControlIcon { anchors.verticalCenter: parent.verticalCenter; width: Math.round(16 * trackViewRoot.ks); height: width
                                      kind: "showSwitch"; ink: showSwitch.checked ? "#0A2A0A" : (trackViewRoot.trackApp ? "#8CE696" : "#FFC8C8") }
                        Text
                        {
                            anchors.verticalCenter: parent.verticalCenter
                            text: showSwitch.text
                            color: showSwitch.checked ? "#0A2A0A" : (trackViewRoot.trackApp ? "#8CE696" : "#FFC8C8")
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(18)
                            font.letterSpacing: (trackViewRoot.fs(18)) * 0.1
                        }
                    }
                    TouchInput { id: showIn; onReleasedAt: (x, y, inside) => { if (inside) showSwitch.tap() } }
                }

                // runde 393: the guide - between the show switch and SETUP
                Btn
                {
                    objectName: "helpSwitch"
                    // the guide explains the deck: no deck (the classic slot page), no HELP
                    visible: trackManager ? trackManager.roleMode : false
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.round(118 * trackViewRoot.ks)
                    height: Math.max(48, Math.round(48 * trackViewRoot.ks))
                    icon: "help"
                    text: trackViewRoot.helpOpen ? qsTr("CLOSE") : qsTr("HELP")
                    active: trackViewRoot.helpOpen
                    tone: trackViewRoot.cGold
                    onTapped: { trackViewRoot.helpOpen = !trackViewRoot.helpOpen; trackViewRoot.setupOpen = false }
                }
                Btn
                {
                    objectName: "setupSwitch"
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.round(132 * trackViewRoot.ks)
                    height: Math.max(48, Math.round(48 * trackViewRoot.ks))
                    icon: "setupSwitch"
                    text: trackViewRoot.setupOpen ? qsTr("CLOSE") : qsTr("SETUP")
                    onTapped: { trackViewRoot.setupOpen = !trackViewRoot.setupOpen; trackViewRoot.helpOpen = false }
                }
            }
        }

        // ============ 2 · THE TRACK ============
        Rectangle
        {
            id: waveCard
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 160
            color: "#111115"
            radius: Math.round(14 * trackViewRoot.ks)
            border.width: trackViewRoot.zoomActive ? 2 : 0      // R403_NO_LINES: the orange frame when zoomed
            border.color: trackViewRoot.zoomActive ? "#E0921A" : trackViewRoot.cEdge
            clip: true

            // ---- the drawing: section strip with the analysed energy, the
            //      waveform, bass as a warm floor, highs as a cool line, kicks
            //      as ticks, the played part of this section in the running
            //      colour, the flags and the playhead
            Canvas
            {
                id: wfCanvas
                x: Math.round(12 * trackViewRoot.ks)
                y: 0
                width: parent.width - 2 * x
                height: parent.height - trackViewRoot.railH
                renderStrategy: Canvas.Threaded

                property string paintedColour: ""
                // the curves once per track, not once per paint
                property var wfC: trackManager ? trackManager.waveform : []
                property var lowC: trackManager ? trackManager.lowCurve : []
                property var highC: trackManager ? trackManager.highCurve : []
                property var kickC: trackManager ? trackManager.kickCurve : []
                property int selected: -1          // the selected flag, an index into trackManager.markers
                property real stripH: Math.max(34, Math.round(44 * trackViewRoot.ks))

                // runde 396: in MARKERS, every flag has a handle at the foot of the track -
                // a tab the thumb can take, not a 2 px line (Tobias 10-07: "de kan godt
                // vaere lidt svaere at hive i med tommelfingeren"). Two rows: a handle
                // that would cover its neighbour goes up one.
                readonly property real handleH: Math.max(44, Math.round(50 * trackViewRoot.ks))
                function handles()
                {
                    var out = []
                    if (!trackViewRoot.markerEdit || !trackManager || trackViewRoot.beatCount <= 0) return out
                    var vf = trackViewRoot.viewFirst(), vc = trackViewRoot.viewCount(), px = width / vc
                    var ks = trackViewRoot.ks
                    var base = height - 8
                    var rowEnd = [ -1e9, -1e9 ]
                    var sorted = sortedMarkers()
                    for (var j = 0; j < sorted.length; j++)
                    {
                        var m = sorted[j]
                        if (m.beat < vf - 4 || m.beat > vf + vc + 4) continue
                        var label = m.type.toUpperCase()
                        // the dragged one says where it lands
                        if (m.index === trackViewRoot.dragIndex) label += "  \u2192  " + qsTr("BAR") + " " + trackViewRoot.barOf(m.beat)
                        var w = Math.max(Math.round(84 * ks), Math.round((label.length * 10 + 44) * ks))
                        var fx = (m.beat - vf) * px
                        var x = Math.min(Math.max(fx - w / 2, 0), width - w)
                        var row = (x < rowEnd[0] + 6) ? 1 : 0
                        rowEnd[row] = Math.max(rowEnd[row], x + w)
                        out.push({ index: m.index, type: m.type, label: label, fx: fx, x: x, w: w,
                                   y: base - handleH - row * (handleH + Math.round(6 * ks)), h: handleH })
                    }
                    return out
                }
                function handleAt(x, y)
                {
                    var hs = handles()
                    // the upper row first: it lies over the lower one's line
                    for (var i = hs.length - 1; i >= 0; i--)
                        if (x >= hs[i].x - 4 && x <= hs[i].x + hs[i].w + 4 && y >= hs[i].y - 4 && y <= hs[i].y + hs[i].h + 4)
                            return hs[i].index
                    return -1
                }

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()

                function sortedMarkers()
                {
                    var mk = trackManager ? trackManager.markers : []
                    var out = []
                    for (var i = 0; i < mk.length; i++) out.push({ beat: trackViewRoot.markerBeat(i), type: mk[i].type, energy: mk[i].energy, index: i })
                    out.sort(function(a, b) { return a.beat - b.beat })
                    return out
                }

                onPaint:
                {
                    var ctx = getContext("2d")
                    var w = width, h = height
                    ctx.reset()
                    ctx.clearRect(0, 0, w, h)
                    var n = trackViewRoot.beatCount
                    if (n <= 0) return

                    var vf = trackViewRoot.viewFirst()
                    var vc = trackViewRoot.viewCount()
                    var px = w / vc
                    function xOf(beat) { return (beat - vf) * px }
                    var strip = stripH
                    var base = h - 8
                    var barTop = strip + 30 + (trackViewRoot.planShown ? trackViewRoot.laneH : 0)   // R443_PLAN_LANE
                    var ks = trackViewRoot.ks
                    var sorted = sortedMarkers()
                    var cur = trackManager.currentBeat

                    // section strip: bands (energy as strength) and the labels, in
                    // two rows so close flags stay readable (runde 178)
                    ctx.font = "bold " + Math.max(10, Math.round(11 * ks)) + "px sans-serif"
                    ctx.textBaseline = "middle"
                    var rowH = Math.floor((strip - 8) / 2)
                    var rowRight = [ -1e9, -1e9 ]
                    for (var j = 0; j < sorted.length; j++)
                    {
                        var m = sorted[j]
                        var endBeat = j + 1 < sorted.length ? sorted[j + 1].beat : n + 1
                        if (endBeat < vf || m.beat > vf + vc) continue
                        var col = Qt.color(trackViewRoot.markerColor(m.type))
                        var e = m.energy >= 0 ? m.energy : 0.5
                        var x0 = Math.max(0, xOf(m.beat)), x1 = Math.min(w, xOf(endBeat))
                        ctx.fillStyle = Qt.rgba(col.r, col.g, col.b, 0.22 + 0.55 * e)
                        ctx.fillRect(x0, 0, x1 - x0, 5)

                        var label = m.type.toUpperCase() + (m.energy >= 0 ? "  " + Math.round(m.energy * 100) + "%" : "")
                        var tw = ctx.measureText(label).width + 18
                        var mxp = xOf(m.beat)
                        var bx = Math.min(Math.max(mxp, 0), w - tw)
                        var lr = (bx < rowRight[0] + 3) ? 1 : 0
                        if (lr === 1 && bx < rowRight[1] + 3) lr = 0
                        rowRight[lr] = bx + tw
                        var ly = 6 + lr * rowH
                        if (m.index === selected)
                        {
                            ctx.fillStyle = "rgba(255,255,255,0.14)"
                            ctx.fillRect(bx, ly, tw, rowH)
                            ctx.strokeStyle = "rgba(255,255,255,0.9)"
                            ctx.lineWidth = 2
                            ctx.strokeRect(bx + 1, ly + 1, tw - 2, rowH - 2)
                        }
                        ctx.fillStyle = Qt.rgba(col.r, col.g, col.b, 1)
                        ctx.fillRect(bx, ly, 2, rowH)
                        ctx.fillText(label, bx + 10, ly + rowH / 2)
                    }
                    ctx.fillStyle = "#24242A"
                    ctx.fillRect(0, strip, w, 1)

                    // the played part of this section, in the running colour
                    if (cur > 0)
                    {
                        var secStart = 1
                        for (var s = 0; s < sorted.length; s++)
                            if (sorted[s].beat <= cur) secStart = sorted[s].beat
                        if (trackEngine && trackEngine.currentColour !== "")
                        {
                            var rc = Qt.color(trackViewRoot.swatch(trackEngine.currentColour))
                            ctx.fillStyle = Qt.rgba(rc.r, rc.g, rc.b, 0.13)
                            ctx.fillRect(xOf(secStart), strip + 1, xOf(cur) - xOf(secStart), base - strip - 1)
                        }
                    }

                    // the bar grid flags snap to - the track's own (BACKLOG 104)
                    var gridStep = trackViewRoot.zoomActive ? 4 : 32
                    var firstBar = (trackManager.downbeat || 0) + 1
                    ctx.strokeStyle = "rgba(255,255,255,0.07)"
                    ctx.lineWidth = 1
                    for (var gb = Math.ceil((vf - firstBar) / gridStep) * gridStep + firstBar; gb < vf + vc; gb += gridStep)
                    {
                        ctx.beginPath(); ctx.moveTo(Math.round(xOf(gb)) + 0.5, strip + 1); ctx.lineTo(Math.round(xOf(gb)) + 0.5, base); ctx.stroke()
                    }

                    // the waveform: the played part brighter
                    var wf = wfC
                    var bw = Math.max(1, px - (px > 4 ? 1.6 : 0.4))
                    for (var i = 0; i < vc; i++)
                    {
                        var b = vf + i
                        if (b < 1 || b > n) continue
                        var v = ((b - 1) < wf.length ? wf[b - 1] : 0) / 255.0
                        var bh = Math.max(1, v * (base - barTop))
                        ctx.fillStyle = (cur > 0 && b <= cur) ? "#3C7FC2" : "#26527F"
                        ctx.fillRect(xOf(b) + (px - bw) / 2, base - bh, bw, bh)
                    }

                    // R529_AI_UNSURE (Tobias 11-02, forslag 3, mockup approved): where the learnt sections are unsure
                    // (under 0.60 - BLT's aiConf, only on the learnt set and never over his own flags) faint diagonal
                    // stripes over those bars, so a glance shows where a flag is worth checking
                    var conf = (trackManager.aiConf && !(typeof trackManager.markersManual === "function" && trackManager.markersManual()))
                               ? trackManager.aiConf : []
                    if (conf.length > 0)
                    {
                        ctx.save()
                        ctx.strokeStyle = "rgba(255,255,255,0.20)"
                        ctx.lineWidth = 1
                        var ui = 0
                        while (ui < conf.length)
                        {
                            if (conf[ui][1] >= 0.60) { ui++; continue }
                            var uj = ui
                            while (uj + 1 < conf.length && conf[uj + 1][1] < 0.60) uj++
                            var ub0 = conf[ui][0], ub1 = uj + 1 < conf.length ? conf[uj + 1][0] : n + 1
                            ui = uj + 1
                            if (ub1 < vf || ub0 > vf + vc) continue
                            var ux0 = Math.max(0, xOf(ub0)), ux1 = Math.min(w, xOf(ub1))
                            var utop = strip + 1
                            ctx.save()
                            ctx.beginPath(); ctx.rect(ux0, utop, ux1 - ux0, base - utop); ctx.clip()
                            ctx.beginPath()
                            var uh = base - utop
                            for (var ux = ux0 - uh - (ux0 % 9); ux < ux1; ux += 9)
                            {
                                ctx.moveTo(ux, base); ctx.lineTo(ux + uh, utop)
                            }
                            ctx.stroke()
                            ctx.restore()
                        }
                        ctx.restore()
                    }

                    var step = Math.max(1, Math.floor(vc / w))
                    // bass: a warm floor over the bars, with a line on top
                    var low = lowC
                    if (low && low.length > 0)
                    {
                        ctx.beginPath()
                        ctx.moveTo(0, base)
                        var pts = []
                        for (var b1 = vf; b1 < vf + vc && b1 <= low.length; b1 += step)
                        {
                            var lv = 0
                            for (var k1 = 0; k1 < step && b1 - 1 + k1 < low.length; k1++) lv = Math.max(lv, low[b1 - 1 + k1])
                            var py = base - (lv / 255) * (base - strip) * 0.32
                            pts.push([xOf(b1) + px / 2, py])
                            ctx.lineTo(xOf(b1) + px / 2, py)
                        }
                        ctx.lineTo(w, base)
                        ctx.closePath()
                        ctx.fillStyle = "rgba(227, 180, 79, 0.13)"
                        ctx.fill()
                        ctx.beginPath()
                        for (var p = 0; p < pts.length; p++) { if (p === 0) ctx.moveTo(pts[p][0], pts[p][1]); else ctx.lineTo(pts[p][0], pts[p][1]) }
                        ctx.strokeStyle = "rgba(227, 180, 79, 0.55)"
                        ctx.lineWidth = 1.4
                        ctx.stroke()
                    }
                    // highs: a cool line in the upper third
                    var high = highC
                    if (high && high.length > 0)
                    {
                        ctx.beginPath()
                        var started = false
                        for (var b2 = vf; b2 < vf + vc && b2 <= high.length; b2 += step)
                        {
                            var hv = 0
                            for (var k2 = 0; k2 < step && b2 - 1 + k2 < high.length; k2++) hv = Math.max(hv, high[b2 - 1 + k2])
                            var hy = strip + (base - strip) * 0.30 - (hv / 255) * (base - strip) * 0.20
                            if (!started) { ctx.moveTo(xOf(b2) + px / 2, hy); started = true }
                            else ctx.lineTo(xOf(b2) + px / 2, hy)
                        }
                        ctx.strokeStyle = "rgba(127, 211, 255, 0.62)"
                        ctx.lineWidth = 1.4
                        ctx.stroke()
                    }
                    // kicks: ticks along the floor, brighter the harder
                    var kick = kickC
                    if (kick && kick.length > 0 && vc < w * 2)
                    {
                        for (var b3 = vf; b3 < vf + vc && b3 <= kick.length; b3++)
                        {
                            var kv = kick[b3 - 1] / 255
                            if (kv < 0.45) continue
                            ctx.fillStyle = "rgba(255, 106, 106, " + (0.2 + 0.55 * kv).toFixed(2) + ")"
                            ctx.fillRect(xOf(b3) + px * 0.25, base + 1, Math.max(1, px * 0.5), 3)
                        }
                    }

                    // the flags: a line down; the one held by the finger thicker,
                    // with the beat it would land on
                    var mk = trackManager.markers
                    for (var o = 0; o < mk.length; o++)
                    {
                        var mb = trackViewRoot.markerBeat(o)
                        if (mb < vf - 2 || mb > vf + vc + 2) continue
                        var fx = xOf(mb)
                        var held = (o === trackViewRoot.dragIndex)
                        var fc = Qt.color(trackViewRoot.markerColor(mk[o].type))
                        ctx.strokeStyle = Qt.rgba(fc.r, fc.g, fc.b, (held || o === selected) ? 1 : 0.75)
                        ctx.lineWidth = held ? 4 : (o === selected ? 3 : 1.5)
                        ctx.beginPath(); ctx.moveTo(fx, strip + 1); ctx.lineTo(fx, base); ctx.stroke()
                        if (held)
                        {
                            ctx.fillStyle = Qt.rgba(fc.r, fc.g, fc.b, 1)
                            ctx.font = "bold " + Math.max(11, Math.round(13 * ks)) + "px sans-serif"
                            ctx.fillText(qsTr("BAR") + " " + trackViewRoot.barOf(mb), fx + 8, strip + 16)
                        }
                    }

                    // the handles (MARKERS on)
                    var hs = handles()
                    ctx.textBaseline = "middle"
                    for (var hi = 0; hi < hs.length; hi++)
                    {
                        var hd = hs[hi]
                        var hc = Qt.color(trackViewRoot.markerColor(hd.type))
                        var sel = (hd.index === selected || hd.index === trackViewRoot.dragIndex)
                        // the stem from the flag line into the tab
                        ctx.strokeStyle = Qt.rgba(hc.r, hc.g, hc.b, 1); ctx.lineWidth = sel ? 3 : 2
                        ctx.beginPath(); ctx.moveTo(hd.fx, strip + 1); ctx.lineTo(hd.fx, hd.y); ctx.stroke()
                        var rr = Math.round(10 * ks)
                        ctx.beginPath()
                        ctx.moveTo(hd.x + rr, hd.y); ctx.lineTo(hd.x + hd.w - rr, hd.y); ctx.arcTo(hd.x + hd.w, hd.y, hd.x + hd.w, hd.y + rr, rr)
                        ctx.lineTo(hd.x + hd.w, hd.y + hd.h - rr); ctx.arcTo(hd.x + hd.w, hd.y + hd.h, hd.x + hd.w - rr, hd.y + hd.h, rr)
                        ctx.lineTo(hd.x + rr, hd.y + hd.h); ctx.arcTo(hd.x, hd.y + hd.h, hd.x, hd.y + hd.h - rr, rr)
                        ctx.lineTo(hd.x, hd.y + rr); ctx.arcTo(hd.x, hd.y, hd.x + rr, hd.y, rr); ctx.closePath()
                        ctx.fillStyle = sel ? Qt.rgba(hc.r, hc.g, hc.b, 1) : Qt.rgba(hc.r * 0.35 + 0.05, hc.g * 0.35 + 0.05, hc.b * 0.35 + 0.06, 0.96)
                        ctx.fill()
                        ctx.strokeStyle = sel ? "#FFFFFF" : Qt.rgba(hc.r, hc.g, hc.b, 1); ctx.lineWidth = sel ? 3 : 2; ctx.stroke()
                        // the grip and the type
                        var ink = sel ? "#101010" : Qt.lighter(Qt.rgba(hc.r, hc.g, hc.b, 1), 1.5)
                        ctx.fillStyle = ink
                        var gx = hd.x + Math.round(14 * ks), gy = hd.y + hd.h / 2
                        for (var gl = -1; gl <= 1; gl++) ctx.fillRect(gx, gy + gl * Math.round(5 * ks) - 1, Math.round(10 * ks), 2)
                        ctx.font = "bold " + Math.max(11, Math.round(13 * ks)) + "px sans-serif"
                        ctx.fillText(hd.label, gx + Math.round(18 * ks), gy + 1)
                    }

                    // the playhead
                    if (cur > 0 && cur >= vf && cur < vf + vc)
                    {
                        var ph = xOf(cur)
                        ctx.strokeStyle = "#FFFFFF"
                        ctx.lineWidth = 2
                        ctx.beginPath(); ctx.moveTo(ph, strip + 1); ctx.lineTo(ph, h); ctx.stroke()
                        ctx.fillStyle = "#FFFFFF"
                        ctx.beginPath(); ctx.moveTo(ph - 7, h); ctx.lineTo(ph + 7, h); ctx.lineTo(ph, h - 10); ctx.closePath(); ctx.fill()
                    }
                }

                Connections
                {
                    target: trackViewRoot
                    function onZoomActiveChanged() { wfCanvas.requestPaint() }
                    function onZoomCenterChanged() { wfCanvas.requestPaint() }
                    function onDragIndexChanged() { wfCanvas.requestPaint() }
                    function onPreviewBeatChanged() { wfCanvas.requestPaint() }
                    function onMarkerEditChanged() { wfCanvas.requestPaint() }
                }
            }

            // R402_BEAT_PULSE: a soft band on the playhead that fades out over the
            // first half of each beat
            Rectangle
            {
                id: playPulse
                objectName: "playPulse"
                readonly property int vf: trackViewRoot.viewFirst()
                readonly property int vc: trackViewRoot.viewCount()
                readonly property int cur: trackViewRoot.currentBeat
                visible: trackManager && trackManager.playing && cur > 0 && cur >= vf && cur < vf + vc
                width: Math.round(12 * trackViewRoot.ks)
                x: wfCanvas.x + (cur - vf) * wfCanvas.width / Math.max(1, vc) - width / 2
                y: wfCanvas.y + wfCanvas.stripH + 1
                height: wfCanvas.height - wfCanvas.stripH - 1
                color: "#FFFFFF"
                opacity: 0
                OpacityAnimator { id: playFade; target: playPulse; from: 0.30; to: 0.0; duration: 320; easing.type: Easing.OutQuad }
                Connections
                {
                    target: trackViewRoot
                    function onCurrentBeatChanged() { if (trackViewRoot.animate && playPulse.visible) playFade.restart() }
                }
            }

            // A finger on a flag - while the pencil is on: press = select it
            // (RETYPE / DELETE wake up), move = drag it. The flag keeps its
            // distance to the finger and the zoom opens around it.
            MouseArea
            {
                id: wfArea
                objectName: "waveformInput"
                x: wfCanvas.x
                y: wfCanvas.y
                width: wfCanvas.width
                height: wfCanvas.height
                // runde 395: SETUP or HELP over it ends a flag drag (onCanceled = release)
                enabled: trackViewRoot.beatCount > 0 && !trackViewRoot.setupOpen && !trackViewRoot.helpOpen
                preventStealing: true

                property int pressIndex: -1
                property real pressX: 0

                function beatAt(mx)
                {
                    return Math.round(mx / (width / trackViewRoot.viewCount()))
                           + trackViewRoot.viewFirst()
                }

                function press(px, py)
                {
                    if (!trackViewRoot.markerEdit) return
                    var hit = wfCanvas.handleAt(px, py)
                    if (hit >= 0)
                    {
                        pressIndex = hit
                        pressX = px
                        trackViewRoot.dragIndex = -1
                        wfCanvas.selected = hit
                        wfCanvas.requestPaint()
                        return
                    }
                    var b = beatAt(px)
                    var mk = trackManager.markers
                    var best = -1, bestDist = 1e9
                    for (var i = 0; i < mk.length; i++)
                    {
                        var d = Math.abs(mk[i].beat - b)
                        if (d < bestDist) { bestDist = d; best = i }
                    }
                    pressIndex = (best >= 0 && bestDist <= Math.max(3, trackViewRoot.viewCount() * 0.03)) ? best : -1
                    pressX = px
                    trackViewRoot.dragIndex = -1
                    wfCanvas.selected = pressIndex
                    wfCanvas.requestPaint()
                }

                // moveMarker() drops any flag already on the target bar: the drag
                // stops at the neighbour instead (runde 176, 179, 198)
                function barTaken(wantBeat)
                {
                    var snapped = trackViewRoot.snapBeat(wantBeat)
                    if (trackViewRoot.beatCount > 0)
                        snapped = Math.min(snapped, trackViewRoot.beatCount)
                    var mk = trackManager.markers
                    var from = (trackViewRoot.dragIndex >= 0 && trackViewRoot.dragIndex < mk.length)
                               ? mk[trackViewRoot.dragIndex].beat : snapped   // where it stands, not the preview
                    var lo = Math.min(from, snapped), hi = Math.max(from, snapped)
                    for (var i = 0; i < mk.length; i++)
                        if (i !== trackViewRoot.dragIndex
                            && (trackViewRoot.snapBeat(mk[i].beat) === snapped || (mk[i].beat > lo && mk[i].beat < hi)))
                            return true
                    return false
                }

                function reindex(wantBeat)
                {
                    var snapped = trackViewRoot.snapBeat(wantBeat)
                    var mk = trackManager.markers
                    var best = -1, bd = 1e9
                    for (var i = 0; i < mk.length; i++)
                    {
                        var d = Math.abs(mk[i].beat - snapped)
                        if (d < bd) { bd = d; best = i }
                    }
                    pressIndex = best
                    trackViewRoot.dragIndex = best
                    wfCanvas.selected = best
                }

                function move(px)
                {
                    if (pressIndex < 0) return
                    if (trackViewRoot.dragIndex < 0)
                    {
                        // a thumb trembles: a press that moves less than this only selects
                        if (Math.abs(px - pressX) < Math.max(10, Math.round(12 * trackViewRoot.ks))) return
                        var mk = trackManager.markers[pressIndex]
                        if (!mk) { pressIndex = -1; return }
                        trackViewRoot.dragIndex = pressIndex
                        trackViewRoot.zoomActive = true
                        var vc = trackViewRoot.viewCount()
                        trackViewRoot.zoomCenter = Math.round(mk.beat - px / width * vc + vc / 2)
                        trackViewRoot.dragOffset = mk.beat - beatAt(px)
                    }
                    trackViewRoot.dragX = px
                    preview(beatAt(px) + trackViewRoot.dragOffset)
                    var edge = width * 0.08
                    panTimer.dir = px < edge ? -1 : (px > width - edge ? 1 : 0)
                    panTimer.running = (panTimer.dir !== 0)
                    wfCanvas.requestPaint()
                }

                // the bar the flag would land on - up to a neighbour, never past it
                function preview(wantBeat)
                {
                    if (trackViewRoot.dragIndex < 0) return
                    if (barTaken(wantBeat)) return
                    var snapped = trackViewRoot.snapBeat(wantBeat)
                    if (trackViewRoot.beatCount > 0)
                        snapped = Math.max(1, Math.min(snapped, trackViewRoot.beatCount))
                    trackViewRoot.previewBeat = snapped
                }

                // commit: a finger let go on purpose - the flag moves once, to the preview.
                // Anything else (cancel, SETUP / HELP, a new track, DONE) moves nothing.
                function release(commit)
                {
                    var idx = trackViewRoot.dragIndex, to = trackViewRoot.previewBeat
                    panTimer.running = false
                    panTimer.dir = 0
                    pressIndex = -1
                    trackViewRoot.dragIndex = -1
                    trackViewRoot.previewBeat = -1
                    trackViewRoot.zoomActive = false
                    if (commit === true && idx >= 0 && to > 0 && trackManager
                        && idx < trackManager.markers.length && trackManager.markers[idx].beat !== to)
                    {
                        trackManager.moveMarker(idx, to)
                        wfCanvas.selected = idx
                    }
                    wfCanvas.requestPaint()
                }

                onPressed: (mouse) => press(mouse.x, mouse.y)
                onPositionChanged: (mouse) => move(mouse.x)
                onReleased: release(true)
                onCanceled: release(false)
                // disabled under a finger (SETUP / HELP opened): no cancel comes - end it here
                onEnabledChanged: if (!enabled) release()

                // the same, for one finger of its own (multitouch)
                MultiPointTouchArea
                {
                    anchors.fill: parent
                    mouseEnabled: false
                    maximumTouchPoints: 10
                    property int pid: -1          // one finger drags a flag; others are ignored (runde 390)
                    function mine(pts) { for (var i = 0; i < pts.length; i++) if (pts[i].pointId === pid) return pts[i]; return null }
                    onPressed: (pts) => { if (pid >= 0 || wfArea.pressed) return; pid = pts[0].pointId; wfArea.press(pts[0].x, pts[0].y) }
                    onUpdated: (pts) => { var p = mine(pts); if (p) wfArea.move(p.x) }
                    onReleased: (pts) => { if (mine(pts) === null) return; pid = -1; wfArea.release(true) }
                    onCanceled: (pts) => { if (pid < 0) return; pid = -1; wfArea.release() }
                    onEnabledChanged: if (!enabled) pid = -1     // runde 395: no stale finger after SETUP / HELP
                }
            }

            // R443_PLAN_LANE: the lane under the section labels - each section's
            // lamps: grey = ran, colour = on now, outlined = the engine's plan (thin:
            // its guess), a red line = his, MEM = his from the last time. A tap on
            // the NEXT section opens its card. After the flag input: it takes the taps.
            Item
            {
                id: planLaneBox
                objectName: "planLane"
                z: 2
                visible: trackViewRoot.planShown
                x: wfCanvas.x
                y: wfCanvas.stripH + 1
                width: wfCanvas.width
                height: trackViewRoot.laneH
                clip: true
                readonly property int vf: trackViewRoot.viewFirst()
                readonly property int vc: Math.max(1, trackViewRoot.viewCount())
                readonly property real px: width / vc
                Rectangle { anchors.fill: parent; color: "#101014" }
                Repeater
                {
                    model: trackViewRoot.planModel
                    Item
                    {
                        id: sec
                        property var md: modelData ? modelData : ({ beat: 0, end: 0, type: "", mode: "", next: false, chips: [] })
                        objectName: "planSec:" + md.beat
                        readonly property real x0: (md.beat - planLaneBox.vf) * planLaneBox.px
                        readonly property real x1: (md.end - planLaneBox.vf) * planLaneBox.px
                        readonly property bool his: md.mode === "saved" || md.mode === "his" || md.mode === "mem" || md.mode === "onhis"
                        readonly property bool filled: his || md.mode === "on"
                        readonly property string tag: md.mode === "saved" ? qsTr("SAVED") : md.mode === "mem" ? qsTr("MEM") : ((md.mode === "his" || md.mode === "onhis") ? qsTr("YOURS") : "")
                        readonly property bool tappable: trackViewRoot.planEditable(md)
                        readonly property real pad: Math.round(6 * trackViewRoot.ks)
                        readonly property real tagW: tag !== "" ? Math.max(trackViewRoot.fs(10) * 4.2 + 14, Math.round(64 * trackViewRoot.ks)) : 0
                        readonly property int n: Math.max(1, md.chips.length)
                        readonly property real fitW: Math.floor((width - 2 * pad - tagW) / n) - Math.round(4 * trackViewRoot.ks)
                        // a word needs this much (the type never goes under 10 px)
                        readonly property real wordW: trackViewRoot.fs(10) * 3.4 + 8
                        // a short section (or a far zoom): the chips are squares without words
                        readonly property bool compact: fitW < wordW
                        readonly property real chipW: compact ? Math.max(6, Math.min(Math.round(14 * trackViewRoot.ks), Math.floor((width - 2 * pad) / n) - 3))
                                                              : Math.min(Math.max(wordW, Math.round(52 * trackViewRoot.ks)), fitW)
                        visible: x1 > 0 && x0 < planLaneBox.width && width > 4
                        x: Math.max(0, x0)
                        width: Math.max(0, Math.min(planLaneBox.width, x1) - x)
                        height: parent.height
                        clip: true
                        Rectangle { anchors.fill: parent; visible: sec.tappable; color: trackViewRoot.planCardOpen && trackViewRoot.planCardBeat === sec.md.beat ? "#24242C" : "#18181E" }   // R484_PL_LANE_BG: the card's section
                        Rectangle { visible: sec.x0 >= 0; width: 2; height: parent.height; color: trackViewRoot.markerColor(sec.md.type) }
                        Row
                        {
                            x: sec.pad + 2
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: sec.compact ? 3 : Math.round(4 * trackViewRoot.ks)
                            Repeater
                            {
                                model: sec.md.chips
                                Rectangle
                                {
                                    objectName: "planChip:" + sec.md.beat + ":" + modelData.key
                                    readonly property color kc: trackViewRoot.kindColour(modelData.kind)
                                    width: sec.chipW
                                    height: sec.compact ? Math.round(12 * trackViewRoot.ks) : trackViewRoot.laneH - Math.round(16 * trackViewRoot.ks)
                                    radius: Math.round(4 * trackViewRoot.ks)
                                    color: sec.filled ? kc : (sec.md.mode === "ran" ? (sec.compact ? "#3A3A44" : "#1E1E24") : "#18181E")
                                    border.width: (sec.md.mode === "next" || sec.md.mode === "guess") ? 1 : 0
                                    border.color: sec.md.mode === "next" ? "#A0A0AA" : "#55555F"
                                    Text
                                    {
                                        visible: !sec.compact
                                        anchors.centerIn: parent
                                        text: modelData.short
                                        color: sec.filled ? "#0C0C10" : (sec.md.mode === "ran" ? "#62626C" : (sec.md.mode === "next" ? "#D0D0D8" : "#8C8C96"))
                                        font.bold: true
                                        font.pixelSize: trackViewRoot.fs(10)
                                    }
                                    Rectangle
                                    {
                                        visible: sec.his
                                        anchors.top: parent.bottom; anchors.topMargin: 1
                                        width: parent.width; height: Math.max(2, Math.round(3 * trackViewRoot.ks))
                                        color: "#E03030"
                                    }
                                }
                            }
                            Rectangle
                            {
                                objectName: "planTag:" + sec.md.beat
                                visible: sec.tag !== "" && !sec.compact
                                width: sec.tagW - Math.round(6 * trackViewRoot.ks)
                                height: trackViewRoot.laneH - Math.round(16 * trackViewRoot.ks)
                                radius: Math.round(4 * trackViewRoot.ks)
                                color: "#3A1416"
                                border.width: 1
                                border.color: "#E03030"
                                Text { anchors.centerIn: parent; text: sec.tag; color: "#F05050"; font.bold: true; font.pixelSize: trackViewRoot.fs(10) }
                            }
                        }
                        TouchInput
                        {
                            enabled: sec.tappable
                            onReleasedAt: (x, y, inside) => { if (inside) trackViewRoot.planTap(sec.md) }   // R484_PL_LANE_TAP: this section's card
                        }
                    }
                }
            }

            // R458_LAST_BTN (mockup C): the last track's outro - the lasers and the
            // strobes go out at the gold line, the soft white comes at the end
            Item
            {
                id: lastOutro
                objectName: "lastOutro"
                z: 2
                readonly property int stage: trackEngine ? trackEngine.lastTrack : 0
                readonly property int from:
                {
                    var n = trackViewRoot.beatCount
                    var mk = trackManager ? trackManager.markers : []
                    var best = -1
                    for (var i = 0; i < mk.length; i++)
                        if (mk[i].type === "outro") best = Math.max(best, trackViewRoot.markerBeat(i))
                    // R479_LAST_WHITE_LANE: the engine's end - the music's, not the file's
                    var end = (trackEngine && trackEngine.lastTrackEnd > 0) ? trackEngine.lastTrackEnd : n
                    return best > 0 ? best : Math.max(1, end - 31)
                }
                readonly property int vf: trackViewRoot.viewFirst()
                readonly property int vc: Math.max(1, trackViewRoot.viewCount())
                readonly property real x0: wfCanvas.x + Math.max(0, from - vf) * wfCanvas.width / vc
                visible: (stage === 1 || stage === 2) && trackViewRoot.beatCount > 0 && from < vf + vc && !trackViewRoot.markerEdit
                x: x0
                y: wfCanvas.y + wfCanvas.stripH + (trackViewRoot.planShown ? trackViewRoot.laneH : 0)
                width: Math.max(0, wfCanvas.x + wfCanvas.width - x0)
                height: wfCanvas.height - wfCanvas.stripH - (trackViewRoot.planShown ? trackViewRoot.laneH : 0)
                Rectangle { anchors.fill: parent; color: "#000000"; opacity: 0.45 }
                Rectangle { width: 2; height: parent.height; color: trackViewRoot.cGold }
                Column
                {
                    // at the foot: the countdown box sits at the top right
                    x: Math.round(8 * trackViewRoot.ks)
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: Math.round(14 * trackViewRoot.ks)
                    width: Math.max(0, parent.width - x - 4)
                    spacing: 2
                    Text { width: parent.width; elide: Text.ElideRight; text: qsTr("LAST \u00B7 OUTRO"); color: trackViewRoot.cGold; font.bold: true; font.pixelSize: trackViewRoot.fs(11) }
                    Text { width: parent.width; elide: Text.ElideRight; text: qsTr("lasers + strobes out"); color: trackViewRoot.cGold; font.pixelSize: trackViewRoot.fs(10) }
                    Text { width: parent.width; elide: Text.ElideRight; text: qsTr("full warm white at the end \u2192") /* R479_LAST_WHITE_RAIL */; color: trackViewRoot.cText; font.pixelSize: trackViewRoot.fs(10) }
                }
            }

            // R445_DROP_HIT: the armed hit on the next DROP flag
            Rectangle
            {
                id: hitTag
                objectName: "hitTag"
                z: 3
                readonly property int at: trackViewRoot.nextDropBeat()
                readonly property int vf: trackViewRoot.viewFirst()
                readonly property int vc: Math.max(1, trackViewRoot.viewCount())
                visible: !!trackEngine && trackEngine.hitKind > 0 && at > 0 && at >= vf && at < vf + vc && !trackViewRoot.markerEdit
                x: Math.min(wfCanvas.x + (at - vf) * wfCanvas.width / vc + 2, waveCard.width - width - Math.round(8 * trackViewRoot.ks))
                y: wfCanvas.stripH + (trackViewRoot.planShown ? trackViewRoot.laneH : 0) + Math.round(6 * trackViewRoot.ks)
                width: hitRow.implicitWidth + Math.round(18 * trackViewRoot.ks)
                height: Math.max(22, Math.round(26 * trackViewRoot.ks))
                radius: Math.round(5 * trackViewRoot.ks)
                color: "#3A1416"
                border.width: 1
                border.color: "#E23B3B"
                Row
                {
                    id: hitRow
                    anchors.centerIn: parent
                    spacing: Math.round(6 * trackViewRoot.ks)
                    ControlIcon { anchors.verticalCenter: parent.verticalCenter; width: Math.round(14 * trackViewRoot.ks); height: width; kind: "bolt"; ink: "#F05A5A" }
                    Text
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("HIT") + "  \u00B7  " + trackViewRoot.hitName(trackEngine ? trackEngine.hitKind : 0)
                        color: "#F05A5A"
                        font.bold: true
                        font.pixelSize: trackViewRoot.fs(13)
                    }
                }
            }

            // R452_EMPTY (Tobias 10-14, mockup C/D/E: "Waiting for track data ... paenere
            // ... en lille guide ... start Beat-link-trigger ... system-tray ... genstarte
            // det hvis det ikke virker"): what is missing, and (i) for how
            Item
            {
                id: emptyBox
                objectName: "emptyTrack"
                anchors.centerIn: wfCanvas
                visible: trackViewRoot.beatCount === 0 || emptyBox.gone   // R518_BLT_GONE_SHOW
                onVisibleChanged: if (!visible) trackViewRoot.bltGuideOpen = false   // R544_GUIDE_FORGET
                readonly property bool linked: !!trackManager && trackManager.connected === true
                // R518_BLT_GONE (Tobias 10-31, approved: "den skal bare tage over hvis den ser at BLT
                // forsvinder"): BLT gone mid-track - QLC+ looked to the end, no BLT and nothing connected.
                // The same box over the waveform, on a panel, with START BLT; gone when BLT runs again
                readonly property bool gone: !trackViewRoot.trackApp && trackViewRoot.beatCount > 0 && known && !running
                z: gone ? 20 : 0                 // over the waveform, the cursor and the corner hold
                // R465_BLT_PAGE (Tobias 10-15): BLT connects only when it has something to send -
                // running and quiet is the usual state before the first track (QLC+ looks for
                // the program every 5 s, and BLT says hello every 4 s)
                readonly property bool running: linked || (!!trackManager && trackManager.bltRunning === true)
                // R468_BLT_LOOK: "not running" only when QLC+ looked to the end - until then
                // what the page said before r465: not connected
                readonly property bool known: !!trackManager && trackManager.bltKnown === true
                // R468_START_BLT: 0 not asked, 1 starting, 2 it did not come up
                readonly property int bltStart: trackManager && trackManager.bltStartState !== undefined ? trackManager.bltStartState : 0
                width: emptyRow.implicitWidth
                height: emptyRow.implicitHeight
                // R518_BLT_GONE_PANEL: the box reads over a playing track
                Rectangle
                {
                    objectName: "bltGonePanel"
                    visible: emptyBox.gone
                    anchors.fill: parent
                    anchors.margins: -Math.round(22 * trackViewRoot.ks)
                    radius: Math.round(10 * trackViewRoot.ks)
                    color: trackViewRoot.cCard
                    opacity: 0.95                // R518_BLT_GONE_FLAT: no line around it (R403: no lines)
                }
                Row
                {
                    id: emptyRow
                    spacing: Math.round(18 * trackViewRoot.ks)
                    // R465_BLT_PAGE: Beat Link Trigger's own logo (dim while it does not run)
                    Image { objectName: "bltLogo"; anchors.verticalCenter: parent.verticalCenter; visible: !trackViewRoot.trackApp
                            width: Math.round(60 * trackViewRoot.ks); height: width
                            source: "blt_logo.png"; smooth: true; mipmap: true; fillMode: Image.PreserveAspectFit
                            opacity: emptyBox.running ? 1.0 : 0.45 }
                    Column
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Math.round(8 * trackViewRoot.ks)
                        Row
                        {
                            spacing: Math.round(12 * trackViewRoot.ks)
                            Text
                            {
                                objectName: "emptyTitle"
                                anchors.verticalCenter: parent.verticalCenter
                                text: trackViewRoot.trackApp ? (trackViewRoot.cdjCount > 0 ? qsTr("Waiting for track data")
                                                                : emptyBox.linked ? qsTr("Looking for the CDJs") : qsTr("Starting TRACK's CDJ reader"))
                                    : emptyBox.gone ? qsTr("Beat Link Trigger stopped")   // R518_BLT_GONE_TITLE
                                    : emptyBox.running ? qsTr("Waiting for track data")
                                    : emptyBox.known ? qsTr("Beat Link Trigger is not running") : qsTr("Beat Link Trigger is not connected")
                                color: trackViewRoot.cText
                                font.bold: true
                                font.pixelSize: trackViewRoot.fs(20)
                            }
                            Rectangle
                            {
                                id: bltInfo
                                objectName: "bltInfo"
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.round(26 * trackViewRoot.ks); height: width; radius: width / 2
                                color: trackViewRoot.bltGuideOpen ? trackViewRoot.cGold : "transparent"
                                border.width: 2
                                border.color: trackViewRoot.bltGuideOpen ? trackViewRoot.cGold : trackViewRoot.cMute
                                Text
                                {
                                    anchors.centerIn: parent
                                    text: "i"
                                    font.bold: true
                                    font.pixelSize: trackViewRoot.fs(15)
                                    color: trackViewRoot.bltGuideOpen ? "#1A1408" : trackViewRoot.cMute
                                }
                                TouchInput
                                {
                                    onReleasedAt: (x, y, inside) => { if (inside) trackViewRoot.bltGuideOpen = !trackViewRoot.bltGuideOpen }
                                }
                            }
                        }
                        Row
                        {
                            spacing: Math.round(8 * trackViewRoot.ks)
                            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: Math.round(10 * trackViewRoot.ks); height: width; radius: width / 2
                                        color: (trackViewRoot.trackApp ? trackViewRoot.cdjCount > 0 : emptyBox.running) ? trackViewRoot.cGreen : trackViewRoot.cGold }
                            Text
                            {
                                objectName: "emptySub"
                                anchors.verticalCenter: parent.verticalCenter
                                text: trackViewRoot.trackApp ? (trackViewRoot.cdjCount > 0
                                        ? (trackViewRoot.cdjCount === 1 ? qsTr("1 player found - play a track on a CDJ")
                                                                        : qsTr("%1 players found - play a track on a CDJ").arg(trackViewRoot.cdjCount))
                                        : emptyBox.linked ? qsTr("TRACK reads the CDJs by itself - is the network cable from the DJ booth in?")
                                                          : qsTr("It takes a few seconds"))
                                    : emptyBox.linked ? qsTr("Beat Link Trigger is connected - play a track on a CDJ")
                                    : emptyBox.running ? qsTr("Beat Link Trigger is running - play a track on a CDJ")
                                    : emptyBox.bltStart === 1 ? qsTr("Starting Beat Link Trigger - it takes about half a minute")
                                    : emptyBox.bltStart === 2 ? qsTr("It did not start - use its logo on the taskbar, then play a track on a CDJ")
                                    : emptyBox.gone ? qsTr("It stopped during the track - start it again here")   // R518_BLT_GONE_SUB
                                    : emptyBox.known ? qsTr("Start it here or from its logo on the taskbar - then play a track on a CDJ")
                                                       : qsTr("Start it from its logo on the taskbar - then play a track on a CDJ")   // R472_BLT_SUB
                                color: trackViewRoot.cMute
                                font.pixelSize: trackViewRoot.fs(13)
                            }
                        }
                        // R468_START_BLT: BLT from the taskbar's own shortcut, without leaving the page
                        Btn
                        {
                            objectName: "startBlt"
                            visible: !trackViewRoot.trackApp && !emptyBox.running && emptyBox.known      // R468_BLT_LOOK: never a second BLT
                            width: Math.round(200 * trackViewRoot.ks); height: Math.round(48 * trackViewRoot.ks)
                            text: emptyBox.bltStart === 1 ? qsTr("STARTING ...") : qsTr("START BLT")
                            tone: trackViewRoot.cGold
                            active: emptyBox.bltStart !== 1
                            onTapped: if (trackManager && emptyBox.bltStart !== 1) trackManager.startBlt()
                        }
                    }
                }
            }

            // R451_TEST_HOLD (Tobias 10-14: "meget mindre og rykkes helt ned i venstre
            // hjoerne ... saa det ikke er hvemsomhelst der kan trykke paa den" - mockup
            // approved): small and dim in the corner, held 1.5 s to start (it fills
            // with gold); a short tap only shows how. R450's test track behind it.
            Rectangle
            {
                id: testHold
                objectName: "testTrack"
                z: 10                            // over the empty rail at the card's foot
                visible: trackViewRoot.beatCount === 0 && !!trackManager
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.leftMargin: Math.round(12 * trackViewRoot.ks)
                anchors.bottomMargin: Math.round(12 * trackViewRoot.ks)
                width: Math.round(88 * trackViewRoot.ks)
                height: Math.round(30 * trackViewRoot.ks)
                radius: Math.round(5 * trackViewRoot.ks)
                color: trackViewRoot.cBtn
                property real progress: 0
                property bool hint: false
                Rectangle
                {
                    objectName: "testTrackFill"
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: parent.width * testHold.progress
                    radius: parent.radius
                    color: trackViewRoot.cGold
                }
                Row
                {
                    anchors.centerIn: parent
                    spacing: Math.round(6 * trackViewRoot.ks)
                    readonly property color ink: testHold.progress > 0.5 ? "#1A1408" : trackViewRoot.cDim
                    ControlIcon { anchors.verticalCenter: parent.verticalCenter; width: Math.round(12 * trackViewRoot.ks); height: width; kind: "play"; ink: parent.ink }
                    Text
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        text: testHoldIn.down ? qsTr("HOLD") : qsTr("TEST")
                        color: parent.ink
                        font.bold: true
                        font.pixelSize: trackViewRoot.fs(12)
                        font.letterSpacing: trackViewRoot.fs(12) * 0.08
                    }
                }
                NumberAnimation { id: testFill; target: testHold; property: "progress"; from: 0; to: 1; duration: 1500 }
                Timer { id: testHintOff; interval: 3000; onTriggered: testHold.hint = false }
                TouchInput
                {
                    id: testHoldIn
                    holdEnabled: true
                    holdMs: 1500
                    onPressedAt: { testHold.hint = true; testHintOff.stop(); testFill.restart() }
                    onHeld: { testFill.stop(); testHold.progress = 0; testHold.hint = false; if (trackManager) trackManager.startTestTrack() }
                    onReleasedAt: (x, y, inside) => { testFill.stop(); testHold.progress = 0; testHintOff.restart() }
                    onCanceled: { testFill.stop(); testHold.progress = 0; testHintOff.restart() }
                }
            }
            Rectangle
            {
                objectName: "testTrackHint"
                z: 10
                visible: testHold.visible && testHold.hint
                anchors.left: testHold.left
                anchors.bottom: testHold.top
                anchors.bottomMargin: Math.round(6 * trackViewRoot.ks)
                width: testHintText.implicitWidth + Math.round(18 * trackViewRoot.ks)
                height: Math.round(26 * trackViewRoot.ks)
                radius: Math.round(4 * trackViewRoot.ks)
                color: "#2A2418"
                Text
                {
                    id: testHintText
                    anchors.centerIn: parent
                    text: qsTr("hold 1.5 s to play a test track - nothing is learned, a real track takes over")
                    color: trackViewRoot.cGold
                    font.pixelSize: trackViewRoot.fs(12)
                }
            }

            // the countdown to the next section, in bars - the last 32 bars
            Rectangle
            {
                id: countdown
                property var nm: trackViewRoot.nextMarker()
                property int bars: nm ? Math.ceil((nm.beat - trackViewRoot.currentBeat) / 4) : 0
                visible: nm !== null && trackManager && trackManager.playing && bars <= 32 && trackViewRoot.currentBeat > 0
                anchors.right: parent.right
                anchors.rightMargin: Math.round(16 * trackViewRoot.ks)
                y: wfCanvas.stripH + (trackViewRoot.planShown ? trackViewRoot.laneH : 0) + Math.round(10 * trackViewRoot.ks)   // R443_PLAN_LANE
                width: cdCol.width + Math.round(28 * trackViewRoot.ks)
                height: cdCol.height + Math.round(16 * trackViewRoot.ks)
                radius: Math.round(10 * trackViewRoot.ks)
                color: Qt.rgba(0.03, 0.03, 0.04, 0.78)
                Column
                {
                    id: cdCol
                    anchors.centerIn: parent
                    Text
                    {
                        anchors.right: parent.right
                        text: countdown.nm ? countdown.nm.type.toUpperCase() + " " + qsTr("IN") : ""
                        color: Qt.lighter(trackViewRoot.markerColor(countdown.nm ? countdown.nm.type : ""), 1.25)
                        font.bold: true
                        font.pixelSize: trackViewRoot.fs(11)
                        font.letterSpacing: (trackViewRoot.fs(11)) * 0.2
                    }
                    Row
                    {
                        anchors.right: parent.right
                        spacing: 6
                        Text
                        {
                            id: cdBars
                            text: countdown.bars
                            color: trackViewRoot.markerColor(countdown.nm ? countdown.nm.type : "")
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(44)
                        }
                        Text
                        {
                            y: cdBars.baselineOffset - baselineOffset
                            text: countdown.bars === 1 ? qsTr("BAR") : qsTr("BARS")
                            color: Qt.lighter(trackViewRoot.markerColor(countdown.nm ? countdown.nm.type : ""), 1.25)
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(14)
                            font.letterSpacing: (trackViewRoot.fs(14)) * 0.12
                        }
                    }
                }
            }

            // R502_M_DROP_LINE: a + TYPE chip held over the track - a dashed line in its colour
            // on the bar it would land on, the bar and its distance to the playhead above it;
            // after it landed, "DROP SET · BAR 129" for 1.5 s on the same spot
            Item
            {
                id: flagDropLayer
                objectName: "flagDropLayer"
                z: 4
                x: wfCanvas.x
                y: wfCanvas.y
                width: wfCanvas.width
                height: wfCanvas.height
                readonly property int vf: trackViewRoot.viewFirst()
                readonly property int vc: Math.max(1, trackViewRoot.viewCount())
                function xOf(b) { return (b - vf) * width / vc }
                readonly property color tone: trackViewRoot.markerColor(trackViewRoot.flagDragType)
                readonly property bool lightTone: 0.299 * tone.r + 0.587 * tone.g + 0.114 * tone.b > 0.6
                readonly property real topY: wfCanvas.stripH + Math.round(10 * trackViewRoot.ks)
                Connections
                {
                    // SETUP or HELP over the page, the pencil put away, a new track: the chip goes back, nothing lands
                    target: trackViewRoot
                    function onSetupOpenChanged() { if (trackViewRoot.setupOpen) trackViewRoot.flagDragEnd(trackViewRoot.flagDragType, false) }
                    function onHelpOpenChanged() { if (trackViewRoot.helpOpen) trackViewRoot.flagDragEnd(trackViewRoot.flagDragType, false) }
                    function onMarkerEditChanged() { if (!trackViewRoot.markerEdit) trackViewRoot.flagDragEnd(trackViewRoot.flagDragType, false) }
                }
                Connections
                {
                    target: trackManager
                    function onTrackChanged() { trackViewRoot.flagDragEnd(trackViewRoot.flagDragType, false) }
                }
                Item
                {
                    id: flagDropLine
                    objectName: "flagDropLine"
                    readonly property real lw: Math.max(2, Math.round(3 * trackViewRoot.ks))
                    readonly property real dash: Math.max(8, Math.round(14 * trackViewRoot.ks))
                    readonly property real gap: Math.max(5, Math.round(8 * trackViewRoot.ks))
                    visible: trackViewRoot.flagDragType !== "" && trackViewRoot.flagDragBeat > 0
                    x: flagDropLayer.xOf(Math.max(1, trackViewRoot.flagDragBeat)) - lw / 2
                    y: wfCanvas.stripH
                    width: lw
                    height: flagDropLayer.height - y
                    clip: true
                    Repeater
                    {
                        // R511_UI_DASH_MODEL: never below 0 - before the track card has its height the line's
                        // height is negative ("Model size of -4 is less than 0" in the log)
                        model: Math.max(0, Math.ceil(flagDropLine.height / (flagDropLine.dash + flagDropLine.gap)))
                        Rectangle
                        {
                            y: index * (flagDropLine.dash + flagDropLine.gap)
                            width: flagDropLine.lw
                            height: flagDropLine.dash
                            color: flagDropLayer.tone
                        }
                    }
                }
                Rectangle
                {
                    id: flagDropBubble
                    objectName: "flagDropBubble"
                    visible: flagDropLine.visible
                    readonly property string text: trackViewRoot.flagDragText
                    width: flagDropText.implicitWidth + Math.round(36 * trackViewRoot.ks)
                    height: Math.max(30, Math.round(42 * trackViewRoot.ks))
                    x: Math.max(0, Math.min(flagDropLine.x + flagDropLine.lw / 2 - width / 2, flagDropLayer.width - width))
                    y: flagDropLayer.topY
                    radius: Math.round(8 * trackViewRoot.ks)
                    color: flagDropLayer.tone
                    Text
                    {
                        id: flagDropText
                        anchors.centerIn: parent
                        text: flagDropBubble.text
                        color: flagDropLayer.lightTone ? "#101010" : "#FFFFFF"
                        font.bold: true
                        font.pixelSize: trackViewRoot.fs(18)
                        font.letterSpacing: trackViewRoot.fs(18) * 0.06
                    }
                }
                Rectangle
                {
                    id: flagSetNote
                    objectName: "flagSetNote"
                    visible: trackViewRoot.flagSetText !== "" && trackViewRoot.flagDragType === ""
                    width: flagSetLabel.implicitWidth + Math.round(36 * trackViewRoot.ks)
                    height: Math.max(30, Math.round(42 * trackViewRoot.ks))
                    x: Math.max(0, Math.min(flagDropLayer.xOf(Math.max(1, trackViewRoot.flagSetBeat)) - width / 2, flagDropLayer.width - width))
                    y: flagDropLayer.topY
                    radius: Math.round(8 * trackViewRoot.ks)
                    color: "#26262D"
                    Text
                    {
                        id: flagSetLabel
                        anchors.centerIn: parent
                        text: trackViewRoot.flagSetText
                        color: trackViewRoot.cText
                        font.bold: true
                        font.pixelSize: trackViewRoot.fs(18)
                        font.letterSpacing: trackViewRoot.fs(18) * 0.06
                    }
                }
            }

            // ---- the rail: the pencil, a warning, the verdict
            Rectangle
            {
                id: rail
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 1
                height: trackViewRoot.railH
                color: "#111115"
                radius: waveCard.radius
                // a press that misses a tile must not reach anything underneath
                MouseArea { anchors.fill: parent }

                readonly property real bh: Math.max(48, Math.round(52 * trackViewRoot.ks))
                readonly property bool tools: trackManager && trackManager.beatCount > 0 && trackManager.roleMode

                Row
                {
                    id: railLeft
                    anchors.left: parent.left
                    anchors.leftMargin: Math.round(10 * trackViewRoot.ks)
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Math.round(8 * trackViewRoot.ks)
                    visible: rail.tools && verdictTools.blaming === 0

                    Btn
                    {
                        objectName: "markerEdit"
                        width: Math.round((trackViewRoot.markerEdit ? 150 : 184) * trackViewRoot.ks)
                        height: rail.bh
                        icon: trackViewRoot.markerEdit ? "check" : "pencil"
                        text: trackViewRoot.markerEdit ? qsTr("DONE") : qsTr("MARKERS")
                        active: trackViewRoot.markerEdit
                        tone: trackViewRoot.cGold
                        onTapped: trackViewRoot.markerEdit = !trackViewRoot.markerEdit
                    }

                    // R443_PLAN_LANE: the plan on the track, on / off - no legend beside
                    // it (Tobias: "dobbeltkonfekt da RIGGEN viser det")
                    Btn
                    {
                        objectName: "planToggle"
                        visible: !trackViewRoot.markerEdit && !!trackEngine
                        width: Math.round(172 * trackViewRoot.ks)
                        height: rail.bh
                        icon: "plan"
                        active: trackEngine ? trackEngine.planLane === true : false
                        text: active ? qsTr("PLAN  ON") : qsTr("PLAN  OFF")
                        tone: trackViewRoot.cGold
                        onTapped: if (trackEngine) trackEngine.planLane = !active
                    }

                    // R452_HIT_LIVE: DROP HIT moved to LIVE, beside FLASH WHITE (Tobias 10-14)

                    // ---- the flag tools: a flag on the bar the track is at, the
                    //      selected flag retyped or deleted. What the operator sets
                    //      is the truth - it goes to BLT's cache as manual.
                    Row
                    {
                        id: flagTools
                        visible: trackViewRoot.markerEdit
                        spacing: Math.round(8 * trackViewRoot.ks)
                        // R402_SLIDE_IN: up from the rail's foot as the pencil opens them
                        transform: Translate { id: flagShift }
                        onVisibleChanged: if (visible && trackViewRoot.animate) flagIn.restart()
                        ParallelAnimation
                        {
                            id: flagIn
                            NumberAnimation { target: flagShift; property: "y"; from: rail.bh * 0.7; to: 0; duration: 220; easing.type: Easing.OutCubic }
                            OpacityAnimator { target: flagTools; from: 0; to: 1; duration: 180 }
                        }
                        property real tw: Math.max(56, (rail.width - verdictTools.width - Math.round(210 * trackViewRoot.ks)
                                                        - 9 * spacing - Math.round(30 * trackViewRoot.ks)) / 10)
                        Repeater
                        {
                            // every type the engine understands (NORMAL, DRIVE, rekordbox' INTRO/OUTRO)
                            model: [ "normal", "drive", "break", "build", "drop", "intro", "outro" ]
                            Rectangle
                            {
                                id: addChip
                                objectName: "addFlag:" + modelData
                                width: flagTools.tw
                                height: rail.bh
                                radius: Math.round(10 * trackViewRoot.ks)
                                // R502_M_CHIP_LOOK: while it is dragged the chip stays behind as an empty frame
                                readonly property bool lifted: trackViewRoot.flagDragType === modelData
                                color: lifted ? "transparent" : (addIn.down ? "#2C2C33" : trackViewRoot.cBtn)
                                border.width: lifted ? 2 : 0
                                border.color: trackViewRoot.cDim
                                // R403_NO_LINES
                                scale: addIn.down && !lifted ? 0.965 : 1.0      // R402_PRESS
                                Behavior on scale { enabled: trackViewRoot.animate; NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
                                Row
                                {
                                    opacity: addChip.lifted ? 0.45 : 1.0
                                    anchors.centerIn: parent
                                    spacing: 7
                                    Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 10; height: 10; radius: 5; color: trackViewRoot.markerColor(modelData) }
                                    Text
                                    {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "+ " + modelData.toUpperCase()
                                        color: "#D6D6DC"
                                        font.bold: true
                                        font.pixelSize: trackViewRoot.fs(13)
                                        font.letterSpacing: (trackViewRoot.fs(13)) * 0.06
                                    }
                                }
                                TouchInput
                                {
                                    id: addIn
                                    // R502_M_CHIP_DRAG: a tap - the finger stays within the tremble a
                                    // moved tab allows - sets the flag on the bar that is playing, as
                                    // always. Past it the chip is lifted and follows the finger; let go
                                    // over the track = the bar under it, anywhere else = nothing. The
                                    // finger stays this chip's (mouse grab / its own touch point), so it
                                    // fires nothing it crosses and a second finger elsewhere still works.
                                    property real px0: 0
                                    property real py0: 0
                                    property bool lifting: false
                                    // R503_UI_TAPONLY_FLAG: pulled while another chip was out = a tap for the whole
                                    // touch - it does not lift later, far off its chip, once that one landed
                                    property bool tapOnly: false
                                    onPressedAt: (x, y) => { px0 = x; py0 = y; lifting = false; tapOnly = false }
                                    onMovedTo: (x, y) =>
                                    {
                                        if (!lifting)
                                        {
                                            if (Math.abs(x - px0) < Math.max(10, Math.round(12 * trackViewRoot.ks))
                                                && Math.abs(y - py0) < Math.max(10, Math.round(12 * trackViewRoot.ks))) return
                                            if (tapOnly) return             // R503_UI_TAPONLY_GUARD
                                            if (trackViewRoot.flagDragType !== "") { tapOnly = true; return }   // another chip is out: this one taps
                                            lifting = true
                                            trackViewRoot.flagDragType = modelData
                                        }
                                        trackViewRoot.flagDragMove(modelData, addIn, x, y)
                                    }
                                    onReleasedAt: (x, y, inside) =>
                                    {
                                        if (lifting)
                                        {
                                            lifting = false
                                            trackViewRoot.flagDragMove(modelData, addIn, x, y)
                                            trackViewRoot.flagDragEnd(modelData, true)
                                            return
                                        }
                                        if (inside) trackManager.addMarker(trackManager.currentBeat > 0 ? trackManager.currentBeat : 1, modelData)
                                    }
                                    onCanceled: { if (lifting) { lifting = false; trackViewRoot.flagDragEnd(modelData, false) } }
                                }
                            }
                        }
                        Item { width: Math.round(14 * trackViewRoot.ks); height: 1 }
                        Btn
                        {
                            objectName: "retypeFlag"
                            width: flagTools.tw; height: rail.bh
                            fontPx: trackViewRoot.fs(13)
                            text: qsTr("RETYPE")
                            // greyed rather than hidden: hiding re-flowed the row (r204)
                            enabled: wfCanvas.selected >= 0
                            onTapped:
                            {
                                var mk = trackManager.markers[wfCanvas.selected]
                                if (mk === undefined) { wfCanvas.selected = -1; return }
                                var order = [ "normal", "drive", "break", "build", "drop", "intro", "outro" ]
                                var next = order[(order.indexOf(mk.type) + 1) % order.length]
                                trackManager.setMarkerType(wfCanvas.selected, next)
                            }
                        }
                        Btn
                        {
                            objectName: "deleteFlag"
                            width: flagTools.tw; height: rail.bh
                            fontPx: trackViewRoot.fs(13)
                            text: qsTr("DELETE")
                            enabled: wfCanvas.selected >= 0
                            active: wfCanvas.selected >= 0
                            tone: "#E36B6B"
                            onTapped: { var i = wfCanvas.selected; wfCanvas.selected = -1; trackManager.removeMarker(i) }
                        }
                        // one step back - the flag and the lesson it taught
                        Btn
                        {
                            objectName: "undoFlag"
                            width: flagTools.tw; height: rail.bh
                            fontPx: trackViewRoot.fs(13)
                            text: qsTr("UNDO")
                            enabled: trackManager ? trackManager.canUndoMarkers : false
                            onTapped: { wfCanvas.selected = -1; trackManager.undoMarkers() }
                        }
                    }
                }

                // a warning that stands all night sits here, where it covers nothing
                Row
                {
                    // R445_DROP_HIT: right of MARKERS / PLAN / DROP HIT (it lay over them,
                    // centred), centred only when the rail has no buttons
                    readonly property real leftEdge: railLeft.visible ? railLeft.x + railLeft.width + Math.round(16 * trackViewRoot.ks) : 0
                    readonly property real room: railLeft.visible ? verdictTools.x - Math.round(16 * trackViewRoot.ks) - leftEdge
                                                                  : rail.width - Math.round(760 * trackViewRoot.ks)
                    x: railLeft.visible ? leftEdge : (rail.width - width) / 2
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    visible: trackManager && trackManager.roleMode && !trackViewRoot.setupOpen
                             && !trackViewRoot.markerEdit && verdictTools.blaming === 0 && warnText.text.length > 0
                    width: Math.max(0, Math.min(implicitWidth, room))
                    ControlIcon { anchors.verticalCenter: parent.verticalCenter; width: 18; height: 18; kind: trackEngine && trackEngine.lastTrack > 0 ? "moon" : "warn"; ink: "#E3B44F" }   // R458_LAST_BTN
                    Text
                    {
                        id: warnText
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.max(0, Math.min(implicitWidth, parent.room - 28))
                        elide: Text.ElideRight
                        color: "#E3B44F"
                        font.pixelSize: trackViewRoot.fs(13)
                        text:
                        {
                            var parts = []
                            var lt = trackEngine ? trackEngine.lastTrack : 0     // R458_LAST_BTN
                            if (lt === 1 || lt === 2)
                                parts.push(qsTr("LAST TRACK: warm white grows over the track (lasers out, strobes steady at the outro), full at its end or when the fader goes down, 5 min, then dark - tap LAST TRACK again to undo"))   // R479_LAST_WHITE_HINT
                            else if (lt >= 3)
                                parts.push(qsTr("LAST TRACK: the white light goes dark by itself after 5 min - SHOW OFF ends it now"))
                            if (trackManager && trackManager.linkStale)
                                parts.push(trackViewRoot.trackApp ? qsTr("CDJ link stale - holding the last look")
                                                                  : qsTr("BLT link stale - holding the last look"))
                            if (trackEngine)
                                for (var i = 0; i < trackEngine.warnings.length; i++)
                                    parts.push(trackEngine.warnings[i])
                            return parts.join("   ·   ")
                        }
                    }
                }

                // ---- the verdict: two thumbs in a corner that never moves. A
                //      long press aims the thumb at one group on stage.
                Item
                {
                    id: verdictTools
                    anchors.right: parent.right
                    anchors.rightMargin: Math.round(10 * trackViewRoot.ks)
                    anchors.verticalCenter: parent.verticalCenter
                    visible: trackManager && trackEngine && trackManager.beatCount > 0 && !trackViewRoot.setupOpen && !trackViewRoot.helpOpen
                    property int blaming: 0
                    property var stageRows: []
                    width: blaming === 0 ? thumbRow.width : blameRow.width
                    height: rail.bh

                    Row
                    {
                        id: thumbRow
                        spacing: Math.round(8 * trackViewRoot.ks)
                        visible: verdictTools.blaming === 0
                        Btn
                        {
                            id: recentSavedButton
                            objectName: "recentSavedMenu"
                            width: Math.round(110 * trackViewRoot.ks); height: rail.bh
                            text: "SAVED"; icon: "savedLooks"; fontPx: trackViewRoot.fs(12)
                            active: trackViewRoot.savedRecentOpen; tone: trackViewRoot.cBlue
                            onTapped: trackViewRoot.savedRecentOpen = !trackViewRoot.savedRecentOpen
                        }
                        Repeater
                        {
                            model: [ 1, -1 ]
                            Rectangle
                            {
                                id: thumb
                                objectName: "rating:" + modelData
                                width: Math.round(72 * trackViewRoot.ks)
                                height: rail.bh
                                radius: Math.round(10 * trackViewRoot.ks)
                                property bool lit: false
                                color: lit ? (modelData > 0 ? "#3E7E4E" : "#8E3A3A") : (modelData > 0 ? "#18241B" : "#271818")
                                Text
                                {
                                    anchors.centerIn: parent
                                    visible: modelData > 0 && trackViewRoot.savedSaveFeedback
                                    text: qsTr("SAVED")
                                    font.pixelSize: trackViewRoot.fs(12); font.bold: true
                                    color: "#B4E6C0"
                                }
                                Canvas
                                {
                                    anchors.centerIn: parent
                                    visible: !(modelData > 0 && trackViewRoot.savedSaveFeedback)
                                    width: 26; height: 26
                                    rotation: modelData > 0 ? 0 : 180
                                    onPaint:
                                    {
                                        var c = getContext("2d"); c.reset()
                                        c.fillStyle = modelData > 0 ? "#9FD8AF" : "#E8A0A0"
                                        c.fillRect(3, 12, 8, 12)
                                        c.beginPath(); c.moveTo(12, 24); c.lineTo(12, 13); c.lineTo(16, 3)
                                        c.quadraticCurveTo(19, 1, 19, 5); c.lineTo(17, 11); c.lineTo(23, 11)
                                        c.quadraticCurveTo(25, 11, 24, 14); c.lineTo(22, 22)
                                        c.quadraticCurveTo(21, 24, 19, 24); c.closePath(); c.fill()
                                    }
                                }
                                TouchInput
                                {
                                    property bool wasHeld: false
                                    holdEnabled: true
                                    // the instant the finger lands is the moment judged
                                    onPressedAt: (x, y) => { wasHeld = false; if (trackEngine) trackEngine.markVerdictPoint() }
                                    onHeld:
                                    {
                                        if (trackEngine === null) return
                                        var rows = trackEngine.onStage()
                                        if (rows.length === 0) return       // nothing to aim at: a plain vote on release
                                        wasHeld = true
                                        verdictTools.stageRows = rows
                                        verdictTools.blaming = modelData
                                        blameTimeout.restart()
                                    }
                                    onReleasedAt: (x, y, inside) =>
                                    {
                                        if (wasHeld) { wasHeld = false; return }
                                        if (!inside) return
                                        // R511_UI_SAVED_STALE: savedFeedback keeps the LAST save's "SAVED" - a thumb up
                                        // that saved nothing (rate() returns early between tracks) said SAVED again.
                                        // SAVED only when the newest saved look really is newer after this tap
                                        var newestAt = function() { var r = trackEngine ? trackEngine.savedLooks : []; return r.length > 0 ? (Number(r[0].savedAt) || 0) : 0 }
                                        var savedBefore = modelData > 0 ? newestAt() : 0
                                        if (trackEngine) trackEngine.rate(modelData)
                                        if (modelData > 0) trackViewRoot.savedSaveFeedback = !!trackEngine && trackEngine.savedFeedback === "SAVED" && newestAt() > savedBefore
                                        thumb.lit = true
                                        thumbFlash.restart()
                                    }
                                }
                                Timer { id: thumbFlash; interval: modelData > 0 ? 1500 : 220; onTriggered: { thumb.lit = false; if (modelData > 0) trackViewRoot.savedSaveFeedback = false } }
                            }
                        }
                    }

                    // the list a long press opens: one tile per group on stage
                    Row
                    {
                        id: blameRow
                        anchors.right: parent.right
                        spacing: Math.round(6 * trackViewRoot.ks)
                        visible: verdictTools.blaming !== 0
                        property int cells: Math.max(1, verdictTools.stageRows.length)
                        property real cellW: Math.max(64, Math.min(150, (rail.width - 90 - cells * spacing) / cells))
                        Repeater
                        {
                            model: verdictTools.stageRows
                            Btn
                            {
                                width: blameRow.cellW
                                height: rail.bh
                                fontPx: trackViewRoot.fs(12)
                                text: modelData ? modelData.group : ""
                                active: true
                                tone: verdictTools.blaming > 0 ? "#4FA36B" : "#B05050"
                                onTapped:
                                {
                                    if (trackEngine) trackEngine.rateGroup(verdictTools.blaming, modelData.group)
                                    verdictTools.blaming = 0
                                    blameTimeout.stop()
                                }
                            }
                        }
                        Btn
                        {
                            width: rail.bh; height: rail.bh
                            text: "×"
                            fontPx: trackViewRoot.fs(18)
                            onTapped: { verdictTools.blaming = 0; blameTimeout.stop() }
                        }
                    }
                    // a list left open in the dark is a trap for the next finger
                    Timer { id: blameTimeout; interval: 6000; onTriggered: verdictTools.blaming = 0 }
                }
            }
        }

        // ============ 3 · THE DECK: rig · look · live - six rows, one grid ============
        Item
        {
            id: deck
            Layout.fillWidth: true
            Layout.preferredHeight: trackViewRoot.deckH
            visible: trackManager && trackEngine && trackManager.roleMode
            readonly property real rowsY: trackViewRoot.cardPad + trackViewRoot.headH
            function rowY(i) { return rowsY + i * (trackViewRoot.rowH + trackViewRoot.g) }

            // ---- RIG: what is in play - set at the start of the night ----
            Card
            {
                id: rigCard
                title: "RIG"
                x: 0; y: 0
                width: trackViewRoot.rigW
                height: parent.height
                // SETUP covers this card: nothing under it may take a finger
                enabled: !trackViewRoot.setupOpen && !trackViewRoot.helpOpen
                opacity: trackViewRoot.setupOpen ? 0 : 1

                readonly property real inX: trackViewRoot.cardPad + 2
                readonly property real inW: width - 2 * inX
                // once here, not per row: groups() rebuilds the table, trims()
                // and cast() are not cheap either (runde 142, 204)
                property var allTrims: trackEngine ? trackEngine.trims : ({})
                property var litNow: trackEngine ? trackEngine.cast : []
                // R432_RIG_NEXT: the section the marks are for, and what a tap does
                readonly property string planName: trackEngine && trackEngine.planSection ? trackEngine.planSection.toUpperCase() : ""
                // R469_RIG_COLOUR: on stage = the fader in the room's colour (no "on stage" words)
                readonly property string roomColour: trackEngine ? trackEngine.currentColour : ""
                readonly property color stageFill: roomColour !== "" ? trackViewRoot.swatch(roomColour) : "#3D86C4"
                readonly property bool stageLight: (0.299 * stageFill.r + 0.587 * stageFill.g + 0.114 * stageFill.b) > 0.6
                readonly property color stageInk: stageLight ? "#14141A" : "#F2F6FA"
                readonly property color stageGrip: stageLight ? "#14141A" : Qt.lighter(stageFill, 1.6)
                Text
                {
                    objectName: "rigPlanHint"
                    visible: trackEngine ? trackEngine.dropMode > 0 && rigCard.planName !== "" : false
                    x: rigCard.inX + Math.round(48 * trackViewRoot.ks)
                    y: trackViewRoot.cardPad
                    width: rigCard.inW - Math.round(48 * trackViewRoot.ks)
                    elide: Text.ElideRight
                    // R471_PICK_FAILSAFE: every group tapped out - the engine chooses
                    text: trackEngine && trackEngine.dropSet.length === 0
                          ? "·  " + qsTr("none picked - the engine chooses the NEXT") + " " + rigCard.planName
                          : "·  " + qsTr("tap a name = in / out of the NEXT") + " " + rigCard.planName
                    color: trackViewRoot.trackApp ? trackViewRoot.cMute : "#F05050"   // TRACK_APP_UI_R560: grey
                    font.pixelSize: trackViewRoot.fs(12)
                }
                // MASTER DIMMER has the top row; five groups fill the other five,
                // a bigger rig shares them
                readonly property int rows: castRep.count
                readonly property real rowH: rows > 5 ? Math.max(40, (5 * trackViewRoot.rowH + 4 * trackViewRoot.g - (rows - 1) * trackViewRoot.g) / rows)
                                                      : trackViewRoot.rowH

                // runde 389 (Tobias 10-07: "rykke Master-dimmeren oeverst i RIG i hele
                // laengden"): the ceiling of the whole rig, over the groups it scales
                Fader
                {
                    objectName: "master"
                    inputName: "masterDrag"
                    x: rigCard.inX; y: deck.rowY(0); width: rigCard.inW; height: trackViewRoot.rowH
                    name: qsTr("MASTER DIMMER")
                    nameInk: "#06182B"
                    level: trackEngine ? trackEngine.master : 1
                    fill: "#4FA3E3"
                    gripInk: "#BFE2FF"
                    valueOnFill: "#06182B"
                    // a tap sets MASTER where the finger lands, a drag follows it (r254)
                    onSetLevel: (v) => { if (trackEngine) trackEngine.master = v }
                }
                Rectangle
                {
                    x: rigCard.inX; width: rigCard.inW; height: 1
                    y: deck.rowY(1) - Math.round(trackViewRoot.g / 2) - 1
                    color: trackViewRoot.cEdge
                }

                // R472_RIG_ROWS: the rows by count - a group switched on or off (the list changes)
                // does not rebuild them, so a held pad and a drag in progress live on
                readonly property var groupRows: trackEngine ? trackEngine.groups : []
                Repeater
                {
                    id: castRep
                    model: rigCard.groupRows.length

                    Item
                    {
                        id: grp
                        // the same fallback as before: a groups rebuild re-evaluates
                        // every binding with modelData gone
                        property var md: rigCard.groupRows[index] ? rigCard.groupRows[index] : ({ key: "", enabled: true, switchOnly: false, base: false })
                        property bool lit: rigCard.litNow.indexOf(md.key) >= 0
                        property bool off: !md.enabled
                        property bool switchOnly: md.switchOnly === true
                        property real trim: rigCard.allTrims[md.key] !== undefined ? rigCard.allTrims[md.key] : 1.0
                        // R431_DROP_PICK: in the next drop (or this one)
                        property bool inDrop: trackEngine ? (!off && trackEngine.dropMode > 0 && trackEngine.dropSet.indexOf(md.key) >= 0) : false   // R431B_DROP_OFF
                        x: rigCard.inX
                        y: deck.rowY(1) + index * (rigCard.rowH + trackViewRoot.g)
                        width: rigCard.inW
                        height: rigCard.rowH
                        readonly property real faderW: Math.round(228 * trackViewRoot.kw)
                        readonly property real swW: Math.round(58 * trackViewRoot.ks)
                        readonly property color ink: off ? "#4E4E56" : (lit ? "#E6E6EC" : "#8C8C96")

                        // R431_RIG_NAMES: the drop's groups carry a red line
                        Rectangle
                        {
                            visible: grp.inDrop
                            x: -Math.round(10 * trackViewRoot.ks)
                            width: Math.round(4 * trackViewRoot.ks); height: parent.height
                            radius: width / 2
                            color: "#E03030"
                        }
                        // R431_RIG_NAMES (Tobias 10-11: B3): a tap on the name = in or out
                        // of the drop - only while a drop is ahead or on
                        Item
                        {
                            objectName: "groupName:" + grp.md.key
                            anchors.left: parent.left
                            anchors.right: grpSwitchCell.left
                            anchors.rightMargin: Math.round(30 * trackViewRoot.ks)
                            height: parent.height
                            TouchInput
                            {
                                enabled: trackEngine ? trackEngine.dropMode > 0 && !grp.off : false
                                onReleasedAt: (x, y, inside) => { if (inside && trackEngine) trackEngine.toggleDropGroup(grp.md.key) }
                            }
                        }
                        // R469_GROUP_FLASH (Tobias 10-16: "ikonet ... som en knap ... med et lille flash i siden"):
                        // the group's own icon is its flash pad - hold = that group full white
                        Rectangle
                        {
                            id: grpFlash
                            objectName: "groupFlash:" + grp.md.key
                            x: -Math.round(4 * trackViewRoot.ks)
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.round(54 * trackViewRoot.ks); height: Math.min(grp.height - 2, width)
                            radius: Math.round(10 * trackViewRoot.ks)
                            property bool held: trackEngine && trackEngine.flashGroups ? trackEngine.flashGroups.indexOf(grp.md.key) >= 0 : false
                            color: held ? "#F2F2F6" : "#24242B"
                            ControlIcon
                            {
                                anchors.right: parent.right; anchors.bottom: parent.bottom
                                anchors.rightMargin: Math.round(3 * trackViewRoot.ks); anchors.bottomMargin: Math.round(3 * trackViewRoot.ks)
                                width: Math.round(16 * trackViewRoot.ks); height: width
                                kind: "bolt"
                                ink: grpFlash.held ? "#101010" : "#E3B44F"
                            }
                            TouchInput
                            {
                                id: padIn
                                enabled: !grp.off
                                onPressedAt: (x, y) => { if (trackEngine) trackEngine.flashGroup(grp.md.key, true) }
                                onReleasedAt: (x, y, inside) => { if (trackEngine) trackEngine.flashGroup(grp.md.key, false) }
                                onCanceled: { if (trackEngine) trackEngine.flashGroup(grp.md.key, false) }
                                // the page gone with the finger still down: no release ever comes
                                Component.onDestruction: if (down && trackEngine) trackEngine.flashGroup(grp.md.key, false)
                            }
                            // "still held" every second - a hold nobody says that for lets go in 3 s
                            Timer
                            {
                                interval: 1000; repeat: true
                                running: padIn.down && grpFlash.held
                                onTriggered: if (trackEngine) trackEngine.flashGroup(grp.md.key, true)
                            }
                        }
                        ControlIcon
                        {
                            id: grpIcon
                            anchors.verticalCenter: parent.verticalCenter
                            x: grpFlash.x + (grpFlash.width - width) / 2      // in the middle of the pad
                            width: Math.round(28 * trackViewRoot.ks); height: width
                            ink: grpFlash.held ? "#101010" : grp.ink
                            kind: grp.md.switchOnly ? "animation" : grp.md.strobes ? "strobe" : grp.md.lasers ? "laser"
                                  : grp.md.key.toLowerCase().indexOf("eyes") >= 0 ? "eyes" : "head"
                        }
                        Column
                        {
                            anchors.left: grpFlash.right
                            anchors.leftMargin: Math.round(10 * trackViewRoot.ks)
                            anchors.right: grpSwitchCell.left
                            anchors.rightMargin: Math.round(4 * trackViewRoot.ks)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Text
                            {
                                width: parent.width
                                elide: Text.ElideRight
                                text: grp.md.key
                                color: grp.ink
                                font.bold: true
                                font.pixelSize: trackViewRoot.fs(15)
                            }
                            Text
                            {
                                id: grpStatus
                                width: parent.width
                                elide: Text.ElideRight
                                textFormat: Text.StyledText
                                font.pixelSize: trackViewRoot.fs(12)
                                // R402_BEAT_PULSE: a group on stage breathes with the beat
                                OpacityAnimator { id: stagePulse; target: grpStatus; from: 0.45; to: 1.0; duration: 260; easing.type: Easing.OutQuad }
                                Connections
                                {
                                    target: trackViewRoot
                                    function onCurrentBeatChanged() { if (grp.lit && trackViewRoot.animate && trackManager && trackManager.playing) stagePulse.restart() }
                                }
                                color: grp.off ? "#5E5E68" : (grp.lit ? trackViewRoot.cGreen : "#6E6E78")
                                // R432_RIG_NEXT (mockup C): the dot is NOW, the red words the
                                // plan for the next section
                                visible: text !== ""                      // R469_RIG_STATUS: the name alone, centred
                                text: (grp.md.base ? "<font color='#4FA3E3'><b>BASE</b></font>" + (grp.inDrop || grp.off ? " · " : "") : "")
                                      + (grp.inDrop ? "<font color='#F05050'><b>▶ " + (grp.md.base ? "" : qsTr("NEXT") + " ")
                                                      + rigCard.planName + "</b></font>"
                                         : (grp.off ? qsTr("off") : ""))
                            }
                        }

                        Item
                        {
                            id: grpSwitchCell
                            anchors.right: parent.right
                            anchors.rightMargin: grp.faderW + Math.round(10 * trackViewRoot.ks)
                            anchors.verticalCenter: parent.verticalCenter
                            width: grp.swW
                            height: parent.height

                            // R431_BASE_SWITCH (Tobias 10-11: "muligt at slukke for basen
                            // med toggle"): the base has the toggle the others have
                            // A TOGGLE (runde 139): in or out of tonight's show
                            Rectangle
                            {
                                id: grpSwitch
                                objectName: "groupSwitch:" + grp.md.key
                                anchors.centerIn: parent
                                width: Math.round(52 * trackViewRoot.ks)
                                height: Math.round(28 * trackViewRoot.ks)
                                radius: height / 2
                                color: grp.off ? "#2C2C33" : trackViewRoot.cGreen
                                Behavior on color { ColorAnimation { duration: 120 } }
                                Rectangle
                                {
                                    width: parent.height - 6; height: width; radius: width / 2
                                    y: 3
                                    x: grp.off ? 3 : parent.width - width - 3
                                    color: grp.off ? "#7E7E86" : "#123012"
                                    Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                                }
                                // a finger target bigger than the pill it draws: the
                                // whole height of the row (runde 390 - it was 44 px)
                                Item
                                {
                                    id: switchTarget
                                    // runde 392: wider - from the end of the name to the
                                    // fader's edge, the row's height and half the gap
                                    readonly property real spare: (grpSwitchCell.width - parent.width) / 2
                                    width: grpSwitchCell.width + Math.round(28 * trackViewRoot.ks) + Math.round(10 * trackViewRoot.ks)
                                    height: grp.height + trackViewRoot.g
                                    x: parent.width + spare + Math.round(10 * trackViewRoot.ks) - width
                                    y: (parent.height - height) / 2
                                    TouchInput { onReleasedAt: (x, y, inside) => { if (inside && trackEngine) trackEngine.setGroupEnabled(grp.md.key, grp.off) } }
                                }
                            }
                        }

                        // the trim - or, for a group whose dimmer is a switch, the
                        // switch itself: a fader with two positions is a lie
                        Fader
                        {
                            id: grpTrimCell
                            // a switch-only group: the names belong to its ON/OFF box
                            objectName: grp.switchOnly ? "" : "groupTrim:" + grp.md.key
                            inputName: grp.switchOnly ? "" : "groupTrim:" + grp.md.key + "Drag"
                            visible: !grp.switchOnly
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: grp.faderW
                            height: Math.max(trackViewRoot.touchH - 8, grp.height)
                            level: grp.off ? 0 : grp.trim
                            valueText: Math.round(grp.trim * 100) + "%"
                            fill: grp.lit ? rigCard.stageFill : (input.down ? "#45454E" : "#3A3A42")      // R469_RIG_COLOUR
                            gripInk: grp.lit ? rigCard.stageGrip : "#B0B0B8"
                            valueOnFill: grp.lit ? rigCard.stageInk : "#F2F6FA"
                            enabled: !grp.off
                            opacity: grp.off ? 0.55 : 1
                            onSetLevel: (v) =>
                            {
                                if (v > 0.97) v = 1
                                if (v < 0.03) v = 0
                                // whole percent (runde 221)
                                v = Math.round(v * 100) / 100
                                if (trackEngine) trackEngine.setGroupTrim(grp.md.key, v)
                            }
                        }
                        Rectangle
                        {
                            objectName: grp.switchOnly ? "groupTrim:" + grp.md.key : ""
                            visible: grp.switchOnly
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: grp.faderW
                            height: grp.height
                            radius: Math.round(10 * trackViewRoot.ks)
                            color: grp.off ? "#0A0A0D" : (grp.lit ? rigCard.stageFill : "#3A3A42")      // R403_NO_LINES   // R469_RIG_COLOUR
                            Text
                            {
                                anchors.centerIn: parent
                                text: grp.off ? qsTr("ON / OFF ONLY") : qsTr("ON")
                                color: grp.off ? "#5E5E68" : (grp.lit ? rigCard.stageInk : "#F2F6FA")
                                font.bold: true
                                font.pixelSize: trackViewRoot.fs(12)
                                font.letterSpacing: (trackViewRoot.fs(12)) * 0.12
                            }
                            // R431_BASE_SWITCH: the base too
                            TouchInput
                            {
                                enabled: grp.switchOnly
                                onReleasedAt: (x, y, inside) => { if (inside && trackEngine) trackEngine.setGroupEnabled(grp.md.key, grp.off) }
                            }
                        }
                    }
                }
            }

            // ---- LOOK: how hard on top, then how the room looks; the haze
            //      machine as a group of its own at the bottom (runde 389) ----
            Card
            {
                id: lookCard
                title: "LOOK"
                titleBoxW: trackViewRoot.trackApp ? labW : 0      // TRACK_APP_LABELS_R555: over the labels
                x: trackViewRoot.rigW + trackViewRoot.gapB
                y: 0
                width: parent.width - trackViewRoot.rigW - trackViewRoot.liveW - 2 * trackViewRoot.gapB
                height: parent.height
                enabled: !trackViewRoot.setupOpen && !trackViewRoot.helpOpen
                opacity: trackViewRoot.setupOpen ? 0 : 1

                readonly property real inX: trackViewRoot.cardPad + 2
                readonly property real inW: width - 2 * inX
                readonly property real labW: Math.round(100 * trackViewRoot.ks)
                readonly property real autoW: Math.round(120 * trackViewRoot.ks)
                readonly property real colGap: Math.round(10 * trackViewRoot.ks)
                readonly property real cX: inX + labW + colGap + autoW + colGap      // the content column
                readonly property real cW: width - inX - cX
                readonly property real autoX: inX + labW + colGap
                readonly property real cg: Math.round(8 * trackViewRoot.ks)          // between the tiles of a row
                function cell(n) { return (cW - (n - 1) * cg) / n }

                // ALL AUTO (runde 396, Tobias 10-07: "en lille full-auto knap der dukker
                // op et sted hvor det passer til hurtigt at toggle alle auto knapperne paa
                // hvis de ikke er det"): over the AUTO column, only while one is off
                Btn
                {
                    id: allAutoTile
                    objectName: "allAuto"
                    visible: !trackViewRoot.allAutoOn
                    x: lookCard.autoX
                    width: lookCard.autoW
                    height: Math.round(32 * trackViewRoot.ks)
                    y: Math.round((deck.rowsY - height) / 2) + Math.round(2 * trackViewRoot.ks)
                    text: qsTr("ALL AUTO"); icon: "revert"; autoKind: true
                    fontPx: trackViewRoot.fs(11)
                    active: true
                    onTapped: trackViewRoot.allAuto()
                    // a finger target the whole height of the header band, wider than the pill
                    Item
                    {
                        x: -Math.round(10 * trackViewRoot.ks); y: -parent.y
                        width: parent.width + 2 * Math.round(10 * trackViewRoot.ks)
                        height: deck.rowsY - Math.round(3 * trackViewRoot.ks)
                        TouchInput { onReleasedAt: (x, y, inside) => { if (inside) trackViewRoot.allAuto() } }
                    }
                }

                // row 0: ENERGY - by the hand. AUTO gives it back to the clock. On top
                // (runde 389, Tobias 10-07: "saa alle Auto-knapperne er over hinanden")
                RowLabel { x: lookCard.inX; y: deck.rowY(0); width: lookCard.labW; height: trackViewRoot.rowH; text: "ENERGY"; color: trackViewRoot.cGold }
                Btn
                {
                    objectName: "energyAuto"
                    x: lookCard.autoX; y: deck.rowY(0); width: lookCard.autoW; height: trackViewRoot.rowH
                    text: qsTr("AUTO"); icon: "autoColour"; autoKind: true
                    active: trackEngine ? trackEngine.roomAuto : false
                    onTapped: if (trackEngine) trackEngine.roomAuto = true
                }
                Fader
                {
                    id: energyFader
                    objectName: "energy"
                    inputName: "energyDrag"
                    property real trim: trackManager ? Math.min(1, trackManager.energyTrim / 100) : 0.5
                    x: lookCard.cX; y: deck.rowY(0); width: lookCard.cW; height: trackViewRoot.rowH
                    level: trim
                    valueText: (trackManager ? Math.round(Math.min(100, trackManager.energyTrim)) : 50) + "%"
                    fill: "#E3B44F"
                    gripInk: "#F6D98A"
                    valueInk: "#F2D58E"
                    valueOnFill: "#2A1D05"
                    // R404_ENERGY_EDGE: the one fader with an edge - ENERGY's gold
                    border.width: 1
                    border.color: Qt.rgba(0.89, 0.71, 0.31, 0.45)
                    // a hand on the bar takes over from the clock - also when it
                    // lands where the clock already put it
                    onPressed: if (trackEngine && trackEngine.roomAuto) trackEngine.roomAuto = false
                    onSetLevel: (v) => { if (trackManager) trackManager.energyTrim = Math.round(v * 100) }
                    // TRACK_APP_UI_R560: the clock's ENERGY - a thin line (AUTO 72 / 15 MIN 78)
                    Item
                    {
                        id: clockMark
                        objectName: "energyClockMark"
                        readonly property bool auto: trackEngine ? trackEngine.roomAuto === true : false
                        readonly property int target: auto ? trackViewRoot.clockSoon : trackViewRoot.clockNow
                        visible: trackViewRoot.trackApp && target >= 0
                                 && (!auto || Math.abs(target - Math.round(energyFader.level * 100)) >= 2)
                        x: Math.max(2, Math.min(parent.width - 4, parent.width * Math.min(100, target) / 100 - 1))
                        y: 0; width: 2; height: parent.height
                        Rectangle { anchors.fill: parent; color: "#FFFFFF"; opacity: 0.55 }
                        Text
                        {
                            objectName: "energyClockText"
                            visible: false               // TRACK_APP_CLOCK_R562: the line alone, no text
                            x: clockMark.x > parent.parent.width - 120 ? -implicitWidth - 6 : 6
                            y: Math.round(4 * trackViewRoot.ks)
                            text: (clockMark.auto ? qsTr("15 MIN") : qsTr("AUTO")) + " " + clockMark.target
                            color: "#FFFFFF"; opacity: 0.7
                            font.bold: true; font.pixelSize: trackViewRoot.fs(10); font.letterSpacing: trackViewRoot.fs(10) * 0.08
                        }
                    }
                }

                // row 1: SECTION
                RowLabel { x: lookCard.inX; y: deck.rowY(1); width: lookCard.labW; height: trackViewRoot.rowH; text: "SECTION" }
                // AUTO, not FOLLOW, and on the LEFT (runde 140): one idea of "let the engine decide"
                Btn
                {
                    objectName: "followMusic"
                    x: lookCard.autoX; y: deck.rowY(1); width: lookCard.autoW; height: trackViewRoot.rowH
                    text: qsTr("AUTO"); icon: "revert"; autoKind: true
                    active: trackManager ? trackManager.overrideState === "" : true
                    onTapped: trackManager.overrideState = ""
                }
                Repeater
                {
                    model: trackViewRoot.states
                    Btn
                    {
                        objectName: "section:" + modelData
                        x: lookCard.cX + index * (lookCard.cell(4) + lookCard.cg)
                        y: deck.rowY(1); width: lookCard.cell(4); height: trackViewRoot.rowH
                        text: modelData.toUpperCase()
                        tone: trackViewRoot.markerColor(modelData)
                        // pinned by hand: the full colour and a white ring; playing
                        // now: the full colour; at rest: its colour as a bar (r403)
                        active: trackManager ? (trackManager.overrideState === modelData || trackViewRoot.liveState === modelData) : false
                        solid: true
                        idleTint: true
                        ring: trackManager ? trackManager.overrideState === modelData : false
                        onTapped: trackManager.overrideState = (trackManager.overrideState === modelData) ? "" : modelData
                    }
                }

                // row 2: POSITION (runde 378) - AUTO and three held positions
                Item
                {
                    objectName: "positionsPanel"
                    anchors.fill: parent
                    visible: trackManager && trackEngine && trackManager.roleMode
                    RowLabel { x: lookCard.inX; y: deck.rowY(2); width: lookCard.labW; height: trackViewRoot.rowH; text: "POSITION"
                               visible: !trackViewRoot.trackApp }   // TRACK_APP_BARS_R552: HEADS / BARS there
                    // TRACK_APP_BARS_R552 / TRACK_APP_TOP_R554: the row's two pages - HEADS (the moving heads) over
                    // BARS (the laser bars), the lit one is the page shown; one tap anywhere on the pair switches
                    Item
                    {
                        objectName: "posPage"
                        visible: trackViewRoot.trackApp
                        x: lookCard.inX                   // TRACK_APP_LABELS_R555: the labels' column
                        y: deck.rowY(2); width: lookCard.labW; height: trackViewRoot.rowH
                        scale: posPageInput.down ? 0.965 : 1.0
                        Repeater
                        {
                            model: [ { key: trackViewRoot.posPage, label: trackViewRoot.posPage === "bars" ? qsTr("BARS") : (trackViewRoot.posPage === "laser" ? qsTr("LASER") : qsTr("HEADS")) } ]
                            Btn
                            {
                                objectName: "posPage:shown"
                                // TRACK_APP_UI_R561: one tile - the page shown; a tap goes HEADS > BARS > LASER
                                width: parent.width; height: parent.height
                                radius: Math.round(6 * trackViewRoot.ks)
                                text: modelData.label
                                fontPx: trackViewRoot.fs(12)
                                tone: trackViewRoot.cGold
                                active: false                     // TRACK_APP_UI_R566: a page, not a function that is on
                                // the bars under his hand while HEADS shows: a gold foot on BARS
                                touchInputEnabled: false          // the pair's own input below takes every tap
                            }
                        }
                        Row
                        {
                            // TRACK_APP_UI_R561: HEADS · BARS · LASER - the one shown dark; white = in his hand
                            anchors.horizontalCenter: parent.horizontalCenter
                            anchors.bottom: parent.bottom; anchors.bottomMargin: Math.round(6 * trackViewRoot.ks)
                            spacing: Math.round(6 * trackViewRoot.ks)    // TRACK_APP_DOTSIZE_R603: was 5
                            Repeater
                            {
                                model: [ "heads", "bars", "laser" ]
                                Rectangle
                                {
                                    objectName: "posDot:" + modelData
                                    // TRACK_APP_DOTPULSE_R602: the heads' own page counts too (a position held: not AUTO)
                                    readonly property bool hand: (modelData === "bars" && trackViewRoot.barsHand) || (modelData === "laser" && trackViewRoot.aniHand)
                                                                 || (modelData === "heads" && trackViewRoot.trackApp && !!trackEngine && trackEngine.positionMode !== "")
                                    // TRACK_APP_DOTPULSE_R602 (Tobias 11-14: "Kan prikken pulsere eller blinke, hvis siden IKKE er
                                    // paa auto? ... saa man kan se at den er live naar man ikke er paa siden"): a page in his hand
                                    // that is not the one shown pulses white; the one shown keeps its gold with the white ring
                                    readonly property bool pulsing: hand && trackViewRoot.posPage !== modelData
                                    property real pulse: 1.0
                                    opacity: pulsing ? pulse : 1.0
                                    SequentialAnimation on pulse
                                    {
                                        running: parent.pulsing && trackViewRoot.animate
                                        loops: Animation.Infinite
                                        NumberAnimation { to: 0.2; duration: 600; easing.type: Easing.InOutSine }
                                        NumberAnimation { to: 1.0; duration: 600; easing.type: Easing.InOutSine }
                                    }
                                    // TRACK_APP_DOTSIZE_R603 (Tobias 11-14: "du maa gerne goere alle prikkerne en lille smule
                                    // stoerre"): 6 -> 8 (5 px at 1366, 7 px at 1920; was 4 / 6)
                                    width: Math.max(5, Math.round(8 * trackViewRoot.ks)); height: width; radius: width / 2
                                    // on the gold tile: the page shown dark, the others faint, a page in his hand white
                                    color: trackViewRoot.posPage === modelData ? trackViewRoot.cGold : (hand ? "#FFFFFF" : "#4A4A54")
                                    border.width: trackViewRoot.posPage === modelData && hand ? 1 : 0   // TRACK_APP_UI_R566 (R567: 1 px)
                                    border.color: "#FFFFFF"
                                }
                            }
                        }
                        TouchInput
                        {
                            id: posPageInput
                            z: 2
                            onReleasedAt: (x, y, inside) => {
                                if (inside)
                                    trackViewRoot.posPage = trackViewRoot.posPage === "heads" ? "bars"
                                                            : (trackViewRoot.posPage === "bars" ? "laser" : "heads")   // TRACK_APP_UI_R561
                            }
                        }
                    }
                    Btn
                    {
                        objectName: "positionAuto"
                        visible: !trackViewRoot.trackApp || trackViewRoot.posPage === "heads"   // TRACK_APP_BARS_R552
                        x: lookCard.autoX; y: deck.rowY(2); width: lookCard.autoW; height: trackViewRoot.rowH
                        text: qsTr("AUTO"); icon: "revert"; autoKind: true
                        active: trackEngine ? trackEngine.positionMode === "" : true
                        onTapped: trackEngine.positionMode = ""
                    }
                    Repeater
                    {
                        model: [ { key: "start", label: qsTr("START POSITION") },
                                 { key: "column", label: qsTr("CENTER COLUMN") },
                                 { key: "down", label: qsTr("STRAIGHT DOWN") } ]
                        Btn
                        {
                            objectName: "position:" + modelData.key
                            visible: !trackViewRoot.trackApp || trackViewRoot.posPage === "heads"   // TRACK_APP_BARS_R552
                            x: lookCard.cX + index * (lookCard.cell(3) + lookCard.cg)
                            y: deck.rowY(2); width: lookCard.cell(3); height: trackViewRoot.rowH
                            text: modelData.label
                            tone: trackViewRoot.cGold
                            active: trackEngine ? trackEngine.positionMode === modelData.key : false
                            // tapped again: AUTO
                            onTapped: trackEngine.positionMode = (trackEngine.positionMode === modelData.key) ? "" : modelData.key
                        }
                    }
                }

                // TRACK_APP_BARS_R552 (Tobias 11-04, mockup B): the POSITION row's BARS page - the laser bars by
                // hand. AUTO = the engine; ON = lit, still; HOME / HOME UNDER / DOWN = where they point (held
                // until AUTO); CHASE = chases over all the bars up to 40 % ENERGY, the engine's moves above
                Item
                {
                    objectName: "barsPanel"
                    anchors.fill: parent
                    visible: trackViewRoot.trackApp && trackViewRoot.posPage === "bars"
                             && !!trackManager && !!trackEngine && trackManager.roleMode
                    Btn
                    {
                        objectName: "barsAuto"
                        x: lookCard.autoX; y: deck.rowY(2); width: lookCard.autoW; height: trackViewRoot.rowH
                        text: qsTr("AUTO"); icon: "revert"; autoKind: true
                        active: !trackViewRoot.barsHand
                        onTapped: trackEngine.barsAuto()
                    }
                    Repeater
                    {
                        model: [ { key: "on", label: qsTr("ON") },
                                 { key: "home", label: qsTr("HOME") },
                                 { key: "under", label: qsTr("HOME UNDER") },
                                 { key: "down", label: qsTr("DOWN") },
                                 { key: "chase", label: qsTr("CHASE") } ]
                        Btn
                        {
                            objectName: "bars:" + modelData.key
                            x: lookCard.cX + index * (lookCard.cell(5) + lookCard.cg)
                            y: deck.rowY(2); width: lookCard.cell(5); height: trackViewRoot.rowH
                            text: modelData.label
                            tone: trackViewRoot.cGold
                            active: !trackEngine ? false
                                    : (modelData.key === "on" ? trackEngine.barsOn
                                       : (modelData.key === "chase" ? trackEngine.barsChase
                                          : trackEngine.barsAim === modelData.key))
                            onTapped:
                            {
                                if (modelData.key === "on")
                                    trackEngine.barsOn = !trackEngine.barsOn
                                else if (modelData.key === "chase")
                                    trackEngine.barsChase = !trackEngine.barsChase
                                else          // tapped again: AUTO for the aim
                                    trackEngine.barsAim = (trackEngine.barsAim === modelData.key) ? "" : modelData.key
                            }
                        }
                    }
                }

                // TRACK_APP_UI_R561 (Tobias 11-06, "Lav 3", "skal vaere manuelt at man kan taende laseren paa et
                // program ..." and "trykke og vaelge fra en liste ... Hvis der er manuelt valgt et, og der ikke er sat
                // on skal looket ramme den naeste sektion"): the POSITION row's LASER page - the animation lasers by
                // hand. AUTO = the engine; ON = on stage in every look (his pick: under 70 % ENERGY and with the bars
                // too) until AUTO; the name = the list of the rig's laser programmes (with ON: plays now, for this
                // section; without: lights the lasers in the next section); ENGINE'S = the engine's programme again.
                Item
                {
                    objectName: "aniPanel"
                    anchors.fill: parent
                    visible: trackViewRoot.trackApp && trackViewRoot.posPage === "laser"
                             && !!trackManager && !!trackEngine && trackManager.roleMode
                    readonly property real cw: lookCard.cell(5)
                    Btn
                    {
                        objectName: "aniAuto"
                        x: lookCard.autoX; y: deck.rowY(2); width: lookCard.autoW; height: trackViewRoot.rowH
                        text: qsTr("AUTO"); icon: "revert"; autoKind: true
                        active: !trackViewRoot.aniHand
                        onTapped: trackEngine.aniAuto()
                    }
                    Btn
                    {
                        objectName: "ani:on"
                        x: lookCard.cX; y: deck.rowY(2); width: parent.cw; height: trackViewRoot.rowH
                        text: qsTr("ON"); tone: trackViewRoot.cGold
                        active: !!trackEngine && trackEngine.aniOn
                        onTapped: trackEngine.aniOn = !trackEngine.aniOn
                    }
                    Rectangle
                    {
                        // the programme: his pick (gold; waiting for the next section: a gold frame) or the
                        // engine's - a tap opens the list
                        objectName: "ani:name"
                        readonly property bool mine: !!trackEngine && trackEngine.aniPicked
                        readonly property bool waits: mine && trackEngine.aniQueued
                        x: lookCard.cX + (parent.cw + lookCard.cg); y: deck.rowY(2)
                        width: 3 * parent.cw + 2 * lookCard.cg; height: trackViewRoot.rowH
                        radius: Math.round(10 * trackViewRoot.ks)
                        color: mine && !waits ? trackViewRoot.cGold : "#1C1C21"
                        border.width: waits ? 2 : 0
                        border.color: trackViewRoot.cGold
                        scale: aniNameInput.down ? 0.97 : 1.0
                        Column
                        {
                            anchors.centerIn: parent
                            width: parent.width - 2 * Math.round(40 * trackViewRoot.ks)   // TRACK_APP_FIX_R567: clear of the list sign
                            spacing: Math.round(2 * trackViewRoot.ks)
                            Text
                            {
                                objectName: "aniNameText"
                                width: parent.width; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight
                                text: !trackEngine || trackEngine.aniProgram === "" ? qsTr("NOT ON STAGE") : trackEngine.aniProgram.toUpperCase()
                                color: parent.parent.mine && !parent.parent.waits ? "#1A1205" : "#E6E6EA"
                                font.bold: true; font.pixelSize: trackViewRoot.fs(13); font.letterSpacing: trackViewRoot.fs(13) * 0.06
                            }
                            Text
                            {
                                objectName: "aniSubText"
                                width: parent.width; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight
                                text: !trackEngine ? ""
                                      : (parent.parent.waits ? qsTr("YOUR PICK · NEXT %1").arg(trackEngine.aniSection !== "" ? trackEngine.aniSection.toUpperCase() : qsTr("SECTION"))
                                         : (parent.parent.mine ? qsTr("YOUR PICK · THIS %1").arg(trackEngine.aniSection !== "" ? trackEngine.aniSection.toUpperCase() : qsTr("SECTION"))
                                            : qsTr("ENGINE'S PICK")))
                                color: parent.parent.waits ? trackViewRoot.cGold : (parent.parent.mine ? "#3A2A08" : "#8A8A94")
                                font.bold: true; font.pixelSize: trackViewRoot.fs(10); font.letterSpacing: trackViewRoot.fs(10) * 0.1
                            }
                        }
                        Text
                        {
                            // the list opens from here
                            anchors.right: parent.right; anchors.rightMargin: Math.round(14 * trackViewRoot.ks)
                            anchors.verticalCenter: parent.verticalCenter
                            text: "☰"
                            color: parent.mine && !parent.waits ? "#1A1205" : "#8A8A94"
                            font.pixelSize: trackViewRoot.fs(16)
                        }
                        TouchInput
                        {
                            id: aniNameInput
                            anchors.fill: parent
                            onReleasedAt: (x, y, inside) => { if (inside) trackViewRoot.aniListOpen = true }
                        }
                    }
                    Btn
                    {
                        // TRACK_APP_UI_R562: NEXT - the next look on the list (AUTO gives the lasers back)
                        objectName: "ani:next"
                        x: lookCard.cX + 4 * (parent.cw + lookCard.cg); y: deck.rowY(2); width: parent.cw; height: trackViewRoot.rowH
                        text: qsTr("NEXT") + "  ▶"; tone: trackViewRoot.cGold
                        onTapped: trackEngine.aniStep(1)
                    }
                }

                // row 3: COLOUR - lit = in the mix (runde 304), ring = leading now
                RowLabel { x: lookCard.inX; y: deck.rowY(3); width: lookCard.labW; height: trackViewRoot.rowH; text: "COLOUR" }
                Btn
                {
                    objectName: "autoColour"
                    x: lookCard.autoX; y: deck.rowY(3); width: lookCard.autoW; height: trackViewRoot.rowH
                    text: qsTr("AUTO"); icon: "autoColour"; autoKind: true
                    // runde 370: lit while the engine decides - no tile, or two and
                    // more taking turns its way; FADE or CHASE put it out
                    active: trackEngine ? (trackEngine.colourMode === 0
                                           && (trackEngine.colourOverride === "" || trackEngine.colourOverrides.length > 1)) : true
                    onTapped: trackEngine.colourOverride = ""
                }
                Repeater
                {
                    id: palRep
                    model: trackEngine ? trackEngine.palette : []
                    Btn
                    {
                        id: chip
                        objectName: "colour:" + (modelData || "")
                        property int cells: Math.max(1, palRep.count)
                        property bool leading: trackEngine && trackEngine.currentColour === modelData
                                               && (trackEngine.colourOverride === "" || trackEngine.colourOverrides.length > 1)
                        x: lookCard.cX + index * (lookCard.cell(cells) + lookCard.cg)
                        y: deck.rowY(3); width: lookCard.cell(cells); height: trackViewRoot.rowH
                        text: (modelData || "").toUpperCase()
                        tone: trackViewRoot.swatch(modelData || "")
                        active: trackEngine ? trackEngine.colourOverrides.indexOf(modelData) >= 0 : false
                        solid: true
                        idleTint: true
                        fontPx: trackViewRoot.fs(cells > 7 ? 12 : 14)
                        onTapped: trackEngine.toggleColourOverride(modelData)
                        // the ring: the colour leading right now
                        Rectangle
                        {
                            visible: chip.leading
                            anchors.fill: parent
                            anchors.margins: -5
                            radius: chip.radius + 3
                            color: "transparent"
                            border.width: 2
                            border.color: "#FFFFFF"
                        }
                        // TRACK_APP_UI_R560: no group on stage can show it - toned down, under the blink
                        Rectangle
                        {
                            objectName: chip.objectName + ":dark"
                            anchors.fill: parent
                            radius: chip.radius
                            color: "#0A0A0D"
                            opacity: 0.62
                            visible: trackViewRoot.trackApp && !chip.active && trackViewRoot.darkColours.indexOf(modelData) >= 0
                        }
                        // runde 313: a colour the engine cannot use blinks - the press
                        // arrived, it was refused
                        Rectangle
                        {
                            anchors.fill: parent
                            radius: chip.radius
                            color: "#FFFFFF"
                            opacity: 0.0
                            SequentialAnimation on opacity
                            {
                                id: rejectBlink
                                running: false
                                loops: 3
                                NumberAnimation { to: 0.8; duration: 90 }
                                NumberAnimation { to: 0.0; duration: 140 }
                            }
                        }
                        Connections
                        {
                            target: trackEngine
                            function onColourRejected(colour) { if (colour === modelData) rejectBlink.restart() }
                        }
                    }
                }

                // row 4: COLOUR MODE (runde 370) and SPEED, on the colour grid
                RowLabel { x: lookCard.inX; y: deck.rowY(4); width: lookCard.labW; height: trackViewRoot.rowH; text: "COLOUR MODE" }
                Btn
                {
                    objectName: "colourModeFade"
                    x: lookCard.cX; y: deck.rowY(4); width: lookCard.cell(7); height: trackViewRoot.rowH
                    text: qsTr("FADE"); icon: "fadeColour"
                    tone: trackViewRoot.cBlue
                    active: trackEngine ? trackEngine.colourMode === 1 : false
                    onTapped: trackEngine.setColourMode(trackEngine.colourMode === 1 ? 0 : 1)
                    // runde 374: how far the fade is - from the colour it leaves to the one it goes to
                    Rectangle
                    {
                        objectName: "colourFadeBar"
                        anchors.left: parent.left; anchors.bottom: parent.bottom
                        anchors.leftMargin: 5; anchors.bottomMargin: 5
                        height: 4; radius: 2
                        visible: trackEngine ? trackEngine.colourStyle === 1 : false
                        width: Math.max(2, (parent.width - 10) * (trackEngine ? trackEngine.colourFadeT : 0))
                        gradient: Gradient
                        {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: trackViewRoot.swatch(trackEngine ? trackEngine.colourFadeFrom : "") }
                            GradientStop { position: 1.0; color: trackViewRoot.swatch(trackEngine ? trackEngine.colourFadeTo : "") }
                        }
                    }
                }
                Btn
                {
                    objectName: "colourModeChase"
                    x: lookCard.cX + lookCard.cell(7) + lookCard.cg; y: deck.rowY(4); width: lookCard.cell(7); height: trackViewRoot.rowH
                    text: qsTr("CHASE"); icon: "chaseColour"
                    tone: trackViewRoot.cBlue
                    active: trackEngine ? trackEngine.colourMode === 2 : false
                    onTapped: trackEngine.setColourMode(trackEngine.colourMode === 2 ? 0 : 2)
                }
                RowLabel
                {
                    // TRACK_APP_SPEED_R563 (Tobias 11-06: "aendre navnet tilbage til bare at vaere speed" - it is everything
                    // that steps: chases, the engine's figures, FADE / CHASE, the heads' sweeps; with 1/4 in the TRACK app)
                    x: lookCard.cX + (trackViewRoot.trackApp ? 2 : 3) * (lookCard.cell(7) + lookCard.cg)
                    y: deck.rowY(4); width: lookCard.cell(7) - Math.round(8 * trackViewRoot.ks); height: trackViewRoot.rowH
                    horizontalAlignment: Text.AlignRight
                    text: "SPEED"
                }
                // how fast the engine runs everything that steps
                Repeater
                {
                    model: trackViewRoot.trackApp ? [ { t: "¼×", v: -2, n: "speedQuarter", c: "#9AA8C0" }, { t: "½×", v: -1, n: "speed0", c: "#7A9ABA" },
                                                      { t: "1×", v: 0, n: "speed1", c: "#4FA3E3" }, { t: "2×", v: 1, n: "speed2", c: "#E3B44F" } ]
                                                  : [ { t: "½×", v: -1, n: "speed0", c: "#7A9ABA" }, { t: "1×", v: 0, n: "speed1", c: "#4FA3E3" },
                                                      { t: "2×", v: 1, n: "speed2", c: "#E3B44F" } ]
                    Btn
                    {
                        objectName: modelData.n
                        x: lookCard.cX + ((trackViewRoot.trackApp ? 3 : 4) + index) * (lookCard.cell(7) + lookCard.cg)
                        y: deck.rowY(4); width: lookCard.cell(7); height: trackViewRoot.rowH
                        text: modelData.t
                        tone: modelData.c
                        active: trackEngine ? trackEngine.speed === modelData.v : modelData.v === 0
                        onTapped: trackEngine.speed = modelData.v
                    }
                }

                // row 5: the haze machine, a group of its own - always by hand (runde 389)
                Item
                {
                    anchors.fill: parent
                    visible: trackEngine ? trackEngine.hazeAvailable : false
                    Rectangle
                    {
                        x: lookCard.inX; width: lookCard.inW; height: 1
                        y: deck.rowY(5) - Math.round(trackViewRoot.g / 2) - 1
                        color: trackViewRoot.cEdge
                    }
                    // R467_HAZE_NAG: haze at 0 for 10 min after 22:00 - the label says so in
                    // gold and LATER quiets it for 30 min; the engine never touches the haze
                    readonly property int hazeNag: trackEngine ? trackEngine.hazeNag : 0
                    RowLabel
                    {
                        objectName: "hazeLabel"
                        x: lookCard.inX; y: deck.rowY(5); width: lookCard.labW; height: trackViewRoot.rowH
                        text: parent.hazeNag > 0 ? qsTr("HAZE AT 0") + "\n" + qsTr("%1 MIN").arg(parent.hazeNag) : "HAZE MACHINE"
                        color: parent.hazeNag > 0 ? "#E3B44F" : trackViewRoot.cMute
                    }
                    // R484_HZ_AUTO_TILE (Tobias 10-20: "der skal oprettes en 'AUTO' knap til haze"): AUTO =
                    // haze AND fan play HAZE & FAN BY CLOCK (SETUP > ADVANCED); a hand on either takes
                    // them back, as ENERGY. LATER has this place while the nag shows
                    Btn
                    {
                        objectName: "hazeAuto"
                        visible: parent.hazeNag <= 0
                        x: lookCard.autoX; y: deck.rowY(5); width: lookCard.autoW; height: trackViewRoot.rowH
                        text: qsTr("AUTO"); icon: "autoColour"; autoKind: true
                        active: trackEngine ? trackEngine.hazeAuto === true : false
                        onTapped: if (trackEngine) trackEngine.hazeAuto = true
                    }
                    Btn
                    {
                        objectName: "hazeLater"
                        visible: parent.hazeNag > 0
                        x: lookCard.autoX; y: deck.rowY(5); width: lookCard.autoW; height: trackViewRoot.rowH
                        text: qsTr("LATER"); tone: "#E3B44F"; active: true; solid: false
                        onTapped: if (trackEngine) trackEngine.hazeLater()
                    }
                    Fader
                    {
                        objectName: "haze"
                        inputName: "hazeDrag"
                        x: lookCard.cX; y: deck.rowY(5)
                        width: (lookCard.cW - trackViewRoot.g) / 2; height: trackViewRoot.rowH
                        name: qsTr("HAZE")
                        nameInk: "#121214"
                        level: trackEngine ? trackEngine.haze : 0
                        fill: "#8A8A8A"
                        gripInk: "#C8C8C8"
                        onSetLevel: (v) => { if (v < 0.03) v = 0; if (trackEngine) trackEngine.haze = v }
                        // R484_HZ_SPRING (Tobias 10-20: "faderen bliver 'elastisk' at man midlertidigt kan
                        // holde den paa en hvilken som helst vaerdi over 70% men at den automatisk ryger
                        // tilbage til 70 naar man slipper faderen"): let go (or taken away) over 70 - 70.
                        // A hand on it ends AUTO, as on ENERGY (the engine does it too)
                        id: hazeFader
                        rest: trackEngine ? trackEngine.hazeLimit / 100 : 0.7
                        onPressed: if (trackEngine && trackEngine.hazeAuto === true) trackEngine.hazeAuto = false
                        function springBack() { if (trackEngine && trackEngine.haze > hazeFader.rest + 0.001) trackEngine.haze = hazeFader.rest }
                        Component.onDestruction: if (input.down) springBack()   // R544_HZGONE_V2: torn down under the finger
                        Connections
                        {
                            target: hazeFader.input
                            function onReleasedAt(x, y, inside) { hazeFader.springBack() }
                            function onCanceled() { hazeFader.springBack() }
                        }
                    }
                    Fader
                    {
                        objectName: "fan"
                        inputName: "fanDrag"
                        x: lookCard.cX + (lookCard.cW + trackViewRoot.g) / 2; y: deck.rowY(5)
                        width: (lookCard.cW - trackViewRoot.g) / 2; height: trackViewRoot.rowH
                        name: qsTr("FAN SPEED")
                        nameInk: "#0C141B"
                        level: trackEngine ? trackEngine.fan : 0
                        fill: "#6A8AA0"
                        gripInk: "#A8C4D8"
                        onSetLevel: (v) => { if (v < 0.03) v = 0; if (trackEngine) trackEngine.fan = v }
                        onPressed: if (trackEngine && trackEngine.hazeAuto === true) trackEngine.hazeAuto = false   // R484_HZ_FAN_HAND: a hand ends AUTO
                    }
                }
            }

            // ---- LIVE: one press, right now - the urgent ones lowest ----
            Card
            {
                id: liveCard
                title: "LIVE"
                Component.onCompleted: Qt.callLater(trackViewRoot.hookLive, liveCard)   // TRACK_APP_LIVEFREE_R576
                x: parent.width - trackViewRoot.liveW
                y: 0
                width: trackViewRoot.liveW
                height: parent.height
                // under SETUP or HELP since runde 393: nothing here may take a finger
                enabled: !trackViewRoot.setupOpen && !trackViewRoot.helpOpen
                opacity: trackViewRoot.setupOpen ? 0 : 1
                readonly property real inX: trackViewRoot.cardPad + 2
                readonly property real inW: width - 2 * inX
                // BLACKOUT and FLASH WHITE share the last three rows: one and a half
                // each - FLASH "lidt mindre" (runde 389), BLACKOUT a bigger target
                readonly property real bigH: (3 * trackViewRoot.rowH + trackViewRoot.g) / 2
                // R420_STUTTER_TILE: BLACKOUT | STUTTER share BLACKOUT's row
                readonly property real halfW: (inW - trackViewRoot.g) / 2

                Btn
                {
                    objectName: "undoLook"
                    x: liveCard.inX; y: deck.rowY(0); width: liveCard.halfW; height: trackViewRoot.rowH
                    icon: "revert"
                    text: qsTr("UNDO")
                    enabled: trackViewRoot.savedUndoAvailable
                    onTapped: if (trackEngine) trackEngine.undoLook()
                }
                Btn
                {
                    objectName: "nextLook"
                    x: liveCard.inX + liveCard.halfW + trackViewRoot.g; y: deck.rowY(0)
                    width: liveCard.halfW; height: trackViewRoot.rowH
                    icon: "nextLook"
                    text: trackViewRoot.trackApp ? qsTr("NEXT LOOK") : qsTr("NEXT")   // TRACK_APP_UI_R560
                    onTapped: if (trackEngine) trackEngine.next()
                }
                Btn
                {
                    objectName: "hold"
                    x: liveCard.inX; y: deck.rowY(1); width: liveCard.inW; height: trackViewRoot.rowH
                    icon: "hold"
                    text: qsTr("HOLD")
                    tone: trackViewRoot.cGold
                    active: trackEngine ? trackEngine.hold : false
                    onTapped: trackEngine.hold = !trackEngine.hold
                }
                Btn
                {
                    objectName: "calm"
                    x: liveCard.inX; y: deck.rowY(2); width: liveCard.inW; height: trackViewRoot.rowH
                    icon: "calm"
                    text: (trackEngine && trackEngine.calmBarsLeft > 0) ? qsTr("CALM") + " " + trackEngine.calmBarsLeft : qsTr("CALM")
                    tone: trackViewRoot.cBlue
                    active: trackEngine ? trackEngine.calmBarsLeft > 0 : false
                    onTapped: trackEngine.calm(trackEngine.calmBarsLeft > 0 ? 0 : 16)
                }

                // BLACKOUT: held = dark while held; slid off = latched; a tap on a
                // latched one lets it go (runde 296: a grab taken away lets go)
                Rectangle
                {
                    id: blackoutTile
                    objectName: "blackout"
                    property bool armed: false          // this press is the one holding it
                    property bool on: trackEngine ? trackEngine.blackout : false
                    // R452_HIT_LIVE: BLACKOUT on the right, beside STUTTER (Tobias 10-14)
                    x: liveCard.inX + liveCard.halfW + trackViewRoot.g; y: deck.rowY(3); width: liveCard.halfW; height: liveCard.bigH
                    radius: Math.round(10 * trackViewRoot.ks)
                    color: on ? "#B03030" : (boIn.down ? "#1A1A1F" : "#0A0A0C")
                    Behavior on color { enabled: trackViewRoot.animate; ColorAnimation { duration: 120 } }     // R402_FADE
                    scale: boIn.down ? 0.975 : 1.0                                                           // R402_PRESS
                    Behavior on scale { enabled: trackViewRoot.animate; NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
                    Row
                    {
                        anchors.centerIn: parent
                        spacing: Math.round(9 * trackViewRoot.ks)
                        ControlIcon { anchors.verticalCenter: parent.verticalCenter; width: Math.round(18 * trackViewRoot.ks); height: width
                                      kind: "blackout"; ink: blackoutTile.on ? "#101010" : "#D6D6DC" }
                        Text
                        {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("BLACKOUT")
                            color: blackoutTile.on ? "#101010" : "#D6D6DC"
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(14)
                            font.letterSpacing: (trackViewRoot.fs(14)) * 0.1
                        }
                    }
                    TouchInput
                    {
                        id: boIn
                        onPressedAt: (x, y) =>
                        {
                            if (!trackEngine) return
                            if (trackEngine.blackout) { trackEngine.blackout = false; blackoutTile.armed = false }
                            else { trackEngine.blackout = true; blackoutTile.armed = true }
                        }
                        onReleasedAt: (x, y, inside) =>
                        {
                            if (!trackEngine || !blackoutTile.armed) return
                            blackoutTile.armed = false
                            // released ON the button: a momentary hold, let go; off it: latched
                            if (inside) trackEngine.blackout = false
                        }
                        onCanceled:
                        {
                            if (blackoutTile.armed && trackEngine) trackEngine.blackout = false
                            blackoutTile.armed = false
                        }
                        // R544_BOGONE_V2: the page torn down with the finger still down - no release comes (as r199's FLASH)
                        Component.onDestruction: if (blackoutTile.armed && trackEngine) trackEngine.blackout = false
                    }
                }

                // R420_STUTTER_TILE (Tobias 10-10, mockup A: "der skal vaere mulighed
                // for at holde knappen nede ligesom blackout, hvor man slider fingeren
                // vaek og den saa holder sig selv nede"): held = dark on every 1/4,
                // 1/8 or 1/16; slid off = latched; a tap on a latched one lets it go.
                // R427_STUTTER_TAP (Tobias 10-11): a quick tap (< 250 ms) locks it too.
                // The chips choose the rate (it stays, also while it runs).
                Rectangle
                {
                    id: stutterTile
                    objectName: "stutter"
                    property bool armed: false          // this press is the one holding it
                    property double pressedAt: 0        // R427_STUTTER_TAP: when that finger landed
                    readonly property int tapMs: 250    // shorter than this = a tap, it locks
                    property bool on: trackEngine ? trackEngine.stutter : false
                    readonly property int rate: trackEngine ? trackEngine.stutterRate : 3
                    x: liveCard.inX; y: deck.rowY(3)          // R452_HIT_LIVE: STUTTER on the left
                    width: liveCard.halfW; height: liveCard.bigH
                    radius: Math.round(10 * trackViewRoot.ks)
                    color: on ? "#E0A030" : (stIn.down ? "#1A1A1F" : "#0A0A0C")
                    Behavior on color { enabled: trackViewRoot.animate; ColorAnimation { duration: 120 } }     // R402_FADE
                    scale: stIn.down ? 0.975 : 1.0                                                           // R402_PRESS
                    Behavior on scale { enabled: trackViewRoot.animate; NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
                    readonly property real chipH: Math.round(30 * trackViewRoot.ks)
                    readonly property real pad: Math.round(8 * trackViewRoot.ks)
                    Column
                    {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: -(stutterTile.chipH + stutterTile.pad) / 2
                        spacing: Math.round(4 * trackViewRoot.ks)
                        Row
                        {
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: Math.round(4 * trackViewRoot.ks)
                            Repeater
                            {
                                model: 4
                                Rectangle
                                {
                                    width: Math.round(4 * trackViewRoot.ks); height: Math.round(16 * trackViewRoot.ks); radius: 1
                                    color: stutterTile.on ? "#101010" : "#D6D6DC"
                                    opacity: index % 2 ? 0.35 : 1
                                }
                            }
                        }
                        Text
                        {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: qsTr("STUTTER")
                            color: stutterTile.on ? "#101010" : "#D6D6DC"
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(14)
                            font.letterSpacing: (trackViewRoot.fs(14)) * 0.1
                        }
                    }
                    TouchInput
                    {
                        id: stIn
                        onPressedAt: (x, y) =>
                        {
                            if (!trackEngine) return
                            if (trackEngine.stutter) { trackEngine.stutter = false; stutterTile.armed = false }
                            else { trackEngine.stutter = true; stutterTile.armed = true; stutterTile.pressedAt = Date.now() }
                        }
                        onReleasedAt: (x, y, inside) =>
                        {
                            if (!trackEngine || !stutterTile.armed) return
                            stutterTile.armed = false
                            // released ON the button after a hold: momentary, let go; off it: latched.
                            // R427_STUTTER_TAP: a quick tap on it locks it (10/10: two taps of
                            // 83-98 ms did nothing) - the next tap lets it go
                            if (inside && Date.now() - stutterTile.pressedAt >= stutterTile.tapMs) trackEngine.stutter = false
                        }
                        onCanceled:
                        {
                            if (stutterTile.armed && trackEngine) trackEngine.stutter = false
                            stutterTile.armed = false
                        }
                        Component.onDestruction: if (stutterTile.armed && trackEngine) trackEngine.stutter = false   // R544_ST_GONE
                    }
                    // the rate: over the hold area, each chip its own finger
                    Row
                    {
                        id: stutterChips
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom; anchors.bottomMargin: stutterTile.pad
                        spacing: Math.round(6 * trackViewRoot.ks)
                        Repeater
                        {
                            model: [ { r: 1, t: "1/4" }, { r: 2, t: "1/8" }, { r: 3, t: "1/16" } ]
                            Rectangle
                            {
                                objectName: "stutterRate:" + modelData.r
                                readonly property bool picked: stutterTile.rate === modelData.r
                                width: Math.floor((stutterTile.width - 2 * stutterTile.pad - 2 * stutterChips.spacing) / 3)
                                height: stutterTile.chipH
                                radius: Math.round(6 * trackViewRoot.ks)
                                color: picked ? (stutterTile.on ? "#101010" : "#E0A030")
                                              : (stutterTile.on ? Qt.rgba(0, 0, 0, 0.18) : "#26262E")
                                Text
                                {
                                    anchors.centerIn: parent
                                    text: modelData.t
                                    font.bold: true
                                    font.pixelSize: trackViewRoot.fs(12)
                                    color: parent.picked ? (stutterTile.on ? "#E0A030" : "#101010")
                                                         : (stutterTile.on ? "#101010" : "#9A9AA4")
                                }
                                // TRACK_APP_FIX_R595 (Tobias 11-13, "Lav 3"): a finger target bigger than the chip it draws -
                                // a near miss above or beside a 30 px chip pressed the tile under it (STUTTER on) instead
                                // of the rate. Up over the chip, down to the tile's edge, half the gap to each side
                                Item
                                {
                                    x: -Math.round(3 * trackViewRoot.ks); y: -Math.round(12 * trackViewRoot.ks)
                                    width: parent.width + Math.round(6 * trackViewRoot.ks)
                                    height: parent.height + Math.round(12 * trackViewRoot.ks) + stutterTile.pad
                                    TouchInput
                                    {
                                        onPressedAt: (x, y) => { if (trackEngine) trackEngine.stutterRate = modelData.r }
                                    }
                                }
                            }
                        }
                    }
                }

                // R452_HIT_LIVE (Tobias 10-14, mockup A/B: "Drop hit skal ligge ved siden af
                // FLASH WHITE, ligesom Stutter med samme stil for valg af flash, dark etc."):
                // a tap on the tile arms the gold kind (the last one) or takes it back; a
                // chip arms that kind. Lands once on the next drop and disarms (R445).
                Rectangle
                {
                    id: hitTile
                    objectName: "dropHit"
                    readonly property bool armed: trackEngine ? trackEngine.hitKind > 0 : false
                    readonly property int kind: trackEngine ? (trackEngine.hitKind > 0 ? trackEngine.hitKind : trackEngine.hitLast) : 1
                    readonly property string text: armed ? qsTr("HIT ARMED") : qsTr("DROP HIT")
                    x: liveCard.inX; y: deck.rowY(3) + liveCard.bigH + trackViewRoot.g
                    width: liveCard.halfW; height: liveCard.bigH
                    radius: Math.round(10 * trackViewRoot.ks)
                    color: armed ? "#E23B3B" : (hitIn.down ? "#1A1A1F" : "#0A0A0C")
                    Behavior on color { enabled: trackViewRoot.animate; ColorAnimation { duration: 120 } }     // R402_FADE
                    scale: hitIn.down ? 0.975 : 1.0                                                           // R402_PRESS
                    Behavior on scale { enabled: trackViewRoot.animate; NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
                    readonly property real chipH: Math.round(30 * trackViewRoot.ks)
                    readonly property real pad: Math.round(8 * trackViewRoot.ks)
                    readonly property color ink: armed ? "#2A0808" : "#D6D6DC"
                    Column
                    {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.verticalCenterOffset: -(hitTile.chipH + hitTile.pad) / 2
                        spacing: Math.round(4 * trackViewRoot.ks)
                        ControlIcon { anchors.horizontalCenter: parent.horizontalCenter; width: Math.round(18 * trackViewRoot.ks); height: width
                                      kind: "bolt"; ink: hitTile.ink }
                        Text
                        {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: hitTile.text
                            color: hitTile.ink
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(14)
                            font.letterSpacing: (trackViewRoot.fs(14)) * 0.1
                        }
                    }
                    TouchInput
                    {
                        id: hitIn
                        onReleasedAt: (x, y, inside) => { if (inside && trackEngine) trackEngine.hitKind = hitTile.armed ? 0 : trackEngine.hitLast }
                    }
                    // the kind: over the tile, each chip its own finger (as STUTTER's rate)
                    Row
                    {
                        id: hitChips
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom; anchors.bottomMargin: hitTile.pad
                        spacing: Math.round(6 * trackViewRoot.ks)
                        Repeater
                        {
                            model: [ { k: 1, t: qsTr("FLSH") }, { k: 2, t: qsTr("STRB") }, { k: 3, t: qsTr("DARK") } ]   // as the approved mockup
                            Rectangle
                            {
                                objectName: "hitKind:" + modelData.k
                                readonly property bool picked: hitTile.kind === modelData.k
                                width: Math.floor((hitTile.width - 2 * hitTile.pad - 2 * hitChips.spacing) / 3)
                                height: hitTile.chipH
                                radius: Math.round(6 * trackViewRoot.ks)
                                color: picked ? "#E0A030" : (hitTile.armed ? Qt.rgba(0, 0, 0, 0.22) : "#26262E")
                                Text
                                {
                                    anchors.centerIn: parent
                                    width: parent.width - 4
                                    horizontalAlignment: Text.AlignHCenter
                                    text: modelData.t
                                    font.bold: true
                                    font.pixelSize: trackViewRoot.fs(11)
                                    fontSizeMode: Text.HorizontalFit
                                    minimumPixelSize: 7
                                    color: parent.picked ? "#101010" : (hitTile.armed ? "#3A0E0E" : "#9A9AA4")
                                }
                                // TRACK_APP_FIX_R595: as the STUTTER chips - a near miss armed or disarmed the HIT instead
                                Item
                                {
                                    x: -Math.round(3 * trackViewRoot.ks); y: -Math.round(12 * trackViewRoot.ks)
                                    width: parent.width + Math.round(6 * trackViewRoot.ks)
                                    height: parent.height + Math.round(12 * trackViewRoot.ks) + hitTile.pad
                                    TouchInput
                                    {
                                        onPressedAt: (x, y) => { if (trackEngine) trackEngine.hitKind = modelData.k }
                                    }
                                }
                            }
                        }
                    }
                }

                // FLASH WHITE: on while held. Dark with a white edge at rest - a big
                // light tile glares in a dark booth - and white while it flashes
                Rectangle
                {
                    id: flashTile
                    objectName: "flash"
                    property bool on: trackEngine ? trackEngine.flashing : false
                    // R452_HIT_LIVE: the right half, beside DROP HIT
                    x: liveCard.inX + liveCard.halfW + trackViewRoot.g; y: deck.rowY(3) + liveCard.bigH + trackViewRoot.g
                    width: liveCard.halfW
                    height: liveCard.bigH
                    radius: Math.round(10 * trackViewRoot.ks)
                    color: on ? "#FFFFFF" : (flashIn.down ? "#34343C" : "#24242A")
                    scale: flashIn.down ? 0.975 : 1.0                                                        // R402_PRESS
                    Behavior on scale { enabled: trackViewRoot.animate; NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
                    border.width: 2
                    border.color: "#F2F2F5"
                    Column
                    {
                        anchors.centerIn: parent
                        spacing: Math.round(6 * trackViewRoot.ks)
                        ControlIcon { anchors.horizontalCenter: parent.horizontalCenter; width: Math.round(24 * trackViewRoot.ks); height: width
                                      kind: "flash"; ink: flashTile.on ? "#101010" : "#FFFFFF" }
                        Text
                        {
                            anchors.horizontalCenter: parent.horizontalCenter
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("FLASH") + "\n" + qsTr("WHITE")      // R452_HIT_LIVE: half the width now
                            color: flashTile.on ? "#101010" : "#FFFFFF"
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(16)
                            font.letterSpacing: (trackViewRoot.fs(16)) * 0.12
                        }
                    }
                    TouchInput
                    {
                        id: flashIn
                        onPressedAt: (x, y) => { if (trackEngine) trackEngine.setFlash(true) }
                        onReleasedAt: (x, y, inside) => { if (trackEngine) trackEngine.setFlash(false) }
                        onCanceled: { if (trackEngine) trackEngine.setFlash(false) }
                        // the page destroyed with the finger still down: no release
                        // ever comes, and FLASH stood at full all night (r199)
                        Component.onDestruction: if (down && trackEngine) trackEngine.setFlash(false)
                    }
                }
            }
        }

        // ============ 4 · THE ENGINE'S LINE - and the air above the taskbar (r141) ============
        Item
        {
            Layout.fillWidth: true
            Layout.preferredHeight: trackViewRoot.footH
            Row
            {
                anchors.left: parent.left
                anchors.leftMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: -2
                width: parent.width / 2
                spacing: 12
                Text { text: qsTr("ON STAGE"); color: trackViewRoot.cMute; font.bold: true; font.pixelSize: trackViewRoot.fs(11); font.letterSpacing: (trackViewRoot.fs(11)) * 0.15
                       anchors.verticalCenter: parent.verticalCenter }
                Text
                {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 120
                    elide: Text.ElideRight
                    textFormat: Text.StyledText
                    font.pixelSize: trackViewRoot.fs(13)
                    color: trackViewRoot.cMute
                    text:
                    {
                        if (!trackEngine) return ""
                        var parts = trackEngine.report.split("  |  ")
                        var cast = parts.length > 0 ? parts[0] : ""
                        var colour = parts.length > 1 ? parts[1] : ""
                        var state = parts.length > 2 ? parts[2] : ""
                        return "<font color='#CFCFD6'>" + cast + "</font>" + (colour !== "" ? "  ·  " + colour : "") + (state !== "" ? "  ·  " + state : "")
                    }
                }
            }
            // TRACK_APP_BUILDNOTE_R601 (mockup approved 11-14): which TRACK this is - always, dim, at the end of the line
            Text
            {
                id: buildFoot
                objectName: "buildFoot"
                visible: trackViewRoot.trackApp
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: -2
                text: !visible ? "" : "TRACK  " + trackAppCtl.build
                                      + (trackAppCtl.buildRound !== undefined && trackAppCtl.buildRound !== "" ? "  \u00B7  " + trackAppCtl.buildRound : "")
                color: trackViewRoot.cDim
                font.pixelSize: trackViewRoot.fs(11)
                font.letterSpacing: trackViewRoot.fs(11) * 0.08
            }
            Row
            {
                anchors.right: buildFoot.visible ? buildFoot.left : parent.right
                anchors.rightMargin: buildFoot.visible ? Math.round(22 * trackViewRoot.ks) : 4
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: -2
                spacing: 12
                // TRACK_APP_UI_R561: the TRACK app shows it from the moment a deck is loaded - its deck, and
                // ANALYSING until the lights have its sections (NEXT goes to QLC+ only then)
                readonly property bool appNext: trackViewRoot.trackApp && trackAppCtl.nextDeck > 0 && trackAppCtl.nextState !== ""
                visible: appNext || (trackManager && trackManager.nextTitle !== undefined && trackManager.nextTitle !== "")
                Text { id: nextLabelText; text: qsTr("NEXT TRACK"); color: trackViewRoot.cMute; font.bold: true; font.pixelSize: trackViewRoot.fs(11); font.letterSpacing: (trackViewRoot.fs(11)) * 0.15
                       anchors.verticalCenter: parent.verticalCenter }
                Text
                {
                    id: nextDeckLabel
                    objectName: "nextDeckText"
                    visible: parent.appNext
                    anchors.verticalCenter: parent.verticalCenter
                    text: trackViewRoot.trackApp ? qsTr("DECK %1").arg(trackAppCtl.nextDeck) : ""   // TRACK_APP_FIX_R565
                    color: "#CFCFD6"; font.bold: true; font.pixelSize: trackViewRoot.fs(11); font.letterSpacing: trackViewRoot.fs(11) * 0.12
                }
                Rectangle
                {
                    id: nextStateBox
                    objectName: "nextStateChip"
                    visible: parent.appNext
                    anchors.verticalCenter: parent.verticalCenter
                    readonly property string st: trackViewRoot.trackApp ? trackAppCtl.nextState : ""
                    // TRACK_APP_NOGRID_R609: no beat grid / waveform from the CDJ yet - red, as a failure
                    readonly property bool waiting: st === "nogrid" || st === "nowave"
                    readonly property color ink: st === "ready" ? "#8CE696" : ((st === "failed" || waiting) ? "#F07A7A" : "#E8B85A")
                    width: nextStateText.implicitWidth + Math.round(16 * trackViewRoot.ks)
                    height: Math.round(18 * trackViewRoot.ks); radius: height / 2
                    color: st === "ready" ? "#16301A" : ((st === "failed" || waiting) ? "#3A1616" : "#33270F")
                    border.width: 1; border.color: Qt.darker(ink, 1.6)
                    Text
                    {
                        id: nextStateText
                        objectName: "nextStateText"
                        anchors.centerIn: parent
                        text: parent.st === "ready" ? "✓ " + qsTr("READY") : (parent.st === "failed" ? qsTr("NOT ANALYSED - TRYING AGAIN")
                              : parent.st === "nogrid" ? qsTr("NO BEAT GRID YET - CAN'T READ IT")      // TRACK_APP_NOGRID_R609
                              : parent.st === "nowave" ? qsTr("NO WAVEFORM YET - CAN'T READ IT")
                              : qsTr("ANALYSING…"))
                        color: parent.ink; font.bold: true; font.pixelSize: trackViewRoot.fs(10); font.letterSpacing: trackViewRoot.fs(10) * 0.1
                    }
                }
                Text
                {
                    anchors.verticalCenter: parent.verticalCenter
                    textFormat: Text.StyledText
                    font.pixelSize: trackViewRoot.fs(13)
                    color: trackViewRoot.cMute
                    objectName: "nextTitleText"
                    // TRACK_APP_FIX_R566: never past the middle of the line (ON STAGE has the left half)
                    readonly property real room: parent.parent.width / 2 - 16 - nextLabelText.width - 12
                                                 - (buildFoot.visible ? buildFoot.width + Math.round(22 * trackViewRoot.ks) : 0)   // TRACK_APP_BUILDNOTE_R601
                                                 - (nextDeckLabel.visible ? nextDeckLabel.width + 12 : 0)
                                                 - (nextStateBox.visible ? nextStateBox.width + 12 : 0)
                    width: Math.max(40, Math.min(implicitWidth, room))
                    elide: Text.ElideRight
                    // TRACK_APP_UI_R561: before NEXT has gone to QLC+, the title the deck has
                    text: !trackManager ? ""
                          : ((trackManager.nextTitle !== "" && !(parent.appNext && trackAppCtl.nextState !== "ready")) || !trackViewRoot.trackApp   // TRACK_APP_FIX_R570
                             ? "<font color='#CFCFD6'>" + trackManager.nextTitle + "</font>"
                               + (trackManager.nextFirstDrop > 0 ? "  ·  " + qsTr("first drop bar") + " " + trackManager.nextFirstDrop : "")
                             : "<font color='#9A9AA4'>" + trackAppCtl.nextTrack + "</font>")
                }
            }
        }
    }


    // TRACK_APP_UI_R561: LASER's list - the rig's laser programmes, his lab looks first; a tap picks one (with ON:
    // now, for this section; without: the next section) and closes it; a press anywhere else closes it
    Item
    {
        id: aniListCloser
        objectName: "aniListCloser"
        anchors.fill: parent
        z: 59
        visible: aniList.visible
        property real liveLeft: width                      // TRACK_APP_LIVEFREE_R576: LIVE stays free
        onVisibleChanged: if (visible) liveLeft = trackViewRoot.liveLeftNow()
        Rectangle { x: 0; y: 0; width: aniListCloser.liveLeft; height: parent.height; color: "#000000"; opacity: 0.45 }
        Item
        {
            x: 0; y: 0; width: aniListCloser.liveLeft; height: parent.height
            TouchInput { anchors.fill: parent; onPressedAt: (x, y) => trackViewRoot.aniListOpen = false }
        }
    }
    Rectangle
    {
        id: aniList
        objectName: "aniList"
        visible: trackViewRoot.aniListOpen && trackViewRoot.trackApp && !!trackEngine
        z: 60
        readonly property real pad: Math.round(18 * trackViewRoot.ks)
        readonly property var items: visible ? trackEngine.aniMenu() : []
        readonly property int cols: 4
        readonly property real cellW: (width - 2 * pad - (cols - 1) * Math.round(10 * trackViewRoot.ks)) / cols
        readonly property real cellH: Math.round(54 * trackViewRoot.ks)
        width: Math.min(trackViewRoot.width - 2 * pad, Math.round(980 * trackViewRoot.ks))
        height: Math.min(trackViewRoot.height - 2 * pad, listHead.height + aniFlow.implicitHeight + 3 * pad)
        anchors.centerIn: parent
        radius: Math.round(16 * trackViewRoot.ks)
        color: "#16161A"
        border.width: 1; border.color: "#2A2A32"
        Column
        {
            id: listHead
            x: aniList.pad; y: aniList.pad; width: aniList.width - 2 * aniList.pad
            spacing: Math.round(4 * trackViewRoot.ks)
            Text { text: qsTr("LASER LOOKS"); color: trackViewRoot.cGold; font.bold: true; font.pixelSize: trackViewRoot.fs(14); font.letterSpacing: trackViewRoot.fs(14) * 0.12 }
            Text
            {
                objectName: "aniListHint"
                text: trackEngine && trackEngine.aniOn ? qsTr("ON - your pick plays now, for the rest of this section")
                                                       : qsTr("Your pick lights the animation lasers in the NEXT section - with ON it plays now")
                color: "#8A8A94"; font.pixelSize: trackViewRoot.fs(12)
            }
        }
        Flickable
        {
            x: aniList.pad; y: listHead.y + listHead.height + aniList.pad
            width: aniList.width - 2 * aniList.pad; height: aniList.height - y - aniList.pad
            contentHeight: aniFlow.implicitHeight; clip: true
            boundsBehavior: Flickable.StopAtBounds
            Flow
            {
                id: aniFlow
                width: parent.width
                spacing: Math.round(10 * trackViewRoot.ks)
                Repeater
                {
                    model: aniList.items
                    Btn
                    {
                        objectName: "aniItem:" + index
                        width: aniList.cellW; height: aniList.cellH
                        text: (modelData.lab ? "LAB · " : "") + String(modelData.name).toUpperCase()
                        fit: true                             // TRACK_APP_FIX_R570
                        tone: trackViewRoot.cGold
                        active: modelData.now === true
                        ring: modelData.now === true && !!trackEngine && trackEngine.aniPicked
                        fontPx: trackViewRoot.fs(12)
                        onTapped: { trackEngine.aniPick(index); trackViewRoot.aniListOpen = false }
                    }
                }
            }
        }
    }

    // R452_EMPTY: the (i) guide - a press anywhere else closes it (and is taken)
    Item
    {
        id: bltGuideCloser
        objectName: "bltGuideCloser"
        anchors.fill: parent
        z: 49
        visible: bltGuide.visible
        property real liveLeft: width                      // TRACK_APP_LIVEFREE_R576: LIVE stays free
        onVisibleChanged: if (visible) liveLeft = trackViewRoot.liveLeftNow()
        Item
        {
            x: 0; y: 0; width: bltGuideCloser.liveLeft; height: parent.height
            TouchInput { onPressedAt: (x, y) => trackViewRoot.bltGuideOpen = false }
        }
    }
    Rectangle
    {
        id: bltGuide
        objectName: "bltGuide"
        // R544_GUIDE_BOX: with the box it explains (the empty page, or BLT gone) - a stale flag brought it back later
        visible: trackViewRoot.bltGuideOpen && emptyBox.visible && !trackViewRoot.setupOpen && !trackViewRoot.helpOpen
        z: 50
        readonly property real pad: Math.round(18 * trackViewRoot.ks)
        width: Math.min(trackViewRoot.width - 2 * pad, Math.round(470 * trackViewRoot.ks))
        height: guideCol.implicitHeight + 2 * pad
        // under the (i), read again when the page moves (as the PLAN card)
        readonly property real moved: trackViewRoot.width + trackViewRoot.height + waveCard.x + waveCard.y + waveCard.width + waveCard.height + emptyRow.width
        x:
        {
            var dep = moved
            var p = bltInfo.mapToItem(trackViewRoot, bltInfo.width, 0)
            return Math.max(pad, Math.min(trackViewRoot.width - width - pad, p.x - width + Math.round(120 * trackViewRoot.ks)))
        }
        y:
        {
            var dep = moved
            var p = bltInfo.mapToItem(trackViewRoot, 0, bltInfo.height)
            return Math.max(pad, Math.min(trackViewRoot.height - height - pad, p.y + Math.round(10 * trackViewRoot.ks)))
        }
        radius: Math.round(10 * trackViewRoot.ks)
        color: "#1A1A20"
        TouchInput { onReleasedAt: (x, y, inside) => trackViewRoot.bltGuideOpen = false }
        Column
        {
            id: guideCol
            x: bltGuide.pad; y: bltGuide.pad
            width: bltGuide.width - 2 * bltGuide.pad
            spacing: Math.round(12 * trackViewRoot.ks)
            Text
            {
                text: qsTr("HOW TO GET THE TRACKS IN")
                color: trackViewRoot.cGold
                font.bold: true
                font.pixelSize: trackViewRoot.fs(13)
                font.letterSpacing: trackViewRoot.fs(13) * 0.08
            }
            Repeater
            {
                model: trackViewRoot.trackApp ? [
                    qsTr("<b>TRACK reads the CDJs by itself.</b> No other program is needed."),
                    qsTr("<b>Load and play a track on a CDJ.</b> It shows up here within a second or two."),
                    qsTr("<b>Still no CDJs?</b> Check the network cable from this PC to the DJ booth."),
                    qsTr("<b>Beat Link Trigger open?</b> Close it - TRACK takes over within 15 seconds.") ] : [
                    qsTr("<b>Beat Link Trigger has to run.</b> Start it from its logo on the taskbar at the bottom of the screen."),
                    qsTr("<b>Load and play a track on a CDJ.</b> It shows up here within a second or two."),
                    qsTr("<b>Still nothing?</b> Close Beat Link Trigger and start it again from the taskbar."),
                    qsTr("<b>No taskbar?</b> QLC+ is in full screen: <b>SETUP</b> - <b>FULLSCREEN</b> takes it out (and back in).") ]
                Row
                {
                    spacing: Math.round(10 * trackViewRoot.ks)
                    Rectangle
                    {
                        width: Math.round(22 * trackViewRoot.ks); height: width; radius: width / 2
                        color: "#2C2C34"
                        Text { anchors.centerIn: parent; text: String(index + 1); color: trackViewRoot.cText; font.bold: true; font.pixelSize: trackViewRoot.fs(12) }
                    }
                    Text
                    {
                        width: guideCol.width - Math.round(32 * trackViewRoot.ks)
                        wrapMode: Text.WordWrap
                        textFormat: Text.StyledText
                        text: modelData
                        color: trackViewRoot.cMute
                        font.pixelSize: trackViewRoot.fs(13)
                    }
                }
            }
            // R465_BLT_PAGE: where to look - its logo on the taskbar (a shortcut, not the tray)
            Row
            {
                visible: !trackViewRoot.trackApp
                spacing: Math.round(10 * trackViewRoot.ks)
                Rectangle
                {
                    id: trayBox
                    width: Math.round(170 * trackViewRoot.ks); height: Math.round(34 * trackViewRoot.ks)
                    radius: Math.round(4 * trackViewRoot.ks)
                    color: "#0C0C0F"
                    Row
                    {
                        anchors.centerIn: parent
                        spacing: Math.round(10 * trackViewRoot.ks)
                        Repeater
                        {
                            model: 4
                            Item
                            {
                                width: Math.round(24 * trackViewRoot.ks); height: Math.round(28 * trackViewRoot.ks)
                                Rectangle { visible: index !== 2; anchors.centerIn: parent; width: Math.round(16 * trackViewRoot.ks); height: width
                                            radius: Math.round(3 * trackViewRoot.ks); color: "#2C2C34" }
                                Image { visible: index === 2; anchors.centerIn: parent; width: Math.round(22 * trackViewRoot.ks); height: width
                                        source: "blt_logo.png"; smooth: true; mipmap: true }
                                Rectangle { visible: index === 2; anchors.bottom: parent.bottom; anchors.horizontalCenter: parent.horizontalCenter
                                            width: Math.round(14 * trackViewRoot.ks); height: Math.max(2, Math.round(2 * trackViewRoot.ks)); color: trackViewRoot.cGold }
                            }
                        }
                    }
                }
                Text
                {
                    anchors.verticalCenter: parent.verticalCenter
                    width: guideCol.width - trayBox.width - Math.round(10 * trackViewRoot.ks)
                    wrapMode: Text.WordWrap
                    text: qsTr("the taskbar, bottom of the screen - this logo")
                    color: trackViewRoot.cDim
                    font.pixelSize: trackViewRoot.fs(12)
                }
            }
        }
    }

    // R443_PLAN_LANE: the next section's card - tap a name = in / out (the
    // RIG's plan), ENGINE DECIDES, FORGET FOR THIS TRACK. On the page, not in
    // the track card: on a small screen it reaches down over RIG and LOOK.
    Rectangle
    {
        id: planCard
        objectName: "planCard"
        visible: trackViewRoot.planShown && trackViewRoot.planCardOpen && !trackViewRoot.setupOpen && !trackViewRoot.helpOpen
        z: 50
        readonly property var nx: trackViewRoot.planCardSec()   // R484_PL_CARD: the next section or a later one
        readonly property real pad: Math.round(16 * trackViewRoot.ks)
        width: Math.min(trackViewRoot.width - 2 * pad, Math.round(680 * trackViewRoot.ks))
        // where the next section starts, under the lane (read again when the page moves)
        readonly property real moved: trackViewRoot.width + trackViewRoot.height + waveCard.x + waveCard.y + waveCard.height + planLaneBox.px + planLaneBox.vf
        x:
        {
            var dep = moved
            if (!nx) return pad
            var p = planLaneBox.mapToItem(trackViewRoot, (nx.beat - planLaneBox.vf) * planLaneBox.px, 0)
            return Math.max(pad, Math.min(trackViewRoot.width - width - pad, p.x))
        }
        y:
        {
            var dep = moved
            var p = planLaneBox.mapToItem(trackViewRoot, 0, planLaneBox.height)
            return Math.max(pad, Math.min(trackViewRoot.height - height - pad, p.y + Math.round(6 * trackViewRoot.ks)))
        }
        height: cardCol.implicitHeight + 2 * pad
        radius: Math.round(10 * trackViewRoot.ks)
        color: trackViewRoot.cCard
        border.width: 2
        border.color: "#E03030"
        // a press on the card reaches nothing under it (the page's one touch pattern)
        TouchInput { }
        Column
        {
            id: cardCol
            x: planCard.pad; y: planCard.pad
            width: planCard.width - 2 * planCard.pad
            spacing: Math.round(4 * trackViewRoot.ks)
            Text
            {
                objectName: "planCardTitle"
                width: parent.width
                elide: Text.ElideRight
                bottomPadding: Math.round(6 * trackViewRoot.ks)
                color: trackViewRoot.cMute
                font.bold: true
                font.pixelSize: trackViewRoot.fs(13)
                font.letterSpacing: trackViewRoot.fs(13) * 0.08
                text:
                {
                    var nx = planCard.nx
                    if (!nx) return ""
                    var who = nx.mode === "saved" ? (qsTr("SAVED LOOK") + " · " + (nx.savedName || "")) : nx.mode === "his" ? qsTr("YOURS") : (nx.mode === "mem" ? qsTr("REMEMBERED") : (nx.mode === "guess" ? qsTr("ENGINE'S GUESS") : qsTr("ENGINE'S PLAN")))
                    // R484_PL_TITLE: a later section names its bar
                    var what = nx.next ? qsTr("NEXT %1").arg(nx.type.toUpperCase()) : qsTr("%1 AT BAR %2").arg(nx.type.toUpperCase()).arg(trackViewRoot.barOf(nx.beat))
                    return what + "  \u00B7  " + who + "  \u00B7  " + qsTr("tap a name = in / out")
                }
            }
            Repeater
            {
                model: trackViewRoot.planCardRows
                Rectangle
                {
                    objectName: "planRow:" + modelData.key
                    width: cardCol.width
                    height: Math.max(40, Math.round(40 * trackViewRoot.ks))
                    radius: Math.round(6 * trackViewRoot.ks)
                    color: modelData["in"] ? "#22222A" : "#1A1A1E"
                    Rectangle { visible: modelData["in"] === true; width: Math.round(6 * trackViewRoot.ks); height: parent.height; color: "#E03030" }
                    Text
                    {
                        x: Math.round(18 * trackViewRoot.ks)
                        width: parent.width * 0.36
                        anchors.verticalCenter: parent.verticalCenter
                        elide: Text.ElideRight
                        text: modelData.key
                        color: modelData["in"] ? "#E9E9EE" : trackViewRoot.cDim
                        font.bold: true
                        font.pixelSize: trackViewRoot.fs(15)
                    }
                    Text
                    {
                        x: parent.width * 0.38
                        width: parent.width * 0.50
                        anchors.verticalCenter: parent.verticalCenter
                        elide: Text.ElideRight
                        text: modelData.note
                        color: trackViewRoot.cMute
                        font.pixelSize: trackViewRoot.fs(12)
                    }
                    Text
                    {
                        anchors.right: parent.right
                        anchors.rightMargin: Math.round(16 * trackViewRoot.ks)
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData["in"] ? qsTr("IN") : qsTr("OUT")
                        color: modelData["in"] ? "#F05050" : trackViewRoot.cDim
                        font.bold: true
                        font.pixelSize: trackViewRoot.fs(13)
                    }
                    TouchInput
                    {
                        onReleasedAt: (x, y, inside) => { if (inside) trackViewRoot.planToggle(modelData.key) }   // R484_PL_ROW_TAP
                    }
                }
            }
            Item { width: 1; height: Math.round(4 * trackViewRoot.ks) }
            Row
            {
                spacing: Math.round(10 * trackViewRoot.ks)
                Btn
                {
                    objectName: "planEngine"
                    width: Math.round(230 * trackViewRoot.ks); height: Math.max(44, Math.round(44 * trackViewRoot.ks))
                    fontPx: trackViewRoot.fs(12)
                    text: qsTr("ENGINE DECIDES")
                    enabled: planCard.nx !== null && (planCard.nx.mode === "his" || planCard.nx.mode === "mem" || planCard.nx.mode === "saved")
                    onTapped: trackViewRoot.planDecides()   // R484_PL_ENGINE_BTN: the next section's or the later one's
                }
                Btn
                {
                    objectName: "planForget"
                    width: Math.round(270 * trackViewRoot.ks); height: Math.max(44, Math.round(44 * trackViewRoot.ks))
                    fontPx: trackViewRoot.fs(12)
                    text: qsTr("FORGET FOR THIS TRACK")
                    onTapped: if (trackEngine) trackEngine.planForgetTrack()
                }
                Btn
                {
                    objectName: "planClose"
                    width: Math.round(120 * trackViewRoot.ks); height: Math.max(44, Math.round(44 * trackViewRoot.ks))
                    fontPx: trackViewRoot.fs(12)
                    text: qsTr("CLOSE")
                    onTapped: trackViewRoot.planCardOpen = false
                }
            }
        }
    }

    // =====================================================================
    //  SETUP: the whole page under the top bar - LIVE and the footer too
    //  (runde 393, Tobias 10-07: "setup og guide maa gerne daekke LIVE, de maa
    //  gerne begge to fylde hele siden"; v2 only - runde 253 kept LIVE free).
    //  The top bar stays: SHOW and the CLOSE on SETUP / HELP live there.
    // =====================================================================
    Loader
    {
        id: setupLoader
        z: 100
        onLoaded: if (item) item.host = trackViewRoot
        x: trackViewRoot.mx
        y: Math.round(14 * trackViewRoot.ks) + trackViewRoot.topH + trackViewRoot.gapB
        width: trackViewRoot.width - 2 * trackViewRoot.mx
        height: trackViewRoot.height - y - Math.round(6 * trackViewRoot.ks)
        visible: trackViewRoot.setupOpen && trackManager && trackManager.roleMode && !trackViewRoot.savedFavoritesOpen
        active: visible
        source: "qrc:/TrackSetup.qml"
        // R402_OVERLAY_IN: a fade and a small zoom on the way in (out at once - a
        // closed page must never take a finger)
        onVisibleChanged: if (visible && trackViewRoot.animate) setupIn.restart()
        ParallelAnimation
        {
            id: setupIn
            OpacityAnimator { target: setupLoader; from: 0; to: 1; duration: 200; easing.type: Easing.OutQuad }
            ScaleAnimator { target: setupLoader; from: 0.97; to: 1; duration: 200; easing.type: Easing.OutCubic }
        }
    }

    Rectangle
    {
        anchors.fill: setupLoader
        visible: setupLoader.visible && setupLoader.status === Loader.Error
        color: "#3A1A1A"
        radius: 4
        z: 101
        Text
        {
            anchors.fill: parent
            anchors.margins: 12
            wrapMode: Text.Wrap
            color: "#FFB0B0"
            font.pixelSize: 13
            text:
            {
                if (setupLoader.status !== Loader.Error)
                    return ""
                var c = Qt.createComponent("qrc:/TrackSetup.qml")
                return "TrackSetup.qml failed to load:\n\n" + (c.status === Component.Error ? c.errorString() : "(no detail)")
            }
        }
    }

    // =====================================================================
    //  R436_SHOW_CHECK (forslag 7, mock 7a - Tobias: "men med mulighed for
    //  stadig at gaa videre"): the show file is older than this build. What is
    //  missing, how to fix it, and CONTINUE ANYWAY - the show runs as it is.
    // =====================================================================
    Rectangle
    {
        id: oldShowDialog
        objectName: "oldShowDialog"
        z: 300
        anchors.fill: parent
        visible: trackViewRoot.oldShow && trackEngine && (!trackEngine.showAcked || trackViewRoot.oldShowOpen)
        color: Qt.rgba(0, 0, 0, 0.62)
        TouchInput { }                   // nothing under it takes a finger
        Rectangle
        {
            id: oldCard
            readonly property real p: Math.round(40 * trackViewRoot.ks)
            width: Math.min(parent.width - 2 * trackViewRoot.mx, Math.round(1100 * trackViewRoot.ks))
            height: oldCol.implicitHeight + 2 * p + Math.round(110 * trackViewRoot.ks)
            anchors.centerIn: parent
            radius: Math.round(14 * trackViewRoot.ks)
            color: trackViewRoot.cCard
            border.width: 3
            border.color: trackViewRoot.cGold
            Column
            {
                id: oldCol
                x: oldCard.p; y: oldCard.p
                width: oldCard.width - 2 * oldCard.p
                spacing: Math.round(16 * trackViewRoot.ks)
                Row
                {
                    spacing: Math.round(24 * trackViewRoot.ks)
                    Rectangle
                    {
                        width: Math.round(210 * trackViewRoot.ks); height: Math.round(48 * trackViewRoot.ks)
                        radius: Math.round(8 * trackViewRoot.ks); color: trackViewRoot.cGold
                        Text { anchors.centerIn: parent; text: qsTr("SHOW FILE"); color: "#1A1205"; font.bold: true; font.pixelSize: trackViewRoot.fs(20) }
                    }
                    Text
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("The show file is older than this build")
                        color: trackViewRoot.cText; font.bold: true; font.pixelSize: trackViewRoot.fs(26)
                    }
                }
                Text
                {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Made before runde %1 - this build expects runde %1 or newer.").arg(trackEngine ? trackEngine.showWants : 0)
                    color: trackViewRoot.cMute; font.pixelSize: trackViewRoot.fs(18)
                }
                Text { text: qsTr("MISSING"); color: trackViewRoot.cMute; font.bold: true; font.pixelSize: trackViewRoot.fs(14) }
                Repeater
                {
                    model: trackEngine && trackEngine.showMissing ? trackEngine.showMissing : []
                    Row
                    {
                        spacing: Math.round(16 * trackViewRoot.ks)
                        readonly property var md: modelData ? modelData : ({ group: "", text: "", family: "", have: 0, want: 0 })
                        Rectangle { width: Math.round(6 * trackViewRoot.ks); height: Math.round(44 * trackViewRoot.ks); color: trackViewRoot.cGold }
                        Column
                        {
                            spacing: Math.round(4 * trackViewRoot.ks)
                            Text { text: parent.parent.md.group !== "" ? parent.parent.md.group : parent.parent.md.family; color: trackViewRoot.cText; font.bold: true; font.pixelSize: trackViewRoot.fs(18) }
                            Text
                            {
                                width: oldCol.width - Math.round(30 * trackViewRoot.ks); wrapMode: Text.WordWrap
                                text: parent.parent.md.text
                                      + (trackViewRoot.oldShowMore ? "\n" + qsTr("%1 of %2 \"%3 …\" programmes in this file").arg(parent.parent.md.have).arg(parent.parent.md.want).arg(parent.parent.md.family) : "")
                                color: trackViewRoot.cMute; font.pixelSize: trackViewRoot.fs(15)
                            }
                        }
                    }
                }
                Text
                {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Everything else works. The show can run as it is.")
                    color: trackViewRoot.cText; font.pixelSize: trackViewRoot.fs(17)
                }
                Text
                {
                    width: parent.width; wrapMode: Text.WordWrap
                    text: qsTr("Fix: copy the new PSMAIN.qxw from qlcplus-pr to this PC and open it again.")
                    color: trackViewRoot.cMute; font.pixelSize: trackViewRoot.fs(16)
                }
            }
            Btn
            {
                objectName: "oldShowMore"
                x: oldCard.p; y: oldCard.height - oldCard.p - height
                width: (oldCard.width - 2 * oldCard.p) * 0.45; height: Math.round(70 * trackViewRoot.ks)
                text: trackViewRoot.oldShowMore ? qsTr("SHOW LESS") : qsTr("SHOW ME WHAT IS MISSING")
                fontPx: trackViewRoot.fs(17)
                onTapped: trackViewRoot.oldShowMore = !trackViewRoot.oldShowMore
            }
            Btn
            {
                objectName: "oldShowContinue"
                x: oldCard.width - oldCard.p - width; y: oldCard.height - oldCard.p - height
                width: (oldCard.width - 2 * oldCard.p) * 0.52; height: Math.round(70 * trackViewRoot.ks)
                text: qsTr("CONTINUE ANYWAY"); active: true; tone: trackViewRoot.cGold
                fontPx: trackViewRoot.fs(21)
                onTapped: { trackViewRoot.oldShowOpen = false; if (trackEngine) trackEngine.showContinue() }
            }
        }
    }

    // =====================================================================
    //  HELP (runde 393, Tobias 10-07: "en HELP der laver et overlay med guide
    //  til programmet"): each explanation lies on what it explains. Like SETUP
    //  it fills the page under the top bar. A tap anywhere closes it.
    // =====================================================================
    component HelpText: Text
    {
        color: "#E9E9EE"
        textFormat: Text.StyledText
        wrapMode: Text.WordWrap
        font.pixelSize: trackViewRoot.fs(15)
        lineHeight: 1.18
        verticalAlignment: Text.AlignVCenter
    }
    component HelpTitle: Text
    {
        color: "#E3B44F"
        font.bold: true
        font.pixelSize: trackViewRoot.fs(13)
        font.letterSpacing: (trackViewRoot.fs(13)) * 0.18
    }
    component HelpFrame: Rectangle
    {
        color: "transparent"
        radius: Math.round(14 * trackViewRoot.ks)
        border.width: 2
        border.color: Qt.rgba(0.89, 0.71, 0.31, 0.75)
    }

    Item
    {
        id: helpOverlay
        objectName: "helpOverlay"
        z: 99
        visible: trackViewRoot.helpOpen && trackManager && trackManager.roleMode
        // R402_OVERLAY_IN: as SETUP
        onVisibleChanged: if (visible && trackViewRoot.animate) helpIn.restart()
        ParallelAnimation
        {
            id: helpIn
            OpacityAnimator { target: helpOverlay; from: 0; to: 1; duration: 200; easing.type: Easing.OutQuad }
            ScaleAnimator { target: helpOverlay; from: 0.97; to: 1; duration: 200; easing.type: Easing.OutCubic }
        }
        x: setupLoader.x
        y: setupLoader.y
        width: setupLoader.width
        height: setupLoader.height
        // the page column under it: track, gap, deck, gap, footer
        readonly property real waveH: trackViewRoot.height - y - 2 * trackViewRoot.gapB - trackViewRoot.deckH - trackViewRoot.footH
        readonly property real deckY: waveH + trackViewRoot.gapB
        readonly property real pad: Math.round(22 * trackViewRoot.ks)
        readonly property real colW: (width - 2 * pad - 3 * pad) / 4
        function rowY(i) { return deckY + deck.rowY(i) }

        Rectangle { anchors.fill: parent; color: Qt.rgba(0.035, 0.035, 0.045, 0.95); radius: Math.round(14 * trackViewRoot.ks) }
        // a tap anywhere on the guide closes it
        TouchInput { onReleasedAt: (x, y, inside) => { if (inside) trackViewRoot.helpOpen = false } }

        // ---- the track, the top bar, the markers - and LIVE, beside it
        HelpFrame { x: 0; y: 0; width: parent.width; height: helpOverlay.waveH }
        Row
        {
            x: helpOverlay.pad; y: helpOverlay.pad
            spacing: helpOverlay.pad
            Repeater
            {
                model: [
                    // R455_HELP (Tobias 10-14: "Opdater nu ogsaa vores Help knap, til alt det nye")
                    { t: "TOP BAR", b: "<b>Section pill</b> - the section playing now; a white frame: pinned by hand.<br><b>Next</b> - the coming marker and how far away.<br><b>Four dots</b> - the beat in the bar; the first, white, is the downbeat.<br><b>START SHOW</b> starts the engine. <b>SHOW ON</b> stops on the second tap (SURE?).<br><b>SETUP</b> - groups, scenes and the rig.<br><b>TEST \u00B7 BPM</b> and <b>STOP TEST</b> - the test track is playing.<br><b>LAST TRACK</b> - from 10 min before closing: tap twice and this track ends the night (lasers out, a warm white grows to full white at the music's end or when the DJ's fader goes down; 5 min, haze down, then dark). Tap twice again to undo." },   // R479_LAST_WHITE_HELP
                    // R511_UI_HELP_FIT: at 1366 x 768 this column and MARKERS & THUMBS ran into the deck below (6 / 35 px)
                    // - said shorter (nothing on the page moves); + TYPE's drag (R502) said too
                    { t: "THE TRACK", b: "<b>Coloured bands</b> - the sections, with their energy.<br><b>White line</b> - where the track is; the bright bars are played.<br><b>The countdown</b> - bars to the next section.<br><b>HIT</b> on a DROP flag - a hit is armed for that drop (<b>DROP HIT</b> in LIVE).<br>Warnings appear in the bar under the track." },
                    { t: "MARKERS & THUMBS", b: "<b>MARKERS</b> opens the marker tools - <b>DONE</b> closes them.<br><b>+ TYPE</b>: a tap = a marker on the bar playing; dragged onto the track = the bar under your finger. Its tab under the track: tap = <b>RETYPE</b> / <b>DELETE</b>, drag = move. <b>UNDO</b> steps back.<br><b>Thumbs</b> rate this moment (up also saves the look); hold one to rate a single group. <b>SAVED</b>: recent looks - <b>NEXT TYPE</b> plays one in the next such section, <b>QUEUED</b> cancels, <b>ALL SAVED LOOKS</b> is the full list.<br><b>PLAN</b> shows every section's lamps under the section names: grey = ran, colour = on now, outlined = the engine's plan (thin: a guess), a red line = yours, <b>MEM</b> = yours last time. Tap the <b>next</b> section or a later one to pick its lamps (the next = RIG's plan) - not once it has begun." },   // R484_PL_HELP
                    { t: "NO TRACK & TOUCH", b: "<b>No track</b> - the track says whether Beat Link Trigger is connected; <b>(i)</b> shows how to start it from its logo on the taskbar.<br><b>TEST</b> (bottom left of the empty track, <b>hold 1.5 s</b>) - with no track from the CDJs: a made-up track on a loop to try the page and the rig. Nothing is learned or logged; a real track takes over.<br>Several fingers work at once - hold <b>FLASH WHITE</b> while you pull <b>ENERGY</b>.<br><b>HELP</b> and <b>SETUP</b> close with the same button; a tap on the guide closes it too." }
                ]
                Column
                {
                    width: helpOverlay.colW
                    spacing: Math.round(10 * trackViewRoot.ks)
                    HelpTitle { text: modelData.t }
                    HelpText { width: parent.width; text: modelData.b; verticalAlignment: Text.AlignTop }
                }
            }
        }

        // ---- RIG: one line on each kind of row
        HelpFrame { x: 0; y: helpOverlay.deckY; width: trackViewRoot.rigW; height: trackViewRoot.deckH }
        HelpTitle { x: rigCard.inX; y: helpOverlay.deckY + trackViewRoot.cardPad; text: "RIG     ·     WHAT IS IN THE SHOW"; width: rigCard.inW; elide: Text.ElideRight }
        HelpText
        {
            x: rigCard.inX; y: helpOverlay.rowY(0); width: rigCard.inW; height: trackViewRoot.rowH
            text: "<b>MASTER DIMMER</b> - the ceiling for the whole rig."
        }
        HelpText
        {
            x: rigCard.inX; y: helpOverlay.rowY(1); width: rigCard.inW
            height: 5 * trackViewRoot.rowH + 4 * trackViewRoot.g
            verticalAlignment: Text.AlignTop
            text: "<b>Toggle</b> - the group is in or out of tonight's show.<br><br><b>Fader</b> - the group's own ceiling; kept through a restart the same night (until noon).<br><br><b>The fader lights up</b> in the room's colour while the group is on stage; grey = in the show, not used right now.<br><br><b>The icon</b> is the group's flash: hold it = that group full white (lasers in the move they are in).<br><br><b>Tap a name</b> - in or out of the next section: <font color='#F05050'>▶ NEXT BREAK</font> (BUILD, DROP ...) is the engine's plan; a tap makes it yours for that whole section. The base has a toggle too.<br><br><b>ON / OFF ONLY</b> - a group that can only be switched."
        }

        // ---- LOOK: the explanation on each row, after its label
        HelpFrame { x: lookCard.x; y: helpOverlay.deckY; width: lookCard.width; height: trackViewRoot.deckH }
        HelpTitle
        {
            x: lookCard.x + lookCard.inX; y: helpOverlay.deckY + trackViewRoot.cardPad
            text: "LOOK     ·     GREEN AUTO = THE ENGINE DECIDES"
            width: lookCard.inW
            elide: Text.ElideRight
        }
        Repeater
        {
            model: [
                { l: "ENERGY",       b: "How hard the show plays - your hand takes over. <b>AUTO</b>: the clock moves it through the night. <b>ALL AUTO</b> (above, when one is off) puts them all back, including HAZE & FAN." },
                { l: "SECTION",      b: "Force a section until the music reaches its next one - then it follows the music again. The one the music is in is filled. Tap it again or <b>AUTO</b> to let go now." },   // R429_SECTION_ONCE
                { l: "POSITION",     b: "Hold the heads in one position. <b>AUTO</b>: the engine moves them." },
                { l: "COLOUR",       b: "Tap colours to make your own mix - it holds until the next section, then <b>AUTO</b> again. <b>Filled</b>: in the mix. <b>Ring</b>: leading now. <b>AUTO</b>: the engine picks." },   // R512_TILES_HELP
                { l: "COLOUR MODE",  b: "<b>FADE</b>: the mix glides over. <b>CHASE</b>: it steps on the kick. <b>SPEED</b>: the pace of moves and chases." },
                { l: "HAZE MACHINE", b: "Haze and fan by hand. <b>HAZE</b> is elastic above HAZE LIMIT (SETUP, ADVANCED): hold above the thin line for extra haze, then release to return to the limit. <b>AUTO</b>: haze and fan follow HAZE & FAN BY CLOCK (SETUP, ADVANCED) - learned from your hand; your hand on either takes them back." }   // R484_HZ_HELP
            ]
            Item
            {
                x: lookCard.x + lookCard.inX
                y: helpOverlay.rowY(index)
                width: lookCard.inW
                height: trackViewRoot.rowH
                HelpTitle
                {
                    width: lookCard.labW + lookCard.colGap + lookCard.autoW
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.l
                }
                HelpText
                {
                    x: lookCard.cX - lookCard.inX
                    width: parent.width - x
                    height: parent.height
                    text: modelData.b
                }
            }
        }

        // ---- LIVE: on each button
        HelpFrame { x: liveCard.x; y: helpOverlay.deckY; width: liveCard.width; height: trackViewRoot.deckH }
        HelpTitle { x: liveCard.x + liveCard.inX; y: helpOverlay.deckY + trackViewRoot.cardPad; text: "LIVE     ·     RIGHT NOW"; width: liveCard.inW; elide: Text.ElideRight }
        Repeater
        {
            model: [
                { y: 0, h: 1, b: "<b>NEXT</b> - a new look now. <b>UNDO</b> brings the previous look back in this section." },
                { y: 1, h: 1, b: "<b>HOLD</b> - freeze the look until you tap it again." },
                { y: 2, h: 1, b: "<b>CALM</b> - calmer for 16 bars; tap again to end it." },
                { y: 3, h: 2, b: "<b>STUTTER</b> - dark on every 1/4, 1/8 or 1/16 while held; a quick tap locks it.  <b>BLACKOUT</b> - dark while held. Slide off to lock; tap to release." },
                { y: 4, h: 2, b: "<b>DROP HIT</b> arms a hit on the next drop: a tap = the gold kind, or <b>FLSH</b> / <b>STRB</b> / <b>DARK</b>. Once, then off.  <b>FLASH WHITE</b> - white while held." }
            ]
            HelpText
            {
                objectName: "liveHelp:" + modelData.y        // R455_HELP
                x: liveCard.x + liveCard.inX
                width: liveCard.inW
                y: modelData.y < 4 ? helpOverlay.rowY(modelData.y) : helpOverlay.rowY(3) + liveCard.bigH + trackViewRoot.g
                height: modelData.h === 1 ? trackViewRoot.rowH : liveCard.bigH
                text: modelData.b
            }
        }
    }

    // the classic slot setup, for a show without roles
    Rectangle
    {
        anchors.fill: setupLoader
        z: 100
        visible: trackViewRoot.setupOpen && trackManager && !trackManager.roleMode
        color: trackViewRoot.cCard
        radius: 8
        Flickable
        {
            anchors.fill: parent
            anchors.margins: 8
            contentHeight: setupCol.height
            clip: true

            Column
            {
                id: setupCol
                width: parent.width
                spacing: 5

                Row
                {
                    spacing: 8
                    Item { width: 130; height: 26 }
                    Repeater
                    {
                        model: trackViewRoot.states
                        Text { width: 190; text: modelData.toUpperCase(); color: trackViewRoot.markerColor(modelData); font.bold: true; font.pixelSize: 14 }
                    }
                    Text { width: 150; text: qsTr("Folder"); color: trackViewRoot.cMute; font.pixelSize: 14 }
                    Text { width: 40; text: qsTr("spd"); color: trackViewRoot.cMute; font.pixelSize: 14 }
                }

                Repeater
                {
                    model: trackManager ? trackManager.slotCount : 0
                    Row
                    {
                        property int slotIndex: index
                        spacing: 8
                        Text
                        {
                            width: 130; height: 34
                            verticalAlignment: Text.AlignVCenter
                            text: trackManager ? trackManager.slotName(slotIndex) : ""
                            color: trackViewRoot.cText
                            font.pixelSize: 15
                        }
                        Repeater
                        {
                            model: trackViewRoot.states
                            Row
                            {
                                property string stateName: modelData
                                width: 190
                                spacing: 4
                                CheckBox
                                {
                                    id: rndBox
                                    width: 32; height: 34
                                    checked: trackManager ? trackManager.lookRandom(stateName, parent.parent.slotIndex) : false
                                    onToggled: trackManager.setLookRandom(stateName, parent.parent.slotIndex, checked)
                                }
                                ComboBox
                                {
                                    width: 150; height: 34
                                    enabled: !rndBox.checked
                                    model: trackManager ? trackManager.slotFunctions(parent.parent.slotIndex) : []
                                    textRole: "name"
                                    Component.onCompleted:
                                    {
                                        if (!trackManager) return
                                        var fid = trackManager.lookFunction(parent.stateName, parent.parent.slotIndex)
                                        for (var i = 0; i < model.length; i++)
                                            if (model[i].id === fid) { currentIndex = i; return }
                                        currentIndex = -1
                                    }
                                    onActivated:
                                    {
                                        var e = model[currentIndex]
                                        if (e !== undefined)
                                            trackManager.setLookFunction(parent.stateName, parent.parent.slotIndex, e.id)
                                    }
                                }
                            }
                        }
                        ComboBox
                        {
                            width: 150; height: 34
                            model: trackManager ? trackManager.folderList() : []
                            textRole: "name"
                            Component.onCompleted:
                            {
                                if (!trackManager) return
                                var f = trackManager.slotFolder(parent.slotIndex)
                                for (var i = 0; i < model.length; i++)
                                    if (model[i].path === f) { currentIndex = i; return }
                                currentIndex = 0
                            }
                            onActivated:
                            {
                                var e = model[currentIndex]
                                if (e !== undefined)
                                    trackManager.setSlotFolder(parent.slotIndex, e.path)
                            }
                        }
                        CheckBox
                        {
                            width: 40; height: 34
                            checked: trackManager ? trackManager.slotFollowsSpeed(parent.slotIndex) : false
                            onToggled: trackManager.setSlotFollowsSpeed(parent.slotIndex, checked)
                        }
                    }
                }

                Row
                {
                    spacing: 8
                    Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("BPM range") + ":"; color: trackViewRoot.cMute; font.pixelSize: 14 }
                    SpinBox { height: 34; from: 40; to: 300; value: trackManager ? trackManager.bpmLow : 80; onValueModified: trackManager.bpmLow = value }
                    SpinBox { height: 34; from: 40; to: 300; value: trackManager ? trackManager.bpmHigh : 140; onValueModified: trackManager.bpmHigh = value }
                    Text { anchors.verticalCenter: parent.verticalCenter; text: "    " + qsTr("Quantize") + ":"; color: trackViewRoot.cMute; font.pixelSize: 14 }
                    ComboBox
                    {
                        width: 90; height: 34
                        model: [ 1, 2, 4, 8, 16, 32 ]
                        currentIndex:
                        {
                            var q = trackManager ? trackManager.quantize : 1
                            var opts = [ 1, 2, 4, 8, 16, 32 ]
                            var idx = opts.indexOf(q)
                            return idx < 0 ? 0 : idx
                        }
                        onActivated: trackManager.quantize = model[currentIndex]
                    }
                }

                Text
                {
                    text: qsTr("Running") + ": " + (trackManager ? trackManager.runningLook : "")
                    color: trackViewRoot.cMute
                    font.pixelSize: 14
                }
            }
        }
    }

    // ================================================= R486_MI_PAGE: MIDI LEARN
    // Tobias 10-20/21 ("midi mapping til faders og knapper", mockups 2-5 approved 10-21):
    // LEARN dims the page, outlines every control a controller can drive (their
    // objectNames are the manager's targets) and covers the top bar with the banner -
    // a tap picks a control, nothing on the page acts. Outside LEARN a mapped control
    // carries a small MIDI tag, and a fader the controller has not taken over shows
    // where the controller is. The manager does the rest (track_runtime/midimap.inc.cpp).
    readonly property bool midiLearnOn: trackManager ? trackManager.midiLearn === true : false
    property var midiRects: []
    function midiScan()
    {
        if (!trackManager || !midiLayer.wanted) { midiRects = []; return }
        var targets = trackManager.midiTargets()
        var maps = trackManager.midiMappings || []
        var want = ({})
        for (var i = 0; i < targets.length; i++) want[targets[i].t] = targets[i]
        var mapped = ({})
        for (var j = 0; j < maps.length; j++) mapped[maps[j].t] = maps[j]
        var out = []
        var walk = function(it)
        {
            if (!it || it === midiLayer || it === setupLoader || !it.visible) return
            var n = it.objectName
            if (n !== "" && want[n] !== undefined && it.width > 0 && it.height > 0)
            {
                var p = it.mapToItem(midiLayer, 0, 0)
                var m = mapped[n]
                out.push({ t: n, x: p.x, y: p.y, w: it.width, h: it.height, fader: want[n].kind === 0,
                           tag: m ? m.tag : "", mapped: m !== undefined,
                           at: m && m.fader && !m.taken ? m.at : -1 })
            }
            var c = it.children
            for (var k = 0; k < c.length; k++) walk(c[k])
        }
        walk(trackViewRoot)
        midiRects = out
    }
    function midiPick(x, y)
    {
        // the smallest outline under the finger (DROP HIT's chips sit inside DROP HIT)
        var best = null
        for (var i = 0; i < midiRects.length; i++)
        {
            var r = midiRects[i]
            if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h && (!best || r.w * r.h < best.w * best.h))
                best = r
        }
        if (best) trackManager.midiSelect(best.t)
        else trackManager.midiCancel()
    }
    Timer { id: midiScanLater; interval: 60; onTriggered: trackViewRoot.midiScan() }
    // the outlines follow the page: its size, the manager's list, SETUP / HELP, and every 3 s while shown
    Timer { interval: 3000; repeat: true; running: midiLayer.wanted; onTriggered: trackViewRoot.midiScan() }
    Connections
    {
        target: trackManager
        ignoreUnknownSignals: true
        function onMidiChanged() { midiScanLater.restart() }
    }
    Connections
    {
        target: trackViewRoot
        // R486_MI_LEAVE: SETUP or HELP opening ends LEARN
        function onSetupOpenChanged() { if (trackViewRoot.setupOpen && trackViewRoot.midiLearnOn) trackManager.midiLearnStop(); midiScanLater.restart() }
        function onHelpOpenChanged() { if (trackViewRoot.helpOpen && trackViewRoot.midiLearnOn) trackManager.midiLearnStop(); midiScanLater.restart() }
        function onMidiLearnOnChanged() { midiScanLater.restart() }
        function onWidthChanged() { midiScanLater.restart() }
        function onHeightChanged() { midiScanLater.restart() }
    }
    Item
    {
        id: midiLayer
        objectName: "midiLayer"
        anchors.fill: parent
        z: 950
        readonly property bool learning: trackViewRoot.midiLearnOn
        readonly property bool tagsOn: !learning && trackManager && (trackManager.midiMappings || []).length > 0
                                       && !trackViewRoot.setupOpen && !trackViewRoot.helpOpen && !trackViewRoot.planCardOpen
        readonly property bool wanted: learning || tagsOn
        readonly property var banner: trackManager && trackManager.midiBanner ? trackManager.midiBanner : ({})
        readonly property string sel: trackManager && trackManager.midiLearnTarget ? trackManager.midiLearnTarget : ""
        readonly property var selRect:
        {
            for (var i = 0; i < trackViewRoot.midiRects.length; i++)
                if (trackViewRoot.midiRects[i].t === sel) return trackViewRoot.midiRects[i]
            return null
        }
        readonly property real dim: !learning ? 0 : (selRect ? 0.6 : 0.35)
        readonly property real bannerH: Math.round(14 * trackViewRoot.ks) + trackViewRoot.topH
        visible: wanted
        onWantedChanged: midiScanLater.restart()

        // LEARN: nothing under it takes a finger or a mouse - a tap picks a control
        TouchInput
        {
            objectName: "midiCatch"
            enabled: midiLayer.learning
            onReleasedAt: (x, y, inside) => { if (inside) trackViewRoot.midiPick(x, y) }
        }
        // the dim, with a hole for the picked control
        Rectangle { visible: midiLayer.dim > 0 && !midiLayer.selRect; anchors.fill: parent; color: "#000000"; opacity: midiLayer.dim }
        Repeater
        {
            model: midiLayer.dim > 0 && midiLayer.selRect ? 4 : 0
            Rectangle
            {
                readonly property var s: midiLayer.selRect
                color: "#000000"; opacity: midiLayer.dim
                x: index === 1 ? s.x + s.w + 3 : 0
                y: index === 0 ? 0 : index === 3 ? s.y + s.h + 3 : s.y - 3
                width: index === 0 || index === 3 ? parent.width : index === 1 ? Math.max(0, parent.width - s.x - s.w - 3) : Math.max(0, s.x - 3)
                height: index === 0 ? Math.max(0, s.y - 3) : index === 3 ? Math.max(0, parent.height - s.y - s.h - 3) : s.h + 6
            }
        }
        // outlines (LEARN) and tags (both)
        Repeater
        {
            model: trackViewRoot.midiRects
            Item
            {
                objectName: "midiMark:" + modelData.t
                readonly property bool picked: modelData.t === midiLayer.sel
                readonly property bool learned: midiLayer.banner.kind === 2 && midiLayer.banner.t === modelData.t
                readonly property color c: picked ? "#E3B44F" : learned ? "#6ECD82" : "#4FA3E3"
                readonly property string tagText: midiLayer.learning ? modelData.tag : (modelData.mapped ? "MIDI" : "")
                visible: midiLayer.learning || modelData.mapped
                x: modelData.x - 3; y: modelData.y - 3; width: modelData.w + 6; height: modelData.h + 6
                opacity: midiLayer.learning && midiLayer.sel !== "" && !picked ? 0.35 : 1
                Rectangle
                {
                    visible: midiLayer.learning
                    anchors.fill: parent; radius: Math.round(6 * trackViewRoot.ks); color: "transparent"
                    border.width: picked || learned ? 3 : 1.5; border.color: parent.c
                }
                Rectangle
                {
                    objectName: "midiTag:" + modelData.t
                    readonly property bool quiet: !midiLayer.learning
                    readonly property bool side: modelData.t === "allAuto"
                    readonly property bool below: modelData.t.indexOf("hitKind:") === 0
                    visible: parent.tagText !== ""
                    x: side ? parent.width + 4 : parent.width - width + 2
                    y: side ? (parent.height - height) / 2 : below ? parent.height - 2 : -height / 2 - 1
                    width: tagT.implicitWidth + trackViewRoot.fs(8); height: tagT.implicitHeight + trackViewRoot.fs(2)
                    radius: 3
                    color: quiet ? "#14324F" : parent.c
                    border.width: quiet ? 1 : 0; border.color: "#4FA3E3"
                    Text
                    {
                        id: tagT
                        anchors.centerIn: parent
                        text: parent.parent.tagText
                        color: parent.quiet ? "#8CC4F0" : "#0A1622"
                        font.bold: true; font.pixelSize: trackViewRoot.fs(9); font.letterSpacing: 0.5
                    }
                }
                // a fader the controller has not taken over: where the controller is
                Item
                {
                    objectName: "midiGhost:" + modelData.t
                    visible: !midiLayer.learning && modelData.fader && modelData.at >= 0
                    readonly property real gx: 3 + 3 + (modelData.w - 6) * modelData.at / 100
                    x: gx; y: 3; width: 1; height: modelData.h
                    Rectangle { x: -1; width: 3; height: parent.height; color: "#4FA3E3" }
                    Rectangle
                    {
                        readonly property bool after: parent.gx < width + 12
                        x: after ? 6 : -width - 6; y: (parent.height - height) / 2
                        width: gT.implicitWidth + trackViewRoot.fs(10); height: gT.implicitHeight + trackViewRoot.fs(4); radius: 3
                        color: "#4FA3E3"
                        Text { id: gT; anchors.centerIn: parent; text: "MIDI " + modelData.at + " %"; color: "#0A1622"; font.bold: true; font.pixelSize: trackViewRoot.fs(10) }
                    }
                }
            }
        }
        // the banner over the top bar (SHOW / LAST TRACK cannot be hit while learning)
        Rectangle
        {
            objectName: "midiBanner"
            visible: midiLayer.learning
            x: 0; y: 0; width: parent.width; height: midiLayer.bannerH
            readonly property int kind: midiLayer.banner.kind !== undefined ? midiLayer.banner.kind : 0
            color: kind === 2 ? "#16331F" : (kind === 1 || kind === 3) ? "#33280F" : "#14324F"
            TouchInput { }                       // the top bar under it takes nothing
            Rectangle
            {
                id: midiChip
                x: trackViewRoot.fs(14); anchors.verticalCenter: parent.verticalCenter
                width: midiChipT.implicitWidth + trackViewRoot.fs(22); height: trackViewRoot.fs(32); radius: 4
                color: parent.kind === 2 ? "#6ECD82" : (parent.kind === 1 || parent.kind === 3) ? "#E3B44F" : "#4FA3E3"
                Text { id: midiChipT; anchors.centerIn: parent; text: parent.parent.kind === 2 ? qsTr("LEARNED") : qsTr("MIDI LEARN"); color: "#0A1622"; font.bold: true; font.pixelSize: trackViewRoot.fs(14); font.letterSpacing: 1.5 }
            }
            Column
            {
                anchors.left: midiChip.right; anchors.leftMargin: trackViewRoot.fs(18); anchors.verticalCenter: parent.verticalCenter
                width: midiBtns.x - x - trackViewRoot.fs(16)
                spacing: trackViewRoot.fs(3)
                Text { objectName: "midiBannerText"; width: parent.width; elide: Text.ElideRight; text: midiLayer.banner.text || ""; color: "#FFFFFF"; font.bold: true; font.pixelSize: trackViewRoot.fs(18) }
                Text { width: parent.width; elide: Text.ElideRight; visible: text !== ""; text: midiLayer.banner.sub || ""; color: "#A9C3DA"; font.pixelSize: trackViewRoot.fs(13) }
            }
            Row
            {
                id: midiBtns
                anchors.right: parent.right; anchors.rightMargin: trackViewRoot.fs(14); anchors.verticalCenter: parent.verticalCenter
                spacing: trackViewRoot.fs(10)
                Repeater
                {
                    model: midiLayer.sel !== "" ? [ { t: qsTr("CANCEL"), n: "midiCancel", primary: false } ]
                         : (trackManager && trackManager.midiCanUndo ? [ { t: qsTr("UNDO"), n: "midiUndo", primary: false }, { t: qsTr("DONE"), n: "midiDone", primary: true } ]
                                                                    : [ { t: qsTr("DONE"), n: "midiDone", primary: true } ])
                    Rectangle
                    {
                        objectName: modelData.n
                        width: Math.max(trackViewRoot.fs(120), bT.implicitWidth + trackViewRoot.fs(36)); height: trackViewRoot.fs(48); radius: 6
                        color: modelData.primary ? "#FFFFFF" : "transparent"
                        border.width: modelData.primary ? 0 : 1.5; border.color: "#A9C3DA"
                        Text { id: bT; anchors.centerIn: parent; text: modelData.t; color: modelData.primary ? "#0A1622" : "#FFFFFF"; font.bold: true; font.pixelSize: trackViewRoot.fs(14); font.letterSpacing: 1.5 }
                        TouchInput
                        {
                            onReleasedAt: (x, y, inside) =>
                            {
                                if (!inside || !trackManager) return
                                if (modelData.n === "midiCancel") trackManager.midiCancel()
                                else if (modelData.n === "midiUndo") trackManager.midiUndo()
                                else trackManager.midiLearnStop()
                            }
                        }
                    }
                }
            }
        }
    }

    // SAVED_LOOKS: approved UI.
    Rectangle
    {
        id: savedPanel
        objectName: "savedLooksPanel"
        visible: trackViewRoot.savedFavoritesOpen && trackViewRoot.setupOpen
        z: 900
        x: trackViewRoot.mx; y: trackViewRoot.topH + trackViewRoot.g
        width: parent.width - 2 * x; height: parent.height - y
        color: trackViewRoot.cCard; radius: Math.round(12 * trackViewRoot.ks)
        MouseArea { anchors.fill: parent }
        readonly property real pad: trackViewRoot.cardPad + 6
        Text
        {
            x: savedPanel.pad; y: savedPanel.pad
            text: "SETUP / SAVED LOOKS"; color: trackViewRoot.cMute
            font.pixelSize: trackViewRoot.fs(12); font.bold: true; font.letterSpacing: 1
        }
        Text
        {
            x: savedPanel.pad; y: savedPanel.pad + 36 * trackViewRoot.ks
            text: "Your saved looks"; color: trackViewRoot.cText
            font.pixelSize: trackViewRoot.fs(30); font.bold: true
        }
        Text
        {
            x: savedPanel.pad; y: savedPanel.pad + 86 * trackViewRoot.ks
            width: savedPanel.width - 2 * savedPanel.pad
            text: "Tap thumbs up while a look plays to save it here."
            color: trackViewRoot.cMute; font.pixelSize: trackViewRoot.fs(15)
        }
        Btn
        {
            x: parent.width - savedPanel.pad - width; y: savedPanel.pad + 32 * trackViewRoot.ks
            width: 136 * trackViewRoot.kw; height: trackViewRoot.rowH
            text: "BACK"; icon: "revert"
            onTapped: trackViewRoot.savedFavoritesOpen = false
        }
        Text {
            x: savedPanel.pad; y: savedPanel.pad + 150 * trackViewRoot.ks
            visible: trackViewRoot.savedRows.length === 0
            text: "No saved looks yet. Tap thumbs up while a look plays."
            color: trackViewRoot.cMute; font.pixelSize: trackViewRoot.fs(15)
        }
        Flickable
        {
            objectName: "savedLooksScroll"
            x: savedPanel.pad; y: savedPanel.pad + 145 * trackViewRoot.ks
            width: savedPanel.width - 2 * savedPanel.pad
            height: Math.max(60, savedPanel.height - y - savedPanel.pad - 48 * trackViewRoot.ks)
            contentWidth: width; contentHeight: savedColumn.height; clip: true
            boundsBehavior: Flickable.StopAtBounds; flickableDirection: Flickable.VerticalFlick
            Column {
                id: savedColumn
                width: parent.width; spacing: trackViewRoot.g
            Repeater
            {
                model: trackViewRoot.savedRows
                Rectangle
                {
                    width: parent.width; height: 126 * trackViewRoot.ks
                    color: trackViewRoot.cBtn; radius: Math.round(10 * trackViewRoot.ks)
                    Rectangle { x: 20 * trackViewRoot.ks; y: 24 * trackViewRoot.ks; width: 8 * trackViewRoot.ks; height: parent.height - 48 * trackViewRoot.ks; radius: 3; color: modelData.colour === "red" ? "#E03030" : (modelData.colour === "blue" ? "#3060E0" : trackViewRoot.cBlue) }
                    Text { x: 48 * trackViewRoot.ks; y: 19 * trackViewRoot.ks; width: parent.width - x - 310 * trackViewRoot.kw; elide: Text.ElideRight; text: modelData.name; color: trackViewRoot.cText; font.bold: true; font.pixelSize: trackViewRoot.fs(19) }
                    Rectangle
                    {
                        objectName: "savedSection:" + modelData.section.toUpperCase()
                        x: 48 * trackViewRoot.ks; y: 49 * trackViewRoot.ks
                        width: 104 * trackViewRoot.ks; height: 25 * trackViewRoot.ks
                        radius: 4; color: modelData.section.toUpperCase() === "DROP" ? "#402126" : (modelData.section.toUpperCase() === "BREAK" ? "#183148" : "#303039")
                        Text { anchors.centerIn: parent; text: modelData.section.toUpperCase(); color: modelData.section.toUpperCase() === "DROP" ? "#FF9299" : (modelData.section.toUpperCase() === "BREAK" ? "#7EC5FF" : "#C0C0CE"); font.bold: true; font.pixelSize: trackViewRoot.fs(12) }
                    }
                    Text { x: 166 * trackViewRoot.ks; y: 53 * trackViewRoot.ks; width: parent.width - x - 310 * trackViewRoot.kw; elide: Text.ElideRight; text: modelData.title; color: trackViewRoot.cMute; font.pixelSize: trackViewRoot.fs(13) }
                    Text { x: 48 * trackViewRoot.ks; y: 82 * trackViewRoot.ks; width: parent.width - x - 310 * trackViewRoot.kw; elide: Text.ElideRight; text: modelData.groups ? modelData.groups.join(" · ") : ""; color: "#C0C0C8"; font.pixelSize: trackViewRoot.fs(13) }
                    Btn
                    {
                        x: parent.width - width - 22 * trackViewRoot.ks; anchors.verticalCenter: parent.verticalCenter
                        width: 270 * trackViewRoot.kw; height: trackViewRoot.rowH
                        active: modelData.queued === true
                        enabled: modelData.canQueue === true
                        text: active ? "QUEUED · " + modelData.section.toUpperCase()
                                     : (enabled ? "USE NEXT " + modelData.section.toUpperCase() : modelData.reason || ("NO NEXT " + modelData.section.toUpperCase()))
                        icon: active ? "check" : "play"; tone: trackViewRoot.cGreen
                        touchInputEnabled: false
                        TapHandler {
                            gesturePolicy: TapHandler.DragThreshold
                            onTapped: if (trackEngine) trackEngine.queueSavedLook(modelData.id)
                        }
                    }
                }
            }
        }
        }
        Text
        {
            x: savedPanel.pad; anchors.bottom: parent.bottom; anchors.bottomMargin: savedPanel.pad
            text: "Starts at the next matching section, then returns to AUTO. ENERGY stays where it is."
            color: trackViewRoot.cMute; font.pixelSize: trackViewRoot.fs(14)
        }
    }

    // SAVED_LOOKS: approved UI.
    MouseArea
    {
        id: recentSavedDismiss
        objectName: "recentSavedDismiss"
        x: 0; y: 0; height: parent.height; z: 700
        width: liveLeft                                    // TRACK_APP_LIVEFREE_R576: LIVE stays free
        property real liveLeft: parent.width
        onVisibleChanged: if (visible) liveLeft = trackViewRoot.liveLeftNow()
        visible: trackViewRoot.savedRecentOpen && !trackViewRoot.setupOpen && !trackViewRoot.helpOpen
        onClicked: trackViewRoot.savedRecentOpen = false
    }
    Rectangle
    {
        id: recentPopup
        objectName: "recentSavedPopup"
        z: 701
        visible: trackViewRoot.savedRecentOpen && !trackViewRoot.setupOpen && !trackViewRoot.helpOpen
        width: Math.min(parent.width - 2 * trackViewRoot.mx, Math.max(340, 540 * trackViewRoot.kw))
        readonly property real pad: Math.max(12, 16 * trackViewRoot.ks)
        readonly property real itemH: Math.max(trackViewRoot.touchH, 70 * trackViewRoot.ks)
        readonly property real titleH: Math.max(32, 36 * trackViewRoot.ks)
        readonly property real footH: Math.max(44, 48 * trackViewRoot.ks)
        height: 2 * pad + titleH + (trackViewRoot.savedRecentEmpty ? 70 : Math.min(3, trackViewRoot.savedRecentRows.length) * itemH + Math.max(0,Math.min(3,trackViewRoot.savedRecentRows.length)-1) * trackViewRoot.g) + trackViewRoot.g + footH
        property point origin: Qt.point(0, 0)
        function align() { if (visible) origin = recentSavedButton.mapToItem(trackViewRoot, 0, recentSavedButton.height) }
        onVisibleChanged: if (visible) Qt.callLater(align)
        Connections {
            target: trackViewRoot
            function onWidthChanged() { Qt.callLater(recentPopup.align) }
            function onHeightChanged() { Qt.callLater(recentPopup.align) }
        }
        Connections { target: rail; function onYChanged() { Qt.callLater(recentPopup.align) } }
        x: Math.max(trackViewRoot.mx, Math.min(parent.width - width - trackViewRoot.mx, origin.x))
        y: Math.min(parent.height - height - trackViewRoot.mx, origin.y + trackViewRoot.g)
        color: "#17171D"; radius: Math.round(10 * trackViewRoot.ks)
        focus: visible
        Keys.onEscapePressed: trackViewRoot.savedRecentOpen = false
        MouseArea { anchors.fill: parent }
        Text
        {
            x: recentPopup.pad; y: recentPopup.pad + 5
            text: "RECENT SAVED LOOKS"; color: trackViewRoot.cText
            font.pixelSize: trackViewRoot.fs(14); font.bold: true
        }
        // TRACK_APP_FIX_R595: the x is 38 x 32 px at every scale - the whole corner of the title row closes too
        Item
        {
            x: parent.width - width; y: 0
            width: Math.max(72, Math.round(72 * trackViewRoot.ks))
            height: recentPopup.pad + recentPopup.titleH
            TouchInput { onReleasedAt: (x, y, inside) => { if (inside) trackViewRoot.savedRecentOpen = false } }
        }
        Btn
        {
            objectName: "recentSavedClose"
            x: parent.width - width - recentPopup.pad; y: recentPopup.pad - 3
            width: 38; height: 32; text: "×"; fontPx: 20
            onTapped: trackViewRoot.savedRecentOpen = false
        }
        Text
        {
            visible: trackViewRoot.savedRecentEmpty
            x: recentPopup.pad; y: recentPopup.pad + recentPopup.titleH + 12
            width: parent.width - 2 * recentPopup.pad
            text: "No saved looks yet. Tap thumbs up while a look plays."; color: trackViewRoot.cMute   // R499_SAVED_EMPTY_TEXT
            font.pixelSize: trackViewRoot.fs(14)
        }
        Flickable
        {
            objectName: "recentSavedScroll"
            visible: !trackViewRoot.savedRecentEmpty
            x: recentPopup.pad; y: recentPopup.pad + recentPopup.titleH
            width: parent.width - 2 * recentPopup.pad
            height: parent.height - y - recentPopup.pad - recentPopup.footH - trackViewRoot.g
            contentWidth: width; contentHeight: recentColumn.height; clip: true
            boundsBehavior: Flickable.StopAtBounds; flickableDirection: Flickable.VerticalFlick
            Column {
                id: recentColumn
                width: parent.width; spacing: trackViewRoot.g
            Repeater
            {
                model: trackViewRoot.savedRecentRows
                Rectangle
                {
                    width: parent.width; height: recentPopup.itemH
                    color: trackViewRoot.cBtn; radius: 6
                    Text
                    {
                        x: 10; y: 8; width: parent.width - 22 - recentUse.width; elide: Text.ElideRight   // R503_UI_RECENT_FIT
                        text: modelData.name; color: trackViewRoot.cText
                        font.pixelSize: trackViewRoot.fs(14); font.bold: true
                    }
                    Rectangle
                    {
                        x: 10; y: parent.height - height - 7
                        width: 76 * trackViewRoot.ks; height: Math.max(18, 23 * trackViewRoot.ks)
                        color: modelData.section.toUpperCase() === "DROP" ? "#402126" : (modelData.section.toUpperCase() === "BREAK" ? "#183148" : "#303039")
                        radius: 3
                        Text { anchors.centerIn: parent; text: modelData.section.toUpperCase(); color: modelData.section.toUpperCase() === "DROP" ? "#FF9299" : (modelData.section.toUpperCase() === "BREAK" ? "#7EC5FF" : "#C0C0CE"); font.pixelSize: trackViewRoot.fs(11); font.bold: true }
                    }
                    Btn
                    {
                        objectName: "recentUse:" + index
                        x: parent.width - width - 6; y: 6
                        id: recentUse
                        // R503_UI_RECENT_BTN: 118 at 2048 as approved, grown with the popup (kw) - the text grows too
                        width: Math.max(118, Math.round(118 * trackViewRoot.kw)); height: parent.height - 12
                        enabled: modelData.canQueue === true
                        active: modelData.queued === true
                        // R503_UI_RECENT_SHORT: the engine's long reasons ran out of this small button - said short here
                        readonly property var shortWhy: ({ "GROUP UNAVAILABLE": "GROUP OFF", "COLOUR UNAVAILABLE": "NO COLOUR",
                                                           "PROGRAM CHANGED": "PROGRAM GONE", "FULL AUTO FIRST": "FULL AUTO" })
                        readonly property string why: modelData.reason || ("NO NEXT " + modelData.section.toUpperCase())
                        text: active ? "QUEUED" : (enabled ? "NEXT " + modelData.section.toUpperCase()
                                                           : (shortWhy[why] || why.replace("NO NEXT ", "NO ")))
                        fontPx: trackViewRoot.fs(11); tone: trackViewRoot.cGreen
                        touchInputEnabled: false
                        TapHandler {
                            gesturePolicy: TapHandler.DragThreshold
                            onTapped: if (trackEngine) trackEngine.queueSavedLook(modelData.id)
                        }
                    }
                }
            }
        }
        }
        Btn
        {
            objectName: "recentAllSaved"
            x: recentPopup.pad; y: parent.height - height - recentPopup.pad
            width: parent.width - 2 * recentPopup.pad; height: recentPopup.footH
            text: "ALL SAVED LOOKS"; icon: "savedLooks"; fontPx: trackViewRoot.fs(13)
            onTapped: { trackViewRoot.savedRecentOpen = false; trackViewRoot.setupOpen = true; trackViewRoot.savedFavoritesOpen = true }
        }
    }

    // R502_M_GHOST: the + TYPE chip in the finger's hand - over the page, just above the finger
    // (a ring marks the finger), framed in its colour. Flat: no shadow, no glow. Takes no input.
    Item
    {
        id: flagGhost
        objectName: "flagGhost"
        z: 600
        anchors.fill: parent
        visible: trackViewRoot.flagDragType !== ""
        readonly property real ringR: Math.max(16, Math.round(24 * trackViewRoot.ks))
        Rectangle
        {
            objectName: "flagGhostRing"
            width: 2 * flagGhost.ringR; height: width; radius: width / 2
            x: trackViewRoot.flagDragX - width / 2
            y: trackViewRoot.flagDragY - height / 2
            color: "transparent"
            border.width: 2
            border.color: Qt.rgba(1, 1, 1, 0.55)
        }
        Rectangle
        {
            id: flagGhostChip
            objectName: "flagGhostChip"
            width: flagTools.tw
            height: rail.bh
            // above the finger; where that would cover the bubble (a short track at 1366 x 768) beside it
            readonly property real gapPx: Math.round(4 * trackViewRoot.ks)
            readonly property real aboveY: trackViewRoot.flagDragY - flagGhost.ringR - gapPx - height
            readonly property real bubbleFoot: trackViewRoot.flagDragType === "" ? 0
                : flagDropBubble.mapToItem(trackViewRoot, 0, flagDropBubble.height).y + trackViewRoot.flagDragY * 0   // (read again as the finger moves)
            readonly property bool beside: flagDropBubble.visible && aboveY < bubbleFoot + gapPx
            readonly property real besideX: trackViewRoot.flagDragX + flagGhost.ringR + gapPx + width <= flagGhost.width
                ? trackViewRoot.flagDragX + flagGhost.ringR + gapPx : trackViewRoot.flagDragX - flagGhost.ringR - gapPx - width
            x: beside ? besideX : Math.max(0, Math.min(trackViewRoot.flagDragX - width / 2, flagGhost.width - width))
            y: beside ? Math.max(bubbleFoot + gapPx, trackViewRoot.flagDragY - height / 2) : Math.max(0, aboveY)
            radius: Math.round(10 * trackViewRoot.ks)
            color: "#2C2C33"
            border.width: 2
            border.color: trackViewRoot.markerColor(trackViewRoot.flagDragType)
            Row
            {
                anchors.centerIn: parent
                spacing: 7
                Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 10; height: 10; radius: 5; color: trackViewRoot.markerColor(trackViewRoot.flagDragType) }
                Text
                {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "+ " + trackViewRoot.flagDragType.toUpperCase()
                    color: trackViewRoot.cText
                    font.bold: true
                    font.pixelSize: trackViewRoot.fs(13)
                    font.letterSpacing: (trackViewRoot.fs(13)) * 0.06
                }
            }
        }
    }

    // TRACK_APP_BUILDNOTE_R601 (mockup approved 11-14): once, after the first start of a new build - TRACK UPDATED
    // (12 s) or UPDATE DID NOT START (30 s) over the waveform; it fades away and takes no taps (nothing under it is covered
    // for the finger)
    Rectangle
    {
        id: updNote
        objectName: "updNote"
        readonly property int kind: trackViewRoot.trackApp && trackAppCtl.updateNoteKind !== undefined ? trackAppCtl.updateNoteKind : 0
        property int shownKind: 0
        function play() { if (kind === 0) return; shownKind = kind; updNoteRun.restart() }
        onKindChanged: play()
        Component.onCompleted: play()
        visible: opacity > 0.01
        opacity: 0
        z: 50
        anchors.horizontalCenter: parent.horizontalCenter
        y: trackViewRoot.topH + Math.round(64 * trackViewRoot.ks)
        width: noteRow.implicitWidth + Math.round(40 * trackViewRoot.ks)
        height: Math.round(54 * trackViewRoot.ks)
        radius: height / 2
        color: shownKind === 2 ? "#2A1212" : "#10241A"
        border.width: 2
        border.color: shownKind === 2 ? "#E03030" : trackViewRoot.cGreen
        SequentialAnimation
        {
            id: updNoteRun
            PauseAnimation { duration: 1500 }                 // the page settles first
            NumberAnimation { target: updNote; property: "opacity"; to: 1.0; duration: 300 }
            PauseAnimation { duration: updNote.shownKind === 2 ? 30000 : 12000 }
            NumberAnimation { target: updNote; property: "opacity"; to: 0.0; duration: 2000 }
        }
        Row
        {
            id: noteRow
            anchors.centerIn: parent
            spacing: Math.round(14 * trackViewRoot.ks)
            Rectangle
            {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.round(26 * trackViewRoot.ks); height: width; radius: width / 2
                color: updNote.shownKind === 2 ? "#E03030" : trackViewRoot.cGreen
                Text { anchors.centerIn: parent; text: updNote.shownKind === 2 ? "!" : "\u2713"; color: "#0E0E12"; font.bold: true; font.pixelSize: trackViewRoot.fs(16) }
            }
            Text
            {
                anchors.verticalCenter: parent.verticalCenter
                text: updNote.shownKind === 2 ? qsTr("UPDATE DID NOT START") : qsTr("TRACK UPDATED")
                color: updNote.shownKind === 2 ? "#FF6A6A" : trackViewRoot.cGreen
                font.bold: true
                font.pixelSize: trackViewRoot.fs(17)
                font.letterSpacing: trackViewRoot.fs(17) * 0.08
            }
            Text
            {
                objectName: "updNoteText"
                anchors.verticalCenter: parent.verticalCenter
                textFormat: Text.StyledText
                text: trackViewRoot.trackApp && trackAppCtl.updateNote !== undefined ? trackAppCtl.updateNote : ""
                color: trackViewRoot.cText
                font.pixelSize: trackViewRoot.fs(14)
            }
        }
    }
}
