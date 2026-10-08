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
    readonly property real topH: Math.max(50, Math.round(64 * ks))
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
    property int currentBeat: trackManager ? trackManager.currentBeat : 0
    property string liveState: trackManager ? trackManager.currentState : "normal"
    property var states: [ "normal", "break", "build", "drop" ]

    property var divValues: [ 0, 4000, 2000, 1000, 500, 250, 125 ]
    property var divLabels: [ "-", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8" ]

    property bool setupOpen: false
    property bool helpOpen: false
    // R402_MOTION (runde 402): the page's motion - presses, fades, the beat, the
    // faders' glide. The Qt check turns it off to read every end state at once
    property bool animate: true
    // the beat in the bar, 0..3, from the track's own downbeat (as barOf)
    readonly property int beatInBar: currentBeat > 0
        ? (((currentBeat - 1 - (trackManager ? (trackManager.downbeat || 0) : 0)) % 4) + 4) % 4 : -1

    // runde 396: the four AUTO tiles in one - lit as their own tiles are lit
    readonly property bool allAutoOn: trackEngine && trackManager
        && trackEngine.roomAuto
        && trackManager.overrideState === ""
        && trackEngine.positionMode === ""
        && trackEngine.colourMode === 0 && (trackEngine.colourOverride === "" || trackEngine.colourOverrides.length > 1)
    function allAuto()
    {
        if (!trackEngine || !trackManager) return
        trackEngine.roomAuto = true
        trackManager.overrideState = ""
        trackEngine.positionMode = ""
        trackEngine.colourOverride = ""        // AUTO colour - and FADE / CHASE off with it
    }             // runde 393: the guide over the page
    // the pencil: the flag tools stay out until it is tapped again (Tobias 10-07)
    property bool markerEdit: false

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
        function onTrackChanged() { wfArea.release(); wfCanvas.selected = -1; wfCanvas.requestPaint() }
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
            } else if (kind === "showSwitch") { c.fillRect(5,5,14,14) }
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
        property alias input: btnInput
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
        color: trackViewRoot.cCard
        radius: Math.round(14 * trackViewRoot.ks)
        border.width: 0                 // R403_NO_LINES
        Text
        {
            x: trackViewRoot.cardPad + 2
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
                anchors.right: statusRight.left
                anchors.rightMargin: Math.round(22 * trackViewRoot.ks)
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3
                Text
                {
                    width: parent.width
                    elide: Text.ElideRight
                    text: trackManager && trackManager.title !== "" ? trackManager.title : qsTr("No track loaded")
                    color: trackViewRoot.cText
                    font.pixelSize: trackViewRoot.fs(23)
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

            Row
            {
                id: statusRight
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Math.round(18 * trackViewRoot.ks)

                // R402_BEAT_DOTS (Tobias 10-08: "kun fire beat prikker ved uret"): the beat
                // in the bar - the one playing lit, the downbeat white - and a small
                // pop on each beat (R402_BEAT_PULSE)
                Row
                {
                    objectName: "beatDots"
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
                    Text
                    {
                        anchors.right: parent.right
                        text: trackViewRoot.fmtTime(trackManager ? trackManager.positionMs : 0)
                              + " / " + trackViewRoot.fmtTime(trackManager ? trackManager.durationMs : 0)
                        color: trackViewRoot.cText
                        font.pixelSize: trackViewRoot.fs(21)
                    }
                    Text
                    {
                        anchors.right: parent.right
                        // runde 308: the tempo the engine times on, and from where.
                        // Anything but LINK is amber
                        text: (trackManager && trackManager.playing ? qsTr("PLAYING") : qsTr("PAUSED"))
                              + "  ·  " + (trackManager && trackManager.engineTempo !== undefined ? trackManager.engineTempo.toFixed(1) : "0") + " BPM"
                              + (trackManager && trackManager.tempoSource ? "  ·  " + trackManager.tempoSource : "")
                              + "  ·  " + (trackManager && trackManager.connected ? qsTr("BLT ok") : qsTr("no BLT"))
                        color: trackManager && trackManager.playing
                               ? (trackManager.tempoSource === "LINK" ? "#59C36A" : "#E3B44F")
                               : trackViewRoot.cDim
                        font.pixelSize: trackViewRoot.fs(13)
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
                    property string text: checked ? (armOff ? qsTr("SURE?") : qsTr("SHOW ON")) : qsTr("START SHOW")
                    Timer { id: showOffArm; interval: 4000; onTriggered: showSwitch.armOff = false }
                    color: armOff ? "#E3B44F" : (checked ? "#3FBF3F" : "#4A1E1E")
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
                                      kind: "showSwitch"; ink: showSwitch.checked ? "#0A2A0A" : "#FFC8C8" }
                        Text
                        {
                            anchors.verticalCenter: parent.verticalCenter
                            text: showSwitch.text
                            color: showSwitch.checked ? "#0A2A0A" : "#FFC8C8"
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
                    var barTop = strip + 30
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

            Text
            {
                anchors.centerIn: wfCanvas
                visible: trackViewRoot.beatCount === 0
                text: qsTr("Waiting for track data from Beat Link Trigger...")
                color: trackViewRoot.cMute
                font.pixelSize: trackViewRoot.fs(15)
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
                y: wfCanvas.stripH + Math.round(10 * trackViewRoot.ks)
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
                                objectName: "addFlag:" + modelData
                                width: flagTools.tw
                                height: rail.bh
                                radius: Math.round(10 * trackViewRoot.ks)
                                color: addIn.down ? "#2C2C33" : trackViewRoot.cBtn
                                // R403_NO_LINES
                                scale: addIn.down ? 0.965 : 1.0      // R402_PRESS
                                Behavior on scale { enabled: trackViewRoot.animate; NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
                                Row
                                {
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
                                    onReleasedAt: (x, y, inside) => { if (inside) trackManager.addMarker(trackManager.currentBeat > 0 ? trackManager.currentBeat : 1, modelData) }
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
                    anchors.centerIn: parent
                    spacing: 10
                    visible: trackManager && trackManager.roleMode && !trackViewRoot.setupOpen
                             && !trackViewRoot.markerEdit && verdictTools.blaming === 0 && warnText.text.length > 0
                    width: Math.min(implicitWidth, rail.width - Math.round(520 * trackViewRoot.ks))
                    ControlIcon { anchors.verticalCenter: parent.verticalCenter; width: 18; height: 18; kind: "warn"; ink: "#E3B44F" }
                    Text
                    {
                        id: warnText
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, rail.width - Math.round(560 * trackViewRoot.ks))
                        elide: Text.ElideRight
                        color: "#E3B44F"
                        font.pixelSize: trackViewRoot.fs(13)
                        text:
                        {
                            var parts = []
                            if (trackManager && trackManager.linkStale)
                                parts.push(qsTr("BLT link stale - holding the last look"))
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
                                Canvas
                                {
                                    anchors.centerIn: parent
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
                                        if (trackEngine) trackEngine.rate(modelData)
                                        thumb.lit = true
                                        thumbFlash.restart()
                                    }
                                }
                                Timer { id: thumbFlash; interval: 220; onTriggered: thumb.lit = false }
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

                Repeater
                {
                    id: castRep
                    model: trackEngine ? trackEngine.groups : []

                    Item
                    {
                        id: grp
                        // the same fallback as before: a groups rebuild re-evaluates
                        // every binding with modelData gone
                        property var md: modelData ? modelData : ({ key: "", enabled: true, switchOnly: false, base: false })
                        property bool lit: rigCard.litNow.indexOf(md.key) >= 0
                        property bool off: !md.enabled
                        property bool switchOnly: md.switchOnly === true
                        property real trim: rigCard.allTrims[md.key] !== undefined ? rigCard.allTrims[md.key] : 1.0
                        x: rigCard.inX
                        y: deck.rowY(1) + index * (rigCard.rowH + trackViewRoot.g)
                        width: rigCard.inW
                        height: rigCard.rowH
                        readonly property real faderW: Math.round(228 * trackViewRoot.kw)
                        readonly property real swW: Math.round(58 * trackViewRoot.ks)
                        readonly property color ink: off ? "#4E4E56" : (lit ? "#E6E6EC" : "#8C8C96")

                        ControlIcon
                        {
                            id: grpIcon
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.round(26 * trackViewRoot.ks); height: width
                            ink: grp.ink
                            kind: grp.md.switchOnly ? "animation" : grp.md.strobes ? "strobe" : grp.md.lasers ? "laser"
                                  : grp.md.key.toLowerCase().indexOf("eyes") >= 0 ? "eyes" : "head"
                        }
                        Column
                        {
                            anchors.left: grpIcon.right
                            anchors.leftMargin: Math.round(12 * trackViewRoot.ks)
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
                                text: (grp.md.base ? "<font color='#4FA3E3'><b>BASE</b></font> · " : "")
                                      + (grp.off ? qsTr("off") : (grp.lit ? "● " + qsTr("on stage") : "○ " + qsTr("waiting")))
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

                            // the base is always in the show: a lock, not a switch
                            ControlIcon
                            {
                                visible: grp.md.base === true
                                anchors.centerIn: parent
                                width: Math.round(20 * trackViewRoot.ks); height: width
                                kind: "lock"; ink: trackViewRoot.cBlue
                            }
                            // A TOGGLE (runde 139): in or out of tonight's show
                            Rectangle
                            {
                                id: grpSwitch
                                objectName: "groupSwitch:" + grp.md.key
                                visible: !grp.md.base
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
                            fill: grp.lit ? (grp.md.base ? "#2E6FA8" : "#3D86C4") : (input.down ? "#45454E" : "#3A3A42")
                            gripInk: grp.lit ? "#BFE3FF" : "#B0B0B8"
                            valueOnFill: "#F2F6FA"
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
                            color: grp.off ? "#0A0A0D" : (grp.lit ? "#3D86C4" : "#3A3A42")      // R403_NO_LINES
                            Text
                            {
                                anchors.centerIn: parent
                                text: grp.off ? qsTr("ON / OFF ONLY") : qsTr("ON")
                                color: grp.off ? "#5E5E68" : "#F2F6FA"
                                font.bold: true
                                font.pixelSize: trackViewRoot.fs(12)
                                font.letterSpacing: (trackViewRoot.fs(12)) * 0.12
                            }
                            // the base is always in the show, switch-only or not
                            TouchInput
                            {
                                enabled: grp.switchOnly && !grp.md.base
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
                    border.color: Qt.rgba(0.89, 0.71, 0.31, 0.45)
                    // a hand on the bar takes over from the clock - also when it
                    // lands where the clock already put it
                    onPressed: if (trackEngine && trackEngine.roomAuto) trackEngine.roomAuto = false
                    onSetLevel: (v) => { if (trackManager) trackManager.energyTrim = Math.round(v * 100) }
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
                    RowLabel { x: lookCard.inX; y: deck.rowY(2); width: lookCard.labW; height: trackViewRoot.rowH; text: "POSITION" }
                    Btn
                    {
                        objectName: "positionAuto"
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
                    x: lookCard.cX + 3 * (lookCard.cell(7) + lookCard.cg)
                    y: deck.rowY(4); width: lookCard.cell(7) - Math.round(8 * trackViewRoot.ks); height: trackViewRoot.rowH
                    horizontalAlignment: Text.AlignRight
                    text: "SPEED"
                }
                // how fast the engine runs the figures
                Repeater
                {
                    model: [ "½×", "1×", "2×" ]
                    Btn
                    {
                        objectName: "speed" + index
                        x: lookCard.cX + (4 + index) * (lookCard.cell(7) + lookCard.cg)
                        y: deck.rowY(4); width: lookCard.cell(7); height: trackViewRoot.rowH
                        text: modelData
                        tone: [ "#7A9ABA", "#4FA3E3", "#E3B44F" ][index]
                        active: trackEngine ? trackEngine.speed === index - 1 : index === 1
                        onTapped: trackEngine.speed = index - 1
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
                    RowLabel { x: lookCard.inX; y: deck.rowY(5); width: lookCard.labW; height: trackViewRoot.rowH; text: "HAZE MACHINE" }
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
                    }
                }
            }

            // ---- LIVE: one press, right now - the urgent ones lowest ----
            Card
            {
                id: liveCard
                title: "LIVE"
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

                Btn
                {
                    objectName: "nextLook"
                    x: liveCard.inX; y: deck.rowY(0); width: liveCard.inW; height: trackViewRoot.rowH
                    icon: "nextLook"
                    text: qsTr("NEXT LOOK")
                    onTapped: trackEngine.next()
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
                    x: liveCard.inX; y: deck.rowY(3); width: liveCard.inW; height: liveCard.bigH
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
                    }
                }

                // FLASH WHITE: on while held. Dark with a white edge at rest - a big
                // light tile glares in a dark booth - and white while it flashes
                Rectangle
                {
                    id: flashTile
                    objectName: "flash"
                    property bool on: trackEngine ? trackEngine.flashing : false
                    x: liveCard.inX; y: deck.rowY(3) + liveCard.bigH + trackViewRoot.g; width: liveCard.inW
                    height: liveCard.bigH
                    radius: Math.round(10 * trackViewRoot.ks)
                    color: on ? "#FFFFFF" : (flashIn.down ? "#34343C" : "#24242A")
                    scale: flashIn.down ? 0.975 : 1.0                                                        // R402_PRESS
                    Behavior on scale { enabled: trackViewRoot.animate; NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
                    border.width: 2
                    border.color: "#F2F2F5"
                    Row
                    {
                        anchors.centerIn: parent
                        spacing: Math.round(12 * trackViewRoot.ks)
                        ControlIcon { anchors.verticalCenter: parent.verticalCenter; width: Math.round(24 * trackViewRoot.ks); height: width
                                      kind: "flash"; ink: flashTile.on ? "#101010" : "#FFFFFF" }
                        Text
                        {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("FLASH WHITE")
                            color: flashTile.on ? "#101010" : "#FFFFFF"
                            font.bold: true
                            font.pixelSize: trackViewRoot.fs(18)
                            font.letterSpacing: (trackViewRoot.fs(18)) * 0.14
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
            Row
            {
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: -2
                spacing: 12
                visible: trackManager && trackManager.nextTitle !== undefined && trackManager.nextTitle !== ""
                Text { text: qsTr("NEXT TRACK"); color: trackViewRoot.cMute; font.bold: true; font.pixelSize: trackViewRoot.fs(11); font.letterSpacing: (trackViewRoot.fs(11)) * 0.15
                       anchors.verticalCenter: parent.verticalCenter }
                Text
                {
                    anchors.verticalCenter: parent.verticalCenter
                    textFormat: Text.StyledText
                    font.pixelSize: trackViewRoot.fs(13)
                    color: trackViewRoot.cMute
                    text: trackManager ? "<font color='#CFCFD6'>" + trackManager.nextTitle + "</font>"
                                         + (trackManager.nextFirstDrop > 0 ? "  ·  " + qsTr("first drop bar") + " " + trackManager.nextFirstDrop : "") : ""
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
        visible: trackViewRoot.setupOpen && trackManager && trackManager.roleMode
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
                    { t: "TOP BAR", b: "<b>Section pill</b> - the section playing now; a white frame: pinned by hand.<br><b>Next</b> - the coming marker and how far away.<br><b>Four dots</b> - the beat in the bar; the first, white, is the downbeat.<br><b>START SHOW</b> starts the engine. <b>SHOW ON</b> stops on the second tap (SURE?).<br><b>SETUP</b> - groups, scenes and the rig." },
                    { t: "THE TRACK", b: "<b>Coloured bands</b> - the sections, with their energy.<br><b>White line</b> - where the track is; the bright bars are played.<br><b>The countdown</b> - bars to the next section.<br>Warnings appear in the bar under the track." },
                    { t: "MARKERS & THUMBS", b: "<b>MARKERS</b> opens the marker tools - <b>DONE</b> closes them.<br><b>+ TYPE</b> sets a marker on the bar playing. Each marker gets a tab at the foot of the track: tap it to <b>RETYPE</b> or <b>DELETE</b>, drag it to move - it lands when you let go. <b>UNDO</b> steps back.<br><b>Thumbs</b> rate this moment; hold one to rate a single group." },
                    { t: "TOUCH", b: "Several fingers work at once - hold <b>FLASH WHITE</b> while you pull <b>ENERGY</b>, or move two faders together.<br><b>HELP</b> and <b>SETUP</b> close with the same button; a tap on the guide closes it too." }
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
            text: "<b>Toggle</b> - the group is in or out of tonight's show.<br><br><b>Fader</b> - the group's own ceiling.<br><br><font color='#6ECD82'>● on stage</font> - lit now. ○ <b>waiting</b> - in the show, not used right now.<br><br><b>Lock</b> - the base group, always in.<br><br><b>ON / OFF ONLY</b> - a group that can only be switched."
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
                { l: "ENERGY",       b: "How hard the show plays - your hand takes over. <b>AUTO</b>: the clock moves it through the night. <b>ALL AUTO</b> (above, when one is off) puts all four back." },
                { l: "SECTION",      b: "Force a section; the one the music is in is filled. Tap it again or <b>AUTO</b> to follow the music." },
                { l: "POSITION",     b: "Hold the heads in one position. <b>AUTO</b>: the engine moves them." },
                { l: "COLOUR",       b: "Tap colours to make your own mix. <b>Filled</b>: in the mix. <b>Ring</b>: leading now. <b>AUTO</b>: the engine picks." },
                { l: "COLOUR MODE",  b: "<b>FADE</b>: the mix glides over. <b>CHASE</b>: it steps on the kick. <b>SPEED</b>: the pace of moves and chases." },
                { l: "HAZE MACHINE", b: "Haze and fan - always by hand, the engine never touches them." }
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
                { y: 0, h: 1, b: "<b>NEXT LOOK</b> - a new look now." },
                { y: 1, h: 1, b: "<b>HOLD</b> - freeze the look until you tap it again." },
                { y: 2, h: 1, b: "<b>CALM</b> - calmer for 16 bars; tap again to end it." },
                { y: 3, h: 2, b: "<b>BLACKOUT</b> - dark while held. Slide off before letting go to lock it; tap to release." },
                { y: 4, h: 2, b: "<b>FLASH WHITE</b> - white while held." }
            ]
            HelpText
            {
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
}
