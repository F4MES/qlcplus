/*
  Q Light Controller Plus
  TrackView.qml

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

import org.qlcplus.classes 1.0
import "."

Rectangle
{
    id: trackViewRoot
    anchors.fill: parent
    color: "#1B1B1B"

    // fixed palette: the stock Controls theme is light and QLC's dark theme
    // does not reach it, so nothing here relies on UISettings for colour
    readonly property color cPanel:  "#262626"
    readonly property color cBtn:    "#3A3A3A"
    readonly property color cBtnHi:  "#4A4A4A"
    readonly property color cLine:   "#555555"
    readonly property color cText:   "#EEEEEE"
    readonly property color cDim:    "#9A9A9A"

    property int beatCount: trackManager ? trackManager.beatCount : 0
    property int currentBeat: trackManager ? trackManager.currentBeat : 0
    property string liveState: trackManager ? trackManager.currentState : "normal"
    property var states: [ "normal", "break", "build", "drop" ]

    property var divValues: [ 0, 4000, 2000, 1000, 500, 250, 125 ]
    property var divLabels: [ "-", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8" ]

    // TRACK_TOUCH_LAYOUT_R137 — original controls, layout-only revision r138.
    readonly property bool compactLayout: height < 950
    property bool setupOpen: false
    property real touchH: Math.max(UISettings.iconSizeMedium * 1.4, 50)

    property int dragIndex: -1
    property real dragX: 0
    property real dragOffset: 0
    property bool zoomActive: false
    property int zoomCenter: 1
    property int zoomSpan: 64

    function markerColor(type)
    {
        if (type === "drop")  return "#E23B3B"
        if (type === "build") return "#E0921A"
        if (type === "break") return "#2F7FD0"
        if (type === "intro") return "#5FB37A"
        if (type === "outro") return "#8C6BB1"
        return "#9AA0A6"
    }

    function fmtTime(ms)
    {
        if (ms <= 0) return "--:--"
        var t = Math.floor(ms / 1000)
        var m = Math.floor(t / 60)
        var s = t % 60
        return m + ":" + (s < 10 ? "0" : "") + s
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
        function onTrackChanged() { wfArea.release(); wfCanvas.requestPaint() }
        function onMarkersChanged() { wfCanvas.requestPaint() }
        // positionChanged arrives once per beat and not more (see below),
        // so this repaints once per beat.
        //
        // Runde 143 put a gate here - only repaint when currentBeat changed -
        // on the reading that TrackManager's 200 ms energy timer emitted
        // positionChanged unconditionally, five times a second. That reading
        // was wrong, and runde 144 measured it: slotEnergyTick() emits
        // positionChanged only in its thirty-second-dead branch, and
        // handlePosition() returns early unless the beat or the playing flag
        // changed. Beat Link Trigger sends no `time` field at all, so the
        // third term of that guard is 0 == 0 for ever. Counted on the log of
        // 2026-09-20: 11172 rows, 11112 beat changes - 1.01 rows per change.
        //
        // The gate was therefore dead code, and two tests were holding it in
        // place. Both are gone. If the protocol ever gains a `time` field -
        // the C++ already parses one - this is where the guard goes, and the
        // paragraph above is why.
        function onPositionChanged() { wfCanvas.requestPaint() }
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
            var want = wfArea.beatAt(trackViewRoot.dragX) + trackViewRoot.dragOffset
            var before = trackManager.markers.length
            trackManager.moveMarker(trackViewRoot.dragIndex, want)
            if (trackManager.markers.length !== before)
                wfArea.reindex(want)
            wfCanvas.requestPaint()
        }
    }

    // Small vector icons: no font symbols that change between Windows and macOS.
    // WHAT MAKES A BAR LOOK LIKE A SLIDER (runde 140).
    //
    // Tobias: "hvordan gør vi så alle sliders faktisk viser at det er
    // sliders? for nye djs der aldrig har set det før kan det godt være lidt
    // svært at se dem." He is right, and it is the same problem on all four
    // of them: a coloured rectangle that fills part of a box is what every
    // progress bar in the world looks like, and nobody drags a progress bar.
    //
    // Three things turn it into something a hand reaches for, and none of
    // them is a label:
    //   the GRIP   a raised handle at the level, with three ridges cut into
    //              it. This is the one that does the work - a ridged handle
    //              is the oldest "hold here" signal there is, and it is the
    //              only part of a fader a DJ has ever touched.
    //   the TRACK  ticks at a quarter, a half and three quarters, so the bar
    //              reads as a scale with positions rather than as a bar that
    //              happens to be part full.
    //   the REST   the part above the level stays visibly empty, so there is
    //              somewhere obvious for the level to go.
    //
    // One component, used by ENERGY, MASTER DIMMER, the five group trims and
    // HAZE / FAN SPEED, so the page teaches the gesture once.
    component SliderGrip: Item {
        property color ink: "#EEEEEE"
        property bool pressed: false
        width: 18
        Rectangle {
            anchors.fill: parent
            anchors.topMargin: 2
            anchors.bottomMargin: 2
            radius: 4
            color: parent.pressed ? "#FFFFFF" : parent.ink
            border.width: 1
            border.color: "#0E0E0E"
            // the ridges
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
        // a quarter, a half, three quarters - short marks top and bottom
        Repeater {
            model: [ 0.25, 0.5, 0.75 ]
            Item {
                x: 3 + (parent.width - 6) * modelData - 1
                width: 2
                height: parent.height
                Rectangle { y: 0; width: 2; height: 6; color: "#4A4A4A" }
                Rectangle { y: parent.height - 6; width: 2; height: 6; color: "#4A4A4A" }
            }
        }
    }

    component ControlIcon: Canvas {
        property string kind: ""
        property color ink: "#DDDDDD"
        width: 22; height: 22
        onKindChanged: requestPaint()
        onInkChanged: requestPaint()
        onPaint: {
            var c = getContext("2d"); c.reset(); c.scale(width / 24, height / 24)
            c.strokeStyle = ink; c.fillStyle = ink; c.lineWidth = 1.7
            c.lineCap = "round"; c.lineJoin = "round"
            function line(x,y,x2,y2) { c.beginPath(); c.moveTo(x,y); c.lineTo(x2,y2); c.stroke() }
            function circle(x,y,r) { c.beginPath(); c.arc(x,y,r,0,Math.PI*2); c.stroke() }
            if (kind === "calm") {
                c.beginPath(); c.moveTo(5,19); c.bezierCurveTo(1,9,15,10,19,3); c.bezierCurveTo(22,16,13,21,5,19); c.stroke(); line(4,21,15,11)
            } else if (kind === "hold") { c.fillRect(6,4,4,16); c.fillRect(14,4,4,16) }
            else if (kind === "nextLook") { c.beginPath(); c.moveTo(4,4); c.lineTo(16,12); c.lineTo(4,20); c.closePath(); c.fill(); line(19,4,19,20) }
            else if (kind === "blackout") { circle(12,12,9); line(6,18,18,6) }
            // "give it back to the music": an arrow curving anticlockwise
            // back to where it started. A sun says "automatic"; on the
            // SECTION row what the button does is HAND THE SECTION BACK, so
            // it gets the revert arrow instead. (Tobias, 2026-09-22.)
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
            else if (kind === "animation") {
                circle(12,12,2)
                for(var k=0;k<4;k++){c.save();c.translate(12,12);c.rotate(k*Math.PI/2);c.beginPath();c.moveTo(0,-3);c.bezierCurveTo(-7,-13,6,-13,3,-3);c.stroke();c.restore()}
            } else { circle(12,8,6); c.strokeRect(3,21,18,2); c.beginPath(); c.moveTo(3,8); c.lineTo(3,18); c.lineTo(21,18); c.lineTo(21,8); c.stroke() }
        }
    }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 6; spacing: 6
Rectangle
        {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            Layout.maximumHeight: 56
            color: trackViewRoot.cPanel
            radius: 4

            Row
            {
                anchors.left: parent.left
                anchors.right: statusRight.left
                anchors.rightMargin: 14
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 14

                Rectangle
                {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 120
                    height: 48
                    radius: 5
                    color: trackViewRoot.markerColor(trackViewRoot.liveState)
                    border.width: trackManager && trackManager.overrideState !== "" ? 3 : 0
                    border.color: "#FFFFFF"

                    Text
                    {
                        anchors.centerIn: parent
                        text: trackViewRoot.liveState.toUpperCase()
                        color: "#000000"
                        font.bold: true
                        font.pixelSize: 20
                    }
                }

                Column
                {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    Text
                    {
                        width: Math.max(0, parent.parent.width - 134)
                        elide: Text.ElideRight
                        text: trackManager && trackManager.title !== ""
                              ? trackManager.title : qsTr("No track loaded")
                        color: trackViewRoot.cText
                        font.pixelSize: 18
                    }
                    Text
                    {
                        property var nm: trackViewRoot.nextMarker()
                        text:
                        {
                            if (nm === null) return qsTr("No further points")
                            var d = nm.beat - trackViewRoot.currentBeat
                            // bars rounded UP, like the countdown in the waveform and
                            // the footer: "in 1 beat (0 bars)" sat next to a "DROP 1"
                            var bars = Math.ceil(d / 4)
                            return qsTr("Next") + ": " + nm.type.toUpperCase()
                                   + " " + qsTr("in") + " " + d + " " + (d === 1 ? qsTr("beat") : qsTr("beats"))
                                   + "  (" + bars + " " + (bars === 1 ? qsTr("bar") : qsTr("bars")) + ")"
                        }
                        color: nm === null ? trackViewRoot.cDim
                                           : trackViewRoot.markerColor(nm.type)
                        font.pixelSize: 15
                    }
                }
            }

            Row
            {
                id: statusRight
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 14

                Column
                {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    Text
                    {
                        text: trackViewRoot.fmtTime(trackManager ? trackManager.positionMs : 0)
                              + " / "
                              + trackViewRoot.fmtTime(trackManager ? trackManager.durationMs : 0)
                        color: trackViewRoot.cText
                        font.pixelSize: 17
                    }
                    Text
                    {
                        text: (trackManager && trackManager.playing
                               ? qsTr("PLAYING") : qsTr("PAUSED"))
                              + "   " + (trackManager ? trackManager.liveBpm : 0) + " BPM"
                              + "   " + (trackManager && trackManager.connected
                                         ? qsTr("BLT ok") : qsTr("no BLT"))
                        color: trackManager && trackManager.playing ? "#3FBF3F"
                                                                    : trackViewRoot.cDim
                        font.pixelSize: 14
                    }
                }

Button
                {
                    width: 160
                    height: 48
                    anchors.verticalCenter: parent.verticalCenter
                    visible: trackManager ? trackManager.roleMode : false
                    checked: trackEngine ? trackEngine.startScene : false
                    objectName: "startScene"
                    text: qsTr("START SCENE")
                    onClicked: if (trackEngine) trackEngine.startScene = !checked

                    contentItem: Text
                    {
                        text: parent.text
                        color: parent.checked ? "#101010" : trackViewRoot.cText
                        font.bold: true
                        font.pixelSize: 15
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle
                    {
                        radius: 5
                        color: parent.checked ? "#E3B44F" : trackViewRoot.cBtn
                        border.width: parent.checked ? 3 : 1
                        border.color: parent.checked ? "#E3B44F" : trackViewRoot.cLine
                    }
                }
Button
                {
                    width: 196
                    height: 48
                    anchors.verticalCenter: parent.verticalCenter
                    checked: trackManager ? trackManager.autoRun : false
                    objectName: "showSwitch"
                ControlIcon { x: 6; anchors.verticalCenter: parent.verticalCenter;  kind: "showSwitch" }
                    text: checked ? qsTr("SHOW ON") : qsTr("SHOW OFF")
                    onClicked: trackManager.autoRun = !checked

                    contentItem: Text
                    {
                        text: parent.text
                        color: parent.checked ? "#0A2A0A" : "#FFC8C8"
                        font.bold: true
                        font.pixelSize: 22
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle
                    {
                        radius: 5
                        color: parent.checked ? "#3FBF3F" : "#4A1E1E"
                        border.width: 3
                        border.color: parent.checked ? "#9BE89B" : "#B03030"
                    }
                }
                Button
                {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 110
                    height: 48
                    objectName: "setupSwitch"
                ControlIcon { x: 6; anchors.verticalCenter: parent.verticalCenter;  kind: "setupSwitch" }
                    text: trackViewRoot.setupOpen ? qsTr("CLOSE") : qsTr("SETUP")
                    onClicked: trackViewRoot.setupOpen = !trackViewRoot.setupOpen

                    contentItem: Text
                    {
                        text: parent.text
                        color: trackViewRoot.cText
                        font.bold: true
                        font.pixelSize: 15
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle
                    {
                        radius: 5
                        color: parent.down ? trackViewRoot.cBtnHi : trackViewRoot.cBtn
                        border.width: 1
                        border.color: trackViewRoot.cLine
                    }
                }
            }
        }
Rectangle
        {
            Layout.fillWidth: true
            Layout.fillHeight: true
            // THE ONE ELASTIC ROW (runde 139). Everything else on this page
            // is now a fixed height, so whatever the window has left over
            // lands here - which is what Tobias allowed ("Du må gerne udvide
            // waveformen til at være lidt højere hvis du skal bruge den til
            // at udfylde pladsen lidt"), and it means there is no slack left
            // to show up as a gap somewhere else. The cap only stops it from
            // swallowing the page if a row below ever collapses.
            Layout.minimumHeight: trackViewRoot.compactLayout ? 130 : 180
            Layout.preferredHeight: trackViewRoot.compactLayout ? 130 : 180
            Layout.maximumHeight: Math.max(180, trackViewRoot.height * 0.62)
            color: "#101010"
            border.width: 1
            border.color: trackViewRoot.zoomActive ? "#E0921A" : trackViewRoot.cLine

            // ---- what the analysis and the engine see, drawn over the
            //      waveform: bass as a warm floor, highs as a cool line, kicks
            //      as ticks, section bands with their energy, the played part
            //      of this section tinted in the running colour, and a countdown
            //      to the next section. A finger on a flag selects it (drag to
            //      move it); the tools at the bottom left add, retype and delete.
            //      (WF_OVERLAY_V15)
            Canvas
            {
                id: wfOverlay
                anchors.fill: parent
                anchors.margins: 1
                anchors.bottomMargin: 56
                z: 1
                renderStrategy: Canvas.Threaded

                // the selected flag (an index into trackManager.markers), -1 = none
                property int selected: -1

                function sortedMarkers()
                {
                    var mk = trackManager ? trackManager.markers : []
                    var out = []
                    for (var i = 0; i < mk.length; i++) out.push({ beat: mk[i].beat, type: mk[i].type, energy: mk[i].energy, index: i })
                    out.sort(function(a, b) { return a.beat - b.beat })
                    return out
                }

                function beatX(beat, first, count) { return (beat - first) / count * width }

                onPaint:
                {
                    var ctx = getContext("2d")
                    var w = width, h = height
                    ctx.clearRect(0, 0, w, h)
                    if (!trackManager || trackManager.beatCount <= 0)
                        return

                    var total = trackManager.beatCount
                    var first = trackViewRoot.zoomActive ? trackViewRoot.viewFirst() : 1
                    var count = trackViewRoot.zoomActive ? trackViewRoot.viewCount() : total
                    if (count <= 0) count = total
                    var step = Math.max(1, Math.floor(count / w))
                    var low = trackManager.lowCurve, high = trackManager.highCurve, kick = trackManager.kickCurve

                    // bass: a warm floor, the lower third
                    if (low && low.length > 0)
                    {
                        ctx.beginPath()
                        ctx.moveTo(0, h)
                        for (var b = first; b < first + count && b <= low.length; b += step)
                        {
                            var v = 0
                            for (var k = 0; k < step && b - 1 + k < low.length; k++) v = Math.max(v, low[b - 1 + k])
                            ctx.lineTo(beatX(b, first, count), h - (v / 255) * h * 0.34)
                        }
                        ctx.lineTo(w, h)
                        ctx.closePath()
                        ctx.fillStyle = "rgba(227, 180, 79, 0.28)"
                        ctx.fill()
                    }

                    // highs: a thin cool line in the upper third
                    if (high && high.length > 0)
                    {
                        ctx.beginPath()
                        var started = false
                        for (var b2 = first; b2 < first + count && b2 <= high.length; b2 += step)
                        {
                            var v2 = 0
                            for (var k2 = 0; k2 < step && b2 - 1 + k2 < high.length; k2++) v2 = Math.max(v2, high[b2 - 1 + k2])
                            var y = h * 0.32 - (v2 / 255) * h * 0.24
                            if (!started) { ctx.moveTo(beatX(b2, first, count), y); started = true }
                            else ctx.lineTo(beatX(b2, first, count), y)
                        }
                        ctx.strokeStyle = "rgba(127, 211, 255, 0.75)"
                        ctx.lineWidth = 1.5
                        ctx.stroke()
                    }

                    // kicks: ticks along the floor, brighter the harder
                    if (kick && kick.length > 0 && count < w * 2)
                    {
                        for (var b3 = first; b3 < first + count && b3 <= kick.length; b3++)
                        {
                            var kv = kick[b3 - 1] / 255
                            if (kv < 0.45) continue
                            ctx.fillStyle = "rgba(255, 106, 106, " + (0.25 + 0.75 * kv).toFixed(2) + ")"
                            ctx.fillRect(beatX(b3, first, count), h - 4, Math.max(1, w / count * 0.6), 4)
                        }
                    }

                    // section bands at the top, with the analysed energy
                    var sorted = sortedMarkers()
                    ctx.font = "bold 11px sans-serif"
                    ctx.textBaseline = "top"
                    for (var j = 0; j < sorted.length; j++)
                    {
                        var m = sorted[j]
                        var endBeat = j + 1 < sorted.length ? sorted[j + 1].beat : total + 1
                        if (endBeat < first || m.beat > first + count) continue
                        var x0 = Math.max(0, beatX(m.beat, first, count))
                        var x1 = Math.min(w, beatX(endBeat, first, count))
                        var e = m.energy >= 0 ? m.energy : 0.5
                        var col = Qt.color(trackViewRoot.markerColor(m.type))
                        ctx.fillStyle = Qt.rgba(col.r, col.g, col.b, 0.18 + 0.5 * e)
                        ctx.fillRect(x0, 0, x1 - x0, 7)
                        if (m.index === selected)
                        {
                            // the selected flag: a white frame on its band and a line down
                            ctx.strokeStyle = "rgba(255,255,255,0.9)"
                            ctx.lineWidth = 2
                            ctx.strokeRect(x0 + 1, 1, Math.max(4, x1 - x0 - 2), 22)
                            ctx.fillStyle = "rgba(255,255,255,0.35)"
                            ctx.fillRect(x0, 0, 2, h)
                        }
                    }

                    // the played part of this section, tinted in the running
                    // colour - and the countdown, which is shown colour or not
                    var cur = trackManager.currentBeat
                    if (cur > 0)
                    {
                        var secStart = 1
                        var secEnd = total + 1
                        var next = null
                        for (var s = 0; s < sorted.length; s++)
                        {
                            if (sorted[s].beat <= cur) secStart = sorted[s].beat
                            else { secEnd = sorted[s].beat; next = sorted[s]; break }
                        }
                        if (trackEngine && trackEngine.currentColour !== "")
                        {
                            var c = Qt.color(liveRow.swatch(trackEngine.currentColour))
                            ctx.fillStyle = Qt.rgba(c.r, c.g, c.b, 0.10)
                            ctx.fillRect(beatX(secStart, first, count), 7, beatX(cur, first, count) - beatX(secStart, first, count), h - 7)
                        }

                        // the countdown to the next section, in bars
                        if (next && trackManager.playing)
                        {
                            var bars = Math.ceil((next.beat - cur) / 4)
                            if (bars <= 32)
                            {
                                var ncol = Qt.color(trackViewRoot.markerColor(next.type))
                                ctx.font = "bold 26px sans-serif"
                                ctx.textBaseline = "alphabetic"
                                var label = next.type.toUpperCase() + "  " + bars
                                var tw = ctx.measureText(label).width
                                ctx.fillStyle = "rgba(0,0,0,0.55)"
                                ctx.fillRect(w - tw - 24, 14, tw + 16, 36)
                                ctx.fillStyle = Qt.rgba(ncol.r, ncol.g, ncol.b, 1)
                                ctx.fillText(label, w - tw - 16, 42)
                            }
                        }
                    }
                }

                Connections
                {
                    target: trackManager
                    function onTrackChanged() { wfOverlay.selected = -1; wfOverlay.requestPaint() }
                    function onMarkersChanged()
                    {
                        // only drop the selection when the flag is actually
                        // gone. Dropping it on every change meant RETYPE
                        // deselected the flag it had just retyped, so the
                        // four-way cycle could never get past one step.
                        if (wfOverlay.selected >= trackManager.markers.length)
                            wfOverlay.selected = -1
                        wfOverlay.requestPaint()
                    }
                    function onPositionChanged() { wfOverlay.requestPaint() }
                }
                Connections
                {
                    target: trackEngine
                    function onLiveChanged() { wfOverlay.requestPaint() }
                }
                Connections
                {
                    target: trackViewRoot
                    function onZoomActiveChanged() { wfOverlay.requestPaint() }
                    function onZoomCenterChanged() { wfOverlay.requestPaint() }
                }
            }

            // ---- flag tools: a flag on the bar the track is at, the selected
            //      flag retyped or deleted. What the operator sets is the truth -
            //      it goes to BLT's cache as manual and teaches the second pass.
            Item
            {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 8
                width: flagTools.width
                height: flagTools.height
                z: 3
                // ... and out of the way while a verdict is being aimed. The
                // flag tools are ~784 px wide, the blame row with six groups
                // ~788, and they sit in opposite bottom corners of the same
                // 1280 px waveform: both visible at once and they overlap by
                // nearly 300 px. Nobody is putting down a section flag and
                // blaming a look in the same second anyway.
                visible: trackManager && trackManager.beatCount > 0 && trackManager.roleMode
                         && verdictTools.blaming === 0

                // a press that misses a tile must not reach the waveform
                // underneath and clear the selection the tiles depend on
                MouseArea { anchors.fill: parent }

                Row
                {
                    id: flagTools
                    spacing: 6

                    Repeater
                    {
                        // every type the engine understands, so a flag of any
                        // kind can be put down and taken away again by hand.
                        // NORMAL was missing, and rekordbox' phrase analysis
                        // adds INTRO and OUTRO on top of our own four.
                        model: [ "normal", "break", "build", "drop", "intro", "outro" ]
                        TrackTile
                        {
                            objectName: "addFlag:"+modelData
                            width: 82          // room for "+ NORMAL", which was cut to "+ NORMA"
                            height: 44
                            label: "+ " + modelData.toUpperCase()
                            activeColor: trackViewRoot.markerColor(modelData)
                            active: true                  // in its section colour, like the SECTION row
                            opacity: 0.85
                            onTapped: trackManager.addMarker(trackManager.currentBeat > 0 ? trackManager.currentBeat : 1, modelData)
                        }
                    }

                    Item { width: 12; height: 1 }

                    TrackTile
                    {
                        width: 84
                        height: 44
                        objectName: "retypeFlag"
                        label: qsTr("RETYPE")
                        // greyed rather than hidden: hiding these re-flowed the
                        // row and slid UNDO in under the finger that had just
                        // tapped RETYPE
                        enabled: wfOverlay.selected >= 0
                        opacity: enabled ? 0.9 : 0.25
                        onTapped:
                        {
                            var mk = trackManager.markers[wfOverlay.selected]
                            if (mk === undefined) { wfOverlay.selected = -1; return }
                            var order = [ "normal", "break", "build", "drop", "intro", "outro" ]
                            var next = order[(order.indexOf(mk.type) + 1) % order.length]
                            trackManager.setMarkerType(wfOverlay.selected, next)
                        }
                    }

                    TrackTile
                    {
                        width: 84
                        height: 44
                        objectName: "deleteFlag"
                        label: qsTr("DELETE")
                        // greyed rather than hidden: hiding these re-flowed the
                        // row and slid UNDO in under the finger that had just
                        // tapped RETYPE
                        enabled: wfOverlay.selected >= 0
                        opacity: enabled ? 0.9 : 0.25
                        activeColor: "#E36B6B"
                        active: wfOverlay.selected >= 0
                        onTapped: { var i = wfOverlay.selected; wfOverlay.selected = -1; trackManager.removeMarker(i) }
                    }

                    Item { width: 12; height: 1 }

                    // one step back - the flag and the lesson it taught
                    TrackTile
                    {
                        width: 88
                        height: 44
                        objectName: "undoFlag"
                        label: qsTr("UNDO")
                        visible: trackManager ? trackManager.canUndoMarkers : false
                        opacity: 0.9
                        onTapped: { wfOverlay.selected = -1; trackManager.undoMarkers() }
                    }
                }
            }

            // ---- the verdict: two thumbs, bottom right, opposite the flag
            //      tools. Deliberately NOT in the live row - that row is full
            //      to the pixel on a 1280 screen - and deliberately in a
            //      corner that never moves, so they can be hit without
            //      looking away from the floor.
            //
            //      A thumb is counted against every program on stage (a long
            //      press aims it at one group), written to the tracklog when
            //      the log is on, and - with RATINGS on in SETUP - it steers
            //      the rotation. The count does not need the log, so the
            //      buttons no longer hide when the log is off: they used to,
            //      from the days when the log was all a thumb did.
            Item
            {
                id: verdictTools
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 8
                z: 3
                visible: trackManager && trackEngine && trackManager.beatCount > 0
                         && !trackViewRoot.setupOpen

                // 0 = the thumbs; +1 / -1 = a thumb is waiting for a target.
                // A long press opens the list of what is on stage; the tap
                // that follows puts the whole verdict on that one group
                // instead of spreading it over everything.
                property int blaming: 0
                property var stageRows: []

                width: blaming === 0 ? thumbRow.width : blameRow.width
                height: blaming === 0 ? thumbRow.height : blameRow.height

                MouseArea { anchors.fill: parent }

                Row
                {
                    id: thumbRow
                    spacing: 8
                    visible: verdictTools.blaming === 0

                    Repeater
                    {
                        model: [ 1, -1 ]

                        Rectangle
                        {
                            id: thumb
                            objectName: "rating:"+modelData
                            width: 62
                            height: 44
                            radius: 6
                            // a press flashes the tile for a moment: the only
                            // receipt there is, since nothing else changes
                            property bool lit: false
                            color: lit ? (modelData > 0 ? "#3E7E4E" : "#8E3A3A") : "#1E1E1E"
                            border.width: 1
                            border.color: modelData > 0 ? "#4FA36B" : "#B05050"
                            opacity: 0.9

                            Canvas
                            {
                                anchors.centerIn: parent
                                width: 26
                                height: 26
                                rotation: modelData > 0 ? 0 : 180
                                onPaint:
                                {
                                    // a thumb, drawn rather than shipped: no
                                    // icon file, no qrc entry, and it scales
                                    var c = getContext("2d")
                                    c.reset()
                                    c.fillStyle = modelData > 0 ? "#9FD8AF" : "#E8A0A0"
                                    // the fist
                                    c.fillRect(3, 12, 8, 12)
                                    // the palm and the thumb over it
                                    c.beginPath()
                                    c.moveTo(12, 24)
                                    c.lineTo(12, 13)
                                    c.lineTo(16, 3)
                                    c.quadraticCurveTo(19, 1, 19, 5)
                                    c.lineTo(17, 11)
                                    c.lineTo(23, 11)
                                    c.quadraticCurveTo(25, 11, 24, 14)
                                    c.lineTo(22, 22)
                                    c.quadraticCurveTo(21, 24, 19, 24)
                                    c.closePath()
                                    c.fill()
                                }
                            }

                            MouseArea
                            {
                                anchors.fill: parent
                                // Qt does not agree with itself across versions
                                // about whether clicked follows pressAndHold.
                                // A flag costs nothing and settles it.
                                property bool held: false
                                onPressed:
                                {
                                    held = false
                                    // The instant the finger lands. Everything
                                    // after this - the 800 ms until a long
                                    // press registers, the seconds spent
                                    // choosing a group, the look changing
                                    // because a thumb down was given - reads
                                    // from this moment, not from whatever is
                                    // on stage when he lets go.
                                    if (trackEngine) trackEngine.markVerdictPoint()
                                }
                                onPressAndHold:
                                {
                                    if (trackEngine === null) return
                                    var rows = trackEngine.onStage()
                                    if (rows.length === 0) return
                                    held = true
                                    verdictTools.stageRows = rows
                                    verdictTools.blaming = modelData
                                    blameTimeout.restart()
                                }
                                onClicked:
                                {
                                    if (held) { held = false; return }
                                    if (trackEngine) trackEngine.rate(modelData)
                                    thumb.lit = true
                                    flash.restart()
                                }
                            }

                            Timer
                            {
                                id: flash
                                interval: 220
                                onTriggered: thumb.lit = false
                            }
                        }
                    }
                }

                // The list a long press opens. One tile per group that has a
                // look, named by the group because that is what he is looking
                // at - not by a function he would have to recognise. Tap one
                // and the verdict goes there alone; tap the cross, or wait,
                // and nothing happened.
                Row
                {
                    id: blameRow
                    spacing: 6
                    visible: verdictTools.blaming !== 0

                    // Six groups at 118 px is 788, which fits a 1280 waveform
                    // with room to spare. A rig with ten would not, and the
                    // row is anchored right - so it would run off the left
                    // edge rather than wrap, and the first groups would be
                    // unreachable. Same lesson as the colour row: give it the
                    // space there is and let the tiles shrink into it.
                    property int cells: Math.max(1, verdictTools.stageRows.length)
                    property real cellW: Math.max(64,
                                            Math.min(118,
                                                (verdictTools.parent.width - 70 - cells * spacing)
                                                / cells))

                    Repeater
                    {
                        model: verdictTools.stageRows

                        TrackTile
                        {
                            width: blameRow.cellW
                            height: 44
                            label: modelData.group
                            active: true
                            activeColor: verdictTools.blaming > 0 ? "#3E7E4E" : "#8E3A3A"
                            opacity: 0.92
                            onTapped:
                            {
                                if (trackEngine)
                                    trackEngine.rateGroup(verdictTools.blaming, modelData.group)
                                verdictTools.blaming = 0
                                blameTimeout.stop()
                            }
                        }
                    }

                    TrackTile
                    {
                        width: 44
                        height: 44
                        label: "\u00d7"
                        opacity: 0.7
                        onTapped: { verdictTools.blaming = 0; blameTimeout.stop() }
                    }
                }

                // A list left open in the dark is a trap for the next finger
                Timer
                {
                    id: blameTimeout
                    interval: 6000
                    onTriggered: verdictTools.blaming = 0
                }
            }

            Canvas
            {
                id: wfCanvas
                anchors.fill: parent
                anchors.margins: 1
                anchors.bottomMargin: 56
                renderStrategy: Canvas.Threaded

                onPaint:
                {
                    var ctx = getContext("2d")
                    var w = width, h = height

                    ctx.reset()
                    ctx.fillStyle = "#101010"
                    ctx.fillRect(0, 0, w, h)

                    var n = trackViewRoot.beatCount
                    if (n <= 0) return

                    var vf = trackViewRoot.viewFirst()
                    var vc = trackViewRoot.viewCount()
                    var px = w / vc
                    var wf = trackManager.waveform
                    var lane = Math.min(48, Math.round(h * 0.32))
                    var base = h - 4

                    function xOf(beat) { return (beat - vf) * px }

                    ctx.fillStyle = "#2E6DA4"
                    for (var i = 0; i < vc; i++)
                    {
                        var b = vf + i
                        if (b < 1 || b > n) continue
                        var v = ((b - 1) < wf.length ? wf[b - 1] : 0) / 255.0
                        var bh = Math.max(1, v * (base - lane))
                        ctx.fillRect(i * px, base - bh, Math.max(1, px), bh)
                    }

                    var gridStep = trackViewRoot.zoomActive ? 4 : 32
                    ctx.strokeStyle = "rgba(255,255,255,0.12)"
                    ctx.lineWidth = 1
                    for (var g = Math.ceil(vf / gridStep) * gridStep; g < vf + vc; g += gridStep)
                    {
                        ctx.beginPath()
                        ctx.moveTo(xOf(g), lane)
                        ctx.lineTo(xOf(g), base)
                        ctx.stroke()
                    }

                    // Two label rows: a label drops to the second row when it
                    // would collide with the previous one, so close markers stay
                    // readable instead of printing on top of each other.
                    var mk = trackManager.markers
                    var rowH = Math.floor((lane - 4) / 2)
                    var rowRight = [ -1e9, -1e9 ]

                    for (var m = 0; m < mk.length; m++)
                    {
                        var mb = mk[m].beat
                        if (mb < vf - 2 || mb > vf + vc + 2) continue

                        var mx = xOf(mb)
                        var col = trackViewRoot.markerColor(mk[m].type)
                        var label = mk[m].type.toUpperCase() + (mk[m].energy >= 0 ? " " + Math.round(mk[m].energy * 100) + "%" : "")
                        var held = (m === trackViewRoot.dragIndex)

                        ctx.strokeStyle = col
                        ctx.lineWidth = held ? 4 : 2
                        ctx.beginPath()
                        ctx.moveTo(mx, 0)
                        ctx.lineTo(mx, base)
                        ctx.stroke()

                        ctx.font = "bold 11px sans-serif"
                        var tw = ctx.measureText(label).width + 10
                        var bx = Math.min(Math.max(mx, 0), w - tw)

                        var labelRow = (bx < rowRight[0] + 3) ? 1 : 0
                        if (labelRow === 1 && bx < rowRight[1] + 3)
                            labelRow = 0          // both taken: overlap the older one
                        rowRight[labelRow] = bx + tw

                        var ly = labelRow * rowH

                        ctx.fillStyle = col
                        ctx.fillRect(bx, ly, tw, rowH - 2)
                        ctx.fillStyle = "#000000"
                        ctx.fillText(label, bx + 5, ly + rowH - 6)

                        if (held)
                        {
                            ctx.fillStyle = col
                            ctx.font = "bold 10px sans-serif"
                            ctx.fillText("beat " + mb, bx + 5, lane + 12)
                        }
                    }

                    var cb = trackViewRoot.currentBeat
                    if (cb > 0 && cb >= vf && cb < vf + vc)     // the view ends one beat before
                    {
                        var ph = xOf(cb)
                        ctx.strokeStyle = "#FFFFFF"
                        ctx.lineWidth = 2
                        ctx.beginPath()
                        ctx.moveTo(ph, 0)
                        ctx.lineTo(ph, h)
                        ctx.stroke()

                        ctx.fillStyle = "#FFFFFF"
                        ctx.beginPath()
                        ctx.moveTo(ph - 7, h)
                        ctx.lineTo(ph + 7, h)
                        ctx.lineTo(ph, h - 10)
                        ctx.closePath()
                        ctx.fill()
                    }
                }
            }

            MouseArea
            {
                id: wfArea
                objectName: "waveformInput"
                anchors.fill: parent
                anchors.margins: 1
                anchors.bottomMargin: 56          // the canvases are inset by one: pick where we draw
                enabled: trackViewRoot.beatCount > 0

                // A finger on a flag: press = select it (RETYPE / DELETE appear),
                // move = drag it. The flag keeps its distance to the finger and
                // the zoom opens around the finger, so nothing jumps.
                property int pressIndex: -1
                property real pressX: 0

                function beatAt(mx)
                {
                    return Math.round(mx / (width / trackViewRoot.viewCount()))
                           + trackViewRoot.viewFirst()
                }

                onPressed: function (mouse)
                {
                    var b = beatAt(mouse.x)
                    var mk = trackManager.markers
                    var best = -1, bestDist = 1e9
                    for (var i = 0; i < mk.length; i++)
                    {
                        var d = Math.abs(mk[i].beat - b)
                        if (d < bestDist) { bestDist = d; best = i }
                    }
                    pressIndex = (best >= 0 && bestDist <= Math.max(3, trackViewRoot.viewCount() * 0.03)) ? best : -1
                    pressX = mouse.x
                    trackViewRoot.dragIndex = -1
                    if (typeof wfOverlay !== "undefined")
                    {
                        wfOverlay.selected = pressIndex
                        wfOverlay.requestPaint()
                    }
                }

                // moveMarker may drop the flag we land on: everyone's index
                // shifts, so find the dragged flag again by the beat we asked for
                function reindex(wantBeat)
                {
                    // moveMarker snapped the flag to a bar line: look for it
                    // there, or a neighbour on the next bar can be nearer to
                    // the raw beat and the drag jumps to the wrong flag
                    var snapped = Math.max(1, Math.floor((wantBeat - 1 + 2) / 4) * 4 + 1)
                    var mk = trackManager.markers
                    var best = -1, bd = 1e9
                    for (var i = 0; i < mk.length; i++)
                    {
                        var d = Math.abs(mk[i].beat - snapped)
                        if (d < bd) { bd = d; best = i }
                    }
                    pressIndex = best
                    trackViewRoot.dragIndex = best
                    if (typeof wfOverlay !== "undefined")
                        wfOverlay.selected = best
                }

                onPositionChanged: function (mouse)
                {
                    if (pressIndex < 0) return
                    if (trackViewRoot.dragIndex < 0)
                    {
                        if (Math.abs(mouse.x - pressX) < 6) return
                        // the drag begins: zoom in with the flag staying under the finger
                        var mk = trackManager.markers[pressIndex]
                        trackViewRoot.dragIndex = pressIndex
                        trackViewRoot.zoomActive = true
                        var vc = trackViewRoot.viewCount()
                        trackViewRoot.zoomCenter = Math.round(mk.beat - mouse.x / width * vc + vc / 2)
                        trackViewRoot.dragOffset = mk.beat - beatAt(mouse.x)
                    }
                    trackViewRoot.dragX = mouse.x
                    var want = beatAt(mouse.x) + trackViewRoot.dragOffset
                    var before = trackManager.markers.length
                    trackManager.moveMarker(trackViewRoot.dragIndex, want)
                    if (trackManager.markers.length !== before)
                        reindex(want)

                    var edge = width * 0.08
                    panTimer.dir = mouse.x < edge ? -1 : (mouse.x > width - edge ? 1 : 0)
                    panTimer.running = (panTimer.dir !== 0)
                    wfCanvas.requestPaint()
                }

                function release()
                {
                    panTimer.running = false
                    panTimer.dir = 0
                    pressIndex = -1
                    trackViewRoot.dragIndex = -1
                    trackViewRoot.zoomActive = false
                    wfCanvas.requestPaint()
                }

                onReleased: release()
                onCanceled: release()
            }
            Text
            {
                anchors.centerIn: parent
                visible: trackViewRoot.beatCount === 0
                text: qsTr("Waiting for track data from Beat Link Trigger...")
                color: trackViewRoot.cDim
                font.pixelSize: 15
            }
        }
Rectangle
        {
            // SECTION is built like COLOUR now (runde 141): the heading on
            // its own line at the top left, the buttons in a row underneath,
            // 48 tall - the same shape, the same height, the same margins.
            // Two rows of the same kind of choice should not be laid out two
            // different ways, and until now SECTION had its title inline on
            // the left, which pushed its AUTO one label's width to the right
            // of the AUTO in COLOUR directly below it. Now they line up
            // because they are the same thing built the same way.
            Layout.fillWidth: true
            Layout.preferredHeight: trackViewRoot.compactLayout ? 88 : 112
            Layout.minimumHeight: trackViewRoot.compactLayout ? 88 : 112
            Layout.maximumHeight: trackViewRoot.compactLayout ? 88 : 112
            color: trackViewRoot.cPanel
            radius: 4

            Text { x: 12; y: 8; text: "SECTION"; color: trackViewRoot.cText
                   font.pixelSize: trackViewRoot.compactLayout ? 16 : 20 }

            RowLayout
            {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.bottom: parent.bottom; anchors.margins: 10
                height: 48
                spacing: 6

                // AUTO, not FOLLOW, and on the LEFT (runde 140, Tobias:
                // "maaske hedde AUTO ligesom paa farverne, og saa rykke den
                // over paa den anden side af sektionerne saa den passer med
                // AUTO paa farverne"). Same word, same sun, same green, same
                // corner as the colour row directly below it - so the page
                // has one idea of "let the engine decide" instead of two
                // words for it in two places.
                TrackTile
                {
                    // exactly as wide as AUTO in the COLOUR row below, by
                    // asking that row rather than guessing a number: both
                    // panels start at the same x, so binding the width makes
                    // the two buttons line up to the pixel and keeps them
                    // lined up if the palette ever gains or loses a colour
                    Layout.preferredWidth: colourRow.cellW
                    Layout.fillHeight: true
                    objectName: "followMusic"
                    label: qsTr("AUTO")
                    active: trackManager ? trackManager.overrideState === "" : true
                    activeColor: "#7ED07E"
                    onTapped: trackManager.overrideState = ""

                    ControlIcon
                    {
                        x: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16; height: 16
                        ink: (trackManager && trackManager.overrideState === "") ? "#101010" : "#DDDDDD"
                        kind: "revert"
                    }
                }

                Repeater
                {
                    model: trackViewRoot.states

                    Button
                    {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        objectName: "section:"+modelData
                        // not checkable: a click would write 'checked' and
                        // break the binding, leaving two sections lit
                        checked: trackManager ? trackManager.overrideState === modelData : false
                        onClicked: trackManager.overrideState =
                                   (trackManager.overrideState === modelData) ? "" : modelData

                        contentItem: Text
                        {
                            text: modelData.toUpperCase()
                            color: (parent.checked || trackViewRoot.liveState === modelData)
                                   ? "#000000" : trackViewRoot.cText
                            font.bold: true
                            font.pixelSize: 19
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle
                        {
                            radius: 5
                            color: parent.checked
                                   ? trackViewRoot.markerColor(modelData)
                                   : (trackViewRoot.liveState === modelData
                                      ? Qt.darker(trackViewRoot.markerColor(modelData), 1.15)
                                      : trackViewRoot.cBtn)
                            border.width: parent.checked ? 3 : 1
                            border.color: parent.checked ? "#FFFFFF" : trackViewRoot.cLine
                        }
                    }
                }


                // the evening's opening picture: the START scene on its
                // own, engine standing still. Switching the show on
                // takes it off again


                // the show switch: the biggest thing in the row, red
                // when the engine is not running, green when it is

            }
        }
RowLayout {
            id: dialsRow
            Layout.fillWidth: true; Layout.fillHeight: false
            // THREE BOXES, each saying what it is (runde 140). It was two:
            // ENERGY, and one called MASTER DIMMER that also held SPEED -
            // two unrelated controls under one name, which is what Tobias
            // caught ("den skal ikke hedde masterdimmer og saa ogsaa have
            // speed i den"). Splitting them is the whole fix: nothing needs
            // a name that covers both, because nothing shares a box.
            //
            // And ENERGY is no longer the big one. It is set once and left
            // ("Den kommer nok ikke til at blive rykket saa ofte"), so it is
            // the same size as the others now. A fixed height again: title
            // 20 + 6 + fader 66 + margins 20 = 112, and nothing grows.
            Layout.preferredHeight: trackViewRoot.compactLayout ? 104 : 112
            Layout.minimumHeight: trackViewRoot.compactLayout ? 104 : 112
            Layout.maximumHeight: trackViewRoot.compactLayout ? 104 : 112
            spacing: 10
            visible: trackManager && trackEngine && trackManager.roleMode && !trackViewRoot.setupOpen
Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.preferredWidth: dialsRow.width * 0.34
                color: trackViewRoot.cPanel; radius: 4; border.color: trackViewRoot.cLine
                ColumnLayout { anchors.fill: parent; anchors.margins: 10; spacing: 6
                    // "MASTER DIMMER", not "MASTER" - it is the room's
                    // brightness, and the word alone read like a master
                    // section. (Tobias, 2026-09-22.) The fillHeight spacer
                    // that sat under it is gone with it: it was there to push
                    // the fader down into a box that had no reason to be tall.
                    Text { text: "MASTER DIMMER"; color: trackViewRoot.cText
                           font.pixelSize: trackViewRoot.compactLayout ? 15 : 17; font.bold: true }
Rectangle
            {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 4
                color: "#141414"
                border.width: 1
                border.color: "#555555"

                Rectangle
                {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.margins: 3
                    width: (parent.width - 6) * (trackEngine ? trackEngine.master : 1)
                    radius: 3
                    color: "#4FA3E3"
                }

                SliderTicks { anchors.fill: parent }
                SliderGrip
                {
                    property real lvl: trackEngine ? trackEngine.master : 1
                    x: Math.max(1, Math.min(parent.width - width - 1,
                                            3 + (parent.width - 6) * lvl - width / 2))
                    height: parent.height
                    ink: "#9FD3FF"
                    pressed: masterArea.pressed
                }

                Text
                {
                    anchors.centerIn: parent
                    text: qsTr("MASTER DIMMER") + "  " + Math.round((trackEngine ? trackEngine.master : 1) * 100) + "%"
                    color: "#EEEEEE"
                    font.bold: true
                    font.pixelSize: 15
                }

                MouseArea
                {
                    id: masterArea
                    objectName: "masterDrag"
                    anchors.fill: parent
                    function apply(x) { if (trackEngine) trackEngine.master = Math.max(0, Math.min(1, (x - 3) / (width - 6))) }
                    onPressed: (mouse) => apply(mouse.x)
                    onPositionChanged: (mouse) => { if (pressed) apply(mouse.x) }
                }
            }


                }
            }
            Rectangle {
                // SPEED is its own box now. It is not a dimmer and it is not
                // a master of anything - it is how fast the engine runs the
                // figures - so it gets its own name and its own frame.
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.preferredWidth: dialsRow.width * 0.26
                color: trackViewRoot.cPanel; radius: 4; border.color: trackViewRoot.cLine
                ColumnLayout { anchors.fill: parent; anchors.margins: 10; spacing: 6
                    Text { text: "SPEED"; color: trackViewRoot.cText
                           font.pixelSize: trackViewRoot.compactLayout ? 15 : 17; font.bold: true }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 6
                        Repeater
                        {
                            model: [ "\u00bd\u00d7", "1\u00d7", "2\u00d7" ]

                            TrackTile
                            {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                objectName: "speed"+index
                                label: modelData
                                active: trackEngine ? trackEngine.speed === index - 1 : index === 1
                                activeColor: [ "#5A7A9A", "#4FA3E3", "#E3B44F" ][index]
                                onTapped: trackEngine.speed = index - 1
                            }
                        }
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.preferredWidth: dialsRow.width * 0.40
                color: trackViewRoot.cPanel; radius: 4; border.color: trackViewRoot.cLine
                ColumnLayout { anchors.fill: parent; anchors.margins: 10; spacing: 6
                    // Just the title. The 62-pixel percentage that used to sit
                    // in the middle of this box is gone (Tobias, 2026-09-22:
                    // "Energi har alt for meget tomt plads med den store
                    // procent tegn, det skal fjernes") - it said the same
                    // number as the fader directly below it, and the Item it
                    // was centred in was a fillHeight spacer, so the box was
                    // mostly air to make room for one duplicate figure.
                    Text { Layout.fillWidth: true; text: "ENERGY"; color: trackViewRoot.cText
                           font.pixelSize: trackViewRoot.compactLayout ? 15 : 17; font.bold: true }
Rectangle
            {
                Layout.fillWidth: true
                // The fader FILLS the box (runde 139). Taking the big
                // percentage out left the box with a title and a 62-pixel
                // bar in 148 pixels of space - the air moved rather than
                // went away. The fader takes it instead, which also makes
                // the one control Tobias calls "rimelig essentiel" the
                // easiest thing on the page to hit.
                Layout.fillHeight: true
                radius: 4
                color: "#141414"
                border.width: 1
                border.color: "#555555"

                objectName: "energy"
                property real trim: trackManager ? Math.min(1, trackManager.energyTrim / 100) : 0.5

                Rectangle
                {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    anchors.margins: 3
                    width: (parent.width - 6) * parent.trim
                    radius: 3
                    color: "#E3B44F"
                }

                SliderTicks { anchors.fill: parent }
                SliderGrip
                {
                    x: Math.max(1, Math.min(parent.width - width - 1,
                                            3 + (parent.width - 6) * parent.trim - width / 2))
                    height: parent.height
                    ink: "#F6D98A"; pressed: energyArea.pressed
                }

                Text
                {
                    anchors.centerIn: parent
                    text: qsTr("ENERGY") + "  " + (trackManager ? Math.round(Math.min(100, trackManager.energyTrim)) : 50) + "%"
                    color: "#EEEEEE"
                    font.bold: true
                    font.pixelSize: 15
                }

                MouseArea
                {
                    id: energyArea
                    objectName: "energyDrag"
                    anchors.fill: parent
                    function apply(x)
                    {
                        // the fill is inset three pixels: read the finger the same
                        // way, or full is unreachable at the right edge
                        var v = Math.round(Math.max(0, Math.min(1, (x - 3) / (width - 6))) * 100)
                        if (trackManager) trackManager.energyTrim = v
                    }
                    // a hand on the bar takes over from the clock - also when it
                    // lands exactly where the clock already put it (the setter
                    // only infers a touch from a CHANGE of value)
                    onPressed: (mouse) => { if (trackEngine && trackEngine.roomAuto) trackEngine.roomAuto = false; apply(mouse.x) }
                    onPositionChanged: (mouse) => { if (pressed) apply(mouse.x) }
                }
            }
                }
            }

        }
RowLayout {
            id: liveRow
            Layout.fillWidth: true; Layout.minimumHeight: trackViewRoot.compactLayout ? 88 : 112; Layout.preferredHeight: trackViewRoot.compactLayout ? 88 : 112
            Layout.maximumHeight: trackViewRoot.compactLayout ? 88 : 112
            spacing: 10
            visible: trackManager && trackEngine && trackManager.roleMode && !trackViewRoot.setupOpen
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
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: liveRow.width * 0.65
                color: trackViewRoot.cPanel; radius: 4; border.color: trackViewRoot.cLine
                Text { x: 12; y: 8; text: "COLOUR"; color: trackViewRoot.cText; font.pixelSize: trackViewRoot.compactLayout ? 16 : 20 }
Row
            {
                id: colourRow
                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 10
                height: 48
                spacing: 6

                property int cells: 1 + (trackEngine ? trackEngine.palette.length : 0)
                property real cellW: Math.max(48, (width - spacing * (cells - 1)) / cells)

                TrackTile
                {
                    width: colourRow.cellW
                    height: colourRow.height
                    objectName: "autoColour"
                ControlIcon { x: 6; anchors.verticalCenter: parent.verticalCenter; ink: "#101010";width: 16; height: 16; kind: "autoColour" }
                    label: qsTr("AUTO")
                    active: trackEngine ? trackEngine.colourOverride === "" : true
                    activeColor: "#7ED07E"
                    onTapped: trackEngine.colourOverride = ""
                }

                Repeater
                {
                    model: trackEngine ? trackEngine.palette : []

                    // lit = locked to this colour. A ring only = this is what
                    // AUTO happens to be running right now.
                    TrackTile
                    {
                        width: colourRow.cellW
                        height: colourRow.height
                        objectName: "colour:"+modelData
                        label: modelData.toUpperCase()
                        activeColor: liveRow.swatch(modelData)
                        active: trackEngine ? trackEngine.colourOverride === modelData : false
                        border.width: (trackEngine && trackEngine.colourOverride === ""
                                       && trackEngine.currentColour === modelData) ? 3 : 1
                        border.color: (trackEngine && trackEngine.currentColour === modelData)
                                      ? liveRow.swatch(modelData) : "#555555"
                        onTapped: trackEngine.colourOverride =
                                      (trackEngine.colourOverride === modelData) ? "" : modelData
                    }
                }
            }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: liveRow.width * 0.35
                color: trackViewRoot.cPanel; radius: 4; border.color: trackViewRoot.cLine
                Text { x: 12; y: 8; text: "INTERVENTION"; color: trackViewRoot.cText; font.pixelSize: trackViewRoot.compactLayout ? 16 : 20 }
                RowLayout { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 10; height: 48; spacing: 8
TrackTile
            {
                Layout.fillWidth: true
                Layout.fillHeight: true
                objectName: "calm"
                ControlIcon { x: 6; anchors.verticalCenter: parent.verticalCenter;  kind: "calm" }
                label: (trackEngine && trackEngine.calmBarsLeft > 0)
                       ? qsTr("CALM") + " " + trackEngine.calmBarsLeft : qsTr("CALM")
                active: trackEngine ? trackEngine.calmBarsLeft > 0 : false
                activeColor: "#4FA3E3"
                onTapped: trackEngine.calm(trackEngine.calmBarsLeft > 0 ? 0 : 16)
            }
TrackTile
            {
                Layout.fillWidth: true
                Layout.fillHeight: true
                objectName: "hold"
                ControlIcon { x: 6; anchors.verticalCenter: parent.verticalCenter;  kind: "hold" }
                label: qsTr("HOLD")
                active: trackEngine ? trackEngine.hold : false
                activeColor: "#E3B44F"
                onTapped: trackEngine.hold = !trackEngine.hold
            }
TrackTile
            {
                Layout.fillWidth: true
                Layout.fillHeight: true
                objectName: "nextLook"
                ControlIcon { x: 6; anchors.verticalCenter: parent.verticalCenter;  kind: "nextLook" }
                label: qsTr("NEXT LOOK")
                active: false
                onTapped: trackEngine.next()
            }
                }
            }
        }

Rectangle
        {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 30 : 0
            radius: 3
            color: "#3A2A1A"
            border.width: 1
            border.color: "#E3B44F"
            visible: trackManager && trackManager.roleMode && !trackViewRoot.setupOpen
                     && warnText.text.length > 0

            Text
            {
                id: warnText
                anchors.fill: parent
                anchors.margins: 6
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                color: "#FFD27F"
                font.pixelSize: 13
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
Rectangle
        {
            Layout.fillWidth: true
            // no fillHeight: the waveform is the one row that grows
            Layout.fillHeight: false
            // Lower cards (runde 139, Tobias: "Grupperne må også gerne være
            // lidt lavere"). 24 for the title, then one card: header row 46,
            // gap, fader 40, margins - 124. The old 210 was carrying a
            // separate status line and a 40-pixel ON/OFF button stacked
            // under the name, with dead space between them.
            Layout.preferredHeight: trackViewRoot.compactLayout ? 132 : 144
            Layout.minimumHeight: trackViewRoot.compactLayout ? 132 : 144
            Layout.maximumHeight: trackViewRoot.compactLayout ? 132 : 144
            color: trackViewRoot.cPanel
            radius: 4

            Text { x: 12; y: 6; text: "LIGHT GROUPS"; color: trackViewRoot.cText; font.pixelSize: 18 }
            // The cast: the groups side by side across the panel, exactly
            // where they have always been - but each one's fader now fills
            // from the LEFT (0 %) to the RIGHT (100 %), the way MASTER and
            // ENERGY do, instead of from the bottom up. The DJ's trim sits on
            // top of everything the engine does, and the switch in the top
            // right corner leaves a group out for the night.
            // A group whose dimmer is a switch (an animation laser) gets no
            // fader at all: the whole row is its on/off button, because a
            // fader that only has two positions is a lie.
            // (CAST_V10_MD_GUARD)
            Column
            {
                id: castPanel
                anchors.fill: parent
                anchors.margins: 6
                anchors.topMargin: 30
                spacing: 4
                visible: !trackViewRoot.setupOpen

                Row
                {
                    id: castRow
                    // once, here - not once per tile. trackEngine.groups is a
                    // full rebuild of the function table, not a cheap getter.
                    property int n: trackEngine ? Math.max(1, trackEngine.groups.length) : 1
                    // ... and the same for the other two getters that are not
                    // cheap either (runde 142). trims() builds a QVariantMap
                    // over every group on each call and cast() copies a list
                    // and SORTS it; the delegate below asked for trims twice
                    // and cast once, so five tiles came to ten map rebuilds
                    // and five sorts every time a trim moved or the cast
                    // changed - and a finger dragging a fader changes the
                    // trim continuously. Read once here, per change, and the
                    // tiles read these.
                    property var allTrims: trackEngine ? trackEngine.trims : ({})
                    property var litNow: trackEngine ? trackEngine.cast : []
                    width: parent.width
                    height: parent.height
                    spacing: 6

                    Repeater
                    {
                        model: trackEngine ? trackEngine.groups : []

                        Rectangle
                        {
                            id: castTile
                            objectName: "groupTrim:"+md.key
                            ControlIcon {
                                x: 10; y: 10; width: trackViewRoot.compactLayout ? 28 : 34; height: width; z: 2
                                ink: castTile.off ? "#666666" : "#CCCCCC"
                                kind: md.switchOnly ? "animation" : md.strobes ? "strobe" : md.lasers ? "laser" : md.key.toLowerCase().indexOf("eyes") >= 0 ? "eyes" : "head"
                            }
                            // same fallback as the track list: a groups rebuild
                            // re-evaluates every tile binding with modelData gone
                            property var md: modelData ? modelData
                                                       : ({ key: "", enabled: true,
                                                            switchOnly: false, base: false })
                            property bool lit: castRow.litNow.indexOf(md.key) >= 0
                            property bool off: !md.enabled
                            property bool switchOnly: md.switchOnly === true
                            property real trim: castRow.allTrims[md.key] !== undefined
                                                ? castRow.allTrims[md.key] : 1.0
                            width: (castRow.width - (castRow.n - 1) * castRow.spacing) / castRow.n
                            height: castRow.height
                            radius: 6
                            color: off ? "#161616" : "#1E1E1E"
                            border.width: md.base ? 2 : 1
                            border.color: lit ? "#9FD3FF" : (md.base ? "#4FA3E3" : "#3A3A3A")
                            clip: true

                            // the scale: 25, 50, 75 % as ticks along the top and
                            // bottom edges, the way a horizontal fader is read
                            Repeater
                            {
                                model: castTile.switchOnly ? [] : [ 0.25, 0.5, 0.75 ]
                                Item
                                {
                                    x: 3 + (castTile.width - 6) * modelData - 1
                                    y: 0
                                    width: 2
                                    height: 52
                                    anchors.bottom: parent.bottom
                                    Rectangle { y: 0; width: 2; height: 8; color: "#4A4A4A" }
                                    Rectangle { y: parent.height - 8; width: 2; height: 8; color: "#4A4A4A" }
                                }
                            }

                            // the fader: the trim fills from the LEFT, with a
                            // bright edge where the level stands
                            Rectangle
                            {
                                id: castFill
                                visible: !castTile.switchOnly
                                anchors.left: parent.left
                                                                anchors.bottom: parent.bottom
                                anchors.margins: 3
                                height: 46
                                width: (parent.width - 6) * (castTile.off ? 0 : castTile.trim)
                                radius: 4
                                color: castTile.lit ? (md.base ? "#2E6FA8" : "#3D86C4")
                                                    : (castArea.pressed ? "#3A3A3A" : "#303030")
                                Behavior on color { ColorAnimation { duration: 150 } }

                                // the grip, at the level - the same handle
                                // the big faders have, so the gesture is the
                                // same one everywhere on the page
                                SliderGrip
                                {
                                    anchors.right: parent.right
                                    anchors.rightMargin: -width / 2
                                    anchors.verticalCenter: parent.verticalCenter
                                    height: parent.height + 4
                                    visible: !castTile.off
                                    ink: castTile.lit ? "#BFE3FF" : "#B0B0B0"
                                    pressed: castArea.pressed
                                    z: 3
                                }
                            }

                            Text {
                                anchors.bottom: parent.bottom; anchors.bottomMargin: 17
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: Math.round(castTile.trim * 100) + "%"
                                visible: !castTile.switchOnly
                                color: "#EEEEEE"; font.pixelSize: 13; z: 2
                            }
                            // an on/off group fills its whole row when it is on -
                            // there is nothing in between to show
                            Rectangle
                            {
                                visible: castTile.switchOnly && !castTile.off
                                anchors.fill: parent
                                anchors.margins: 3
                                radius: 4
                                color: castTile.lit ? (md.base ? "#2E6FA8" : "#3D86C4") : "#303030"
                                Behavior on color { ColorAnimation { duration: 150 } }
                            }

                            // drag anywhere: the trim. A switch-only group toggles
                            // instead - one tap, on or off.
                            MouseArea
                            {
                                id: castArea
                                objectName: "groupTrim:"+md.key+"Drag"
                                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                                height: castTile.switchOnly ? parent.height : 52
                                // the base group is always in the show, switch-only
                                // or not - the comment on the switch below says so
                                // and this is the path that could have broken it
                                enabled: (castTile.switchOnly && !md.base) || !castTile.off
                                // The switch is in the header; this MouseArea covers only the trim.
                                property bool hasSwitch: false
                                function onSwitch(x, y)
                                {
                                    return hasSwitch && width > 96 && x > width - 69 && y < 41
                                }
                                function apply(x)
                                {
                                    var v = (x - 3) / (width - 6)
                                    v = Math.max(0, Math.min(1, v))
                                    if (v > 0.97) v = 1
                                    if (v < 0.03) v = 0
                                    if (trackEngine) trackEngine.setGroupTrim(md.key, v)
                                }
                                // decided on the press alone: a drag that began on
                                // the fader keeps working when the finger wanders
                                // into the halo - testing every move froze the
                                // fader at about half on the right-hand side
                                property bool dragging: false
                                onPressed: (mouse) =>
                                {
                                    dragging = false
                                    if (castTile.switchOnly)
                                        return
                                    if (!onSwitch(mouse.x, mouse.y))
                                    {
                                        dragging = true
                                        apply(mouse.x)
                                    }
                                }
                                onPositionChanged: (mouse) =>
                                {
                                    if (pressed && dragging)
                                        apply(mouse.x)
                                }
                                onReleased: dragging = false
                                onClicked: (mouse) =>
                                {
                                    if (castTile.switchOnly && !md.base && trackEngine)
                                        trackEngine.setGroupEnabled(md.key, castTile.off)
                                }
                            }

                            Column
                            {
                                // room kept free on the right for the toggle
                                x: trackViewRoot.compactLayout ? 44 : 52; y: 9
                                width: Math.max(24, parent.width - x - (md.base ? 10 : 68))
                                spacing: 2

                                Text
                                {
                                    width: parent.width
                                    elide: Text.ElideRight
                                    text: md.key.toUpperCase()
                                    color: castTile.lit ? "#FFFFFF" : (castTile.off ? "#444444" : "#8A8A8A")
                                    font.bold: true
                                    font.pixelSize: 14
                                }
                                Text
                                {
                                    width: parent.width
                                    elide: Text.ElideRight
                                    text: castTile.off ? qsTr("OFF")
                                        : castTile.switchOnly ? qsTr("ON")
                                        : (md.base ? qsTr("BASE") + "  " : "")
                                          + Math.round(castTile.trim * 100) + "%"
                                    color: castTile.lit ? "#E0F0FF" : "#6A6A6A"
                                    font.bold: castTile.switchOnly
                                    font.pixelSize: 12
                                }
                            }

                            // The second status line is gone (runde 139). It
                            // read "BASE · READY" against the line under the
                            // name that already says BASE and the level, and
                            // holding it took a 64-pixel gap above the fader.
                            // What it alone carried - that a group is lit
                            // RIGHT NOW - is now the name's colour and the
                            // card's border, which it always was as well.
                            // the switch: in or out of tonight's show. The base
                            // (the heads) is always in; SETUP decides which one it
                            // is. A switch-only group is its own switch.
                            // A TOGGLE, not a button (runde 139, Tobias:
                            // "tilføj toggle i stedet for en on/off knap").
                            // A switch shows its state by where the knob is,
                            // so it needs no word in it and no second line
                            // under the name to explain it - which is half of
                            // why the card can now be 124 tall instead of 210.
                            // Top right, clear of the fader at the bottom.
                            Rectangle
                            {
                                id: groupSwitch
                                objectName: "groupSwitch:"+md.key
                                x: parent.width - width - 10
                                y: 11
                                width: 52
                                height: 26
                                radius: height / 2
                                visible: !md.base
                                color: castTile.off ? "#2E2E2E" : "#7ED07E"
                                border.width: 1
                                border.color: castTile.off ? "#4A4A4A" : "#9FE39F"
                                Behavior on color { ColorAnimation { duration: 120 } }

                                Rectangle
                                {
                                    id: groupKnob
                                    width: parent.height - 6
                                    height: width
                                    radius: width / 2
                                    y: 3
                                    x: castTile.off ? 3 : parent.width - width - 3
                                    color: castTile.off ? "#8A8A8A" : "#123012"
                                    Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
                                }

                                MouseArea
                                {
                                    // a touch target bigger than the switch it
                                    // draws: the pill is 52x26, the finger is not
                                    anchors.centerIn: parent
                                    width: parent.width + 16
                                    height: parent.height + 16
                                    onClicked: trackEngine.setGroupEnabled(md.key, castTile.off)
                                }
                            }
                        }
                    }
                }
            }
            // The role picker owns setup now: one tap per function decides
            // what it does, and the engine handles the rest.
            // Loaded indirectly so a fault in TrackSetup cannot take the whole
            // page down with it - and so the fault is shown instead of hidden.
            Loader
            {
                id: setupLoader
                parent: trackViewRoot        // overlay the whole page
                z: 100
                onLoaded: if (item) item.host = trackViewRoot
                anchors.fill: parent
                visible: trackViewRoot.setupOpen && trackManager
                         && trackManager.roleMode
                active: visible
                source: "qrc:/TrackSetup.qml"
            }

            Rectangle
            {
                anchors.fill: parent
                anchors.margins: 8
                visible: setupLoader.visible && setupLoader.status === Loader.Error
                color: "#3A1A1A"
                radius: 4

                Text
                {
                    anchors.fill: parent
                    anchors.margins: 12
                    wrapMode: Text.Wrap
                    color: "#FFB0B0"
                    font.pixelSize: 13
                    text:
                    {
                        // only when the Loader really failed: this used to
                        // compile TrackSetup on every page load
                        if (setupLoader.status !== Loader.Error)
                            return ""
                        var c = Qt.createComponent("qrc:/TrackSetup.qml")
                        return "TrackSetup.qml failed to load:\n\n"
                               + (c.status === Component.Error
                                  ? c.errorString() : "(no detail)")
                    }
                }
            }

            Flickable
            {
                visible: trackViewRoot.setupOpen && trackManager
                         && !trackManager.roleMode
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
                            Text
                            {
                                width: 190
                                text: modelData.toUpperCase()
                                color: trackViewRoot.markerColor(modelData)
                                font.bold: true
                                font.pixelSize: 14
                            }
                        }

                        Text
                        {
                            width: 150
                            text: qsTr("Folder")
                            color: trackViewRoot.cDim
                            font.pixelSize: 14
                        }
                        Text
                        {
                            width: 40
                            text: qsTr("spd")
                            color: trackViewRoot.cDim
                            font.pixelSize: 14
                        }
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
                                width: 130
                                height: 34
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
                                        width: 32
                                        height: 34
                                        checked: trackManager
                                                 ? trackManager.lookRandom(stateName,
                                                       parent.parent.slotIndex) : false
                                        onToggled: trackManager.setLookRandom(
                                                       stateName, parent.parent.slotIndex, checked)
                                    }

                                    ComboBox
                                    {
                                        width: 150
                                        height: 34
                                        enabled: !rndBox.checked
                                        model: trackManager
                                               ? trackManager.slotFunctions(parent.parent.slotIndex)
                                               : []
                                        textRole: "name"

                                        Component.onCompleted:
                                        {
                                            if (!trackManager) return
                                            var fid = trackManager.lookFunction(
                                                          parent.stateName, parent.parent.slotIndex)
                                            for (var i = 0; i < model.length; i++)
                                                if (model[i].id === fid) { currentIndex = i; return }
                                            currentIndex = -1
                                        }

                                        onActivated:
                                        {
                                            var e = model[currentIndex]
                                            if (e !== undefined)
                                                trackManager.setLookFunction(
                                                    parent.stateName, parent.parent.slotIndex, e.id)
                                        }
                                    }
                                }
                            }

                            ComboBox
                            {
                                width: 150
                                height: 34
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
                                width: 40
                                height: 34
                                checked: trackManager
                                         ? trackManager.slotFollowsSpeed(parent.slotIndex) : false
                                onToggled: trackManager.setSlotFollowsSpeed(parent.slotIndex,
                                                                            checked)
                            }
                        }
                    }

                    Row
                    {
                        spacing: 8

                        Text
                        {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("BPM range") + ":"
                            color: trackViewRoot.cDim
                            font.pixelSize: 14
                        }
                        SpinBox
                        {
                            height: 34
                            from: 40
                            to: 300
                            value: trackManager ? trackManager.bpmLow : 80
                            onValueModified: trackManager.bpmLow = value
                        }
                        SpinBox
                        {
                            height: 34
                            from: 40
                            to: 300
                            value: trackManager ? trackManager.bpmHigh : 140
                            onValueModified: trackManager.bpmHigh = value
                        }

                        Text
                        {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "    " + qsTr("Quantize") + ":"
                            color: trackViewRoot.cDim
                            font.pixelSize: 14
                        }
                        ComboBox
                        {
                            width: 90
                            height: 34
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
                        text: qsTr("Running") + ": "
                              + (trackManager ? trackManager.runningLook : "")
                        color: trackViewRoot.cDim
                        font.pixelSize: 14
                    }
                }
            }
        }
RowLayout {
            id: footerRow
            Layout.fillWidth: true; Layout.preferredHeight: 56; Layout.maximumHeight: 56
            spacing: 10

Rectangle
            {
                Layout.preferredWidth: 190
                Layout.fillHeight: true
                radius: 4
                objectName: "flash"
                ControlIcon { x: 6; anchors.verticalCenter: parent.verticalCenter; ink: "#101010"; kind: "flash" }
                color: (trackEngine && trackEngine.flashing) ? "#FFFFFF" : "#E36B6B"

                Text
                {
                    anchors.centerIn: parent
                    text: qsTr("FLASH WHITE")
                    color: "#101010"
                    font.bold: true
                    font.pixelSize: 18
                }

                MouseArea
                {
                    anchors.fill: parent
                    onPressed: trackEngine.setFlash(true)
                    onReleased: trackEngine.setFlash(false)
                    onCanceled: trackEngine.setFlash(false)
                }
            }
Rectangle
            {
                id: blackoutTile
                objectName: "blackout"
                ControlIcon { x: 6; anchors.verticalCenter: parent.verticalCenter;  kind: "blackout" }
                property bool armed: false          // this press is the one holding it

                // TrackTile's own look, value for value (radius 3, #3A3A3A,
                // border #555555, 13 px, bold and dark text when active), so
                // the row is unchanged to the eye and only the BEHAVIOUR is
                // different. It cannot BE a TrackTile: that one has a
                // TapHandler and no press/release of its own.
                Layout.preferredWidth: 150
                Layout.fillHeight: true
                radius: 3
                color: (trackEngine && trackEngine.blackout) ? "#B03030" : "#3A3A3A"
                border.width: 1
                border.color: (trackEngine && trackEngine.blackout)
                              ? Qt.lighter("#B03030", 1.3) : "#555555"

                Text
                {
                    anchors.centerIn: parent
                    text: qsTr("BLACKOUT")
                    color: (trackEngine && trackEngine.blackout) ? "#101010" : "#EEEEEE"
                    font.bold: trackEngine ? trackEngine.blackout : false
                    font.pixelSize: 13
                }

                MouseArea
                {
                    anchors.fill: parent
                    // do not let a Flickable under this steal the press: a
                    // stolen grab would fire onCanceled and leave the room
                    // dark with nothing holding it
                    preventStealing: true
                    onPressed:
                    {
                        if (!trackEngine)
                            return
                        if (trackEngine.blackout)
                        {
                            // latched on: this tap lets it go
                            trackEngine.blackout = false
                            blackoutTile.armed = false
                        }
                        else
                        {
                            trackEngine.blackout = true
                            blackoutTile.armed = true
                        }
                    }
                    onReleased: (mouse) =>
                    {
                        if (!trackEngine || !blackoutTile.armed)
                            return
                        blackoutTile.armed = false
                        // released ON the button: a momentary hold, let go.
                        // released off it: leave it latched.
                        if (mouse.x >= 0 && mouse.y >= 0
                            && mouse.x <= width && mouse.y <= height)
                            trackEngine.blackout = false
                    }
                    // the grab taken away from us counts as "finger left the
                    // button": latched, not released
                    onCanceled: blackoutTile.armed = false
                }
            }

// The sliders sit to the RIGHT of the two buttons now
// (runde 140, Tobias: "FLASH og BLACKOUT skal ogsaa rykkes
// paa den anden side af haze-sliderne"). The two things you
// hit in a hurry are together at the near edge, and the two
// you set once an evening are out of the way.
Item { Layout.fillWidth: true }

RowLayout
        {
            id: atmosRow
            Layout.fillWidth: true
            Layout.preferredWidth: footerRow.width * 0.53
            Layout.fillHeight: false
            Layout.preferredHeight: 56
            Layout.maximumHeight: 56
            spacing: 10
            visible: trackManager && trackManager.roleMode && trackEngine
                     && trackEngine.hazeAvailable && !trackViewRoot.setupOpen

            Repeater
            {
                model: [ "haze", "fan" ]

                Rectangle
                {
                    id: atmosSlider
                    objectName: modelData
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 4
                    color: "#1B1B1B"
                    border.width: 1
                    border.color: "#555555"

                    property bool isHaze: modelData === "haze"
                    property real level: trackEngine
                                         ? (isHaze ? trackEngine.haze : trackEngine.fan) : 0

                    Rectangle
                    {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.margins: 3
                        width: (parent.width - 6) * atmosSlider.level
                        radius: 3
                        color: atmosSlider.isHaze ? "#8A8A8A" : "#6A8AA0"
                    }

                    SliderTicks { anchors.fill: parent }
                    SliderGrip
                    {
                        x: Math.max(1, Math.min(parent.width - width - 1,
                                                3 + (parent.width - 6) * atmosSlider.level - width / 2))
                        height: parent.height
                        ink: atmosSlider.isHaze ? "#C8C8C8" : "#A8C4D8"
                        pressed: atmosArea.pressed
                    }

                    Text
                    {
                        anchors.centerIn: parent
                        text: (atmosSlider.isHaze ? qsTr("HAZE") : qsTr("FAN SPEED"))
                              + "  " + Math.round(atmosSlider.level * 100) + "%"
                        color: "#CCCCCC"
                        font.bold: true
                        font.pixelSize: 13
                    }

                    MouseArea
                    {
                        id: atmosArea
                        objectName: atmosSlider.objectName+"Drag"
                        anchors.fill: parent
                        function apply(x)
                        {
                            var v = Math.max(0, Math.min(1, (x - 3) / (width - 6)))
                            if (v < 0.03) v = 0
                            if (atmosSlider.isHaze) trackEngine.haze = v
                            else trackEngine.fan = v
                        }
                        onPressed: (mouse) => apply(mouse.x)
                        onPositionChanged: (mouse) => { if (pressed) apply(mouse.x) }
                    }
                }
            }
        }

        }

// The engine's own line, moved to the BOTTOM of the page (runde 141,
// Tobias: "(released) linjen skal staa nederst, saa bruger vi den til at
// faa lidt afstand til windows linjen"). It is the least urgent thing on
// the page and it now does a second job: twenty-eight pixels of air
// between FLASH and the Windows taskbar, so a thumb going for the flash
// cannot catch the clock instead. The height is unchanged.
Item
            {
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                Layout.maximumHeight: 28
                Layout.leftMargin: 12
                Layout.rightMargin: 12

                Column
                {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width
                spacing: 2

                Text
                {
                    width: parent.width
                    elide: Text.ElideRight
                    text: trackEngine ? trackEngine.report.split("  |  ")[0] : ""
                    color: "#CCCCCC"
                    font.pixelSize: 14
                    font.bold: true
                }
                Text
                {
                    width: parent.width
                    elide: Text.ElideRight
                    text:
                    {
                        if (!trackEngine || !trackManager) return ""
                        var parts = trackEngine.report.split("  |  ")
                        var colour = parts.length > 1 ? parts[1] : ""
                        var state = parts.length > 2 ? parts[2] : ""
                        // the next section, in bars
                        var cur = trackManager.currentBeat
                        var mk = trackManager.markers
                        var next = null
                        for (var i = 0; i < mk.length; i++)
                            if (mk[i].beat > cur && (next === null || mk[i].beat < next.beat)) next = mk[i]
                        var count = (next && trackManager.playing) ? "   \u2192 " + next.type.toUpperCase() + " " + Math.ceil((next.beat - cur) / 4) : ""
                        // the other deck, analysed ahead of time
                        var nxt = (trackManager.nextTitle !== undefined && trackManager.nextTitle !== "")
                                  ? "     " + qsTr("NEXT") + ": " + trackManager.nextTitle
                                    + (trackManager.nextFirstDrop > 0 ? " (" + qsTr("drop at bar") + " " + trackManager.nextFirstDrop + ")" : "")
                                  : ""
                        return colour + "   \u00b7   " + state + count + nxt
                    }
                    color: "#8A8A8A"
                    font.pixelSize: 12
                }
                }
            }
    }
}
