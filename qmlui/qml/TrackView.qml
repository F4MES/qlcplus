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

    property bool setupOpen: false
    property real touchH: Math.max(UISettings.iconSizeMedium * 1.4, 50)

    property int dragIndex: -1
    property real dragX: 0
    property bool zoomActive: false
    property int zoomCenter: 1
    property int zoomSpan: 64

    function markerColor(type)
    {
        if (type === "drop")  return "#E23B3B"
        if (type === "build") return "#E0921A"
        if (type === "break") return "#2F7FD0"
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
        function onTrackChanged() { wfCanvas.requestPaint() }
        function onMarkersChanged() { wfCanvas.requestPaint() }
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
            trackManager.moveMarker(trackViewRoot.dragIndex,
                                    wfArea.beatAt(trackViewRoot.dragX))
            wfCanvas.requestPaint()
        }
    }

    ColumnLayout
    {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 6

        // =============================================== waveform strip
        Rectangle
        {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 110
            color: "#101010"
            border.width: 1
            border.color: trackViewRoot.zoomActive ? "#E0921A" : trackViewRoot.cLine

            // ---- what the analysis and the engine see, drawn over the
            //      waveform: bass as a warm floor, highs as a cool line, kicks
            //      as ticks, section bands with their energy, the played part
            //      of this section tinted in the running colour, and a countdown
            //      to the next section. (WF_OVERLAY_V1)
            Canvas
            {
                id: wfOverlay
                anchors.fill: parent
                anchors.margins: 1
                z: 1
                renderStrategy: Canvas.Threaded

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
                    var mk = trackManager.markers
                    var sorted = []
                    for (var i = 0; i < mk.length; i++) sorted.push(mk[i])
                    sorted.sort(function(a, b) { return a.beat - b.beat })
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
                        if (x1 - x0 > 60)
                        {
                            ctx.fillStyle = "rgba(255,255,255,0.75)"
                            ctx.fillText(m.type.toUpperCase() + (m.energy >= 0 ? "  " + Math.round(m.energy * 100) + "%" : ""), x0 + 4, 10)
                        }
                    }

                    // the played part of this section, tinted in the running colour
                    var cur = trackManager.currentBeat
                    if (cur > 0 && trackEngine && trackEngine.currentColour !== "")
                    {
                        var secStart = 1
                        var secEnd = total + 1
                        var next = null
                        for (var s = 0; s < sorted.length; s++)
                        {
                            if (sorted[s].beat <= cur) secStart = sorted[s].beat
                            else { secEnd = sorted[s].beat; next = sorted[s]; break }
                        }
                        var swatch = liveRow.swatch(trackEngine.currentColour)
                        var c = Qt.color(swatch)
                        ctx.fillStyle = Qt.rgba(c.r, c.g, c.b, 0.10)
                        ctx.fillRect(beatX(secStart, first, count), 7, beatX(cur, first, count) - beatX(secStart, first, count), h - 7)

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
                    function onTrackChanged() { wfOverlay.requestPaint() }
                    function onMarkersChanged() { wfOverlay.requestPaint() }
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

            Canvas
            {
                id: wfCanvas
                anchors.fill: parent
                anchors.margins: 1
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
                    var lane = Math.round(h * 0.32)
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

                    var mk = trackManager.markers
                    for (var m = 0; m < mk.length; m++)
                    {
                        var mb = mk[m].beat
                        if (mb < vf - 2 || mb > vf + vc + 2) continue

                        var mx = xOf(mb)
                        var col = trackViewRoot.markerColor(mk[m].type)
                        var label = mk[m].type.toUpperCase()
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

                        ctx.fillStyle = col
                        ctx.fillRect(bx, 0, tw, lane - 3)
                        ctx.fillStyle = "#000000"
                        ctx.fillText(label, bx + 5, lane - 8)

                        if (held)
                        {
                            ctx.fillStyle = col
                            ctx.font = "bold 10px sans-serif"
                            ctx.fillText("beat " + mb, bx + 5, lane + 12)
                        }
                    }

                    var cb = trackViewRoot.currentBeat
                    if (cb > 0 && cb >= vf && cb <= vf + vc)
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
                anchors.fill: parent
                enabled: trackViewRoot.beatCount > 0

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

                    if (bestDist <= Math.max(3, trackViewRoot.viewCount() * 0.025))
                    {
                        trackViewRoot.dragIndex = best
                        trackViewRoot.dragX = mouse.x
                        trackViewRoot.zoomCenter = mk[best].beat
                        trackViewRoot.zoomActive = true
                        wfCanvas.requestPaint()
                    }
                    else trackViewRoot.dragIndex = -1
                }

                onPositionChanged: function (mouse)
                {
                    if (trackViewRoot.dragIndex < 0) return
                    trackViewRoot.dragX = mouse.x
                    trackManager.moveMarker(trackViewRoot.dragIndex, beatAt(mouse.x))

                    var edge = width * 0.08
                    panTimer.dir = mouse.x < edge ? -1 : (mouse.x > width - edge ? 1 : 0)
                    panTimer.running = (panTimer.dir !== 0)
                    wfCanvas.requestPaint()
                }

                function release()
                {
                    panTimer.running = false
                    panTimer.dir = 0
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

        // =============================================== status
        Rectangle
        {
            Layout.fillWidth: true
            Layout.preferredHeight: 66
            color: trackViewRoot.cPanel
            radius: 4

            Row
            {
                anchors.left: parent.left
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
                            return qsTr("Next") + ": " + nm.type.toUpperCase()
                                   + " " + qsTr("in") + " " + d + " " + qsTr("beats")
                                   + "  (" + Math.round(d / 4) + " " + qsTr("bars") + ")"
                        }
                        color: nm === null ? trackViewRoot.cDim
                                           : trackViewRoot.markerColor(nm.type)
                        font.pixelSize: 15
                    }
                }
            }

            Row
            {
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
                    anchors.verticalCenter: parent.verticalCenter
                    width: 110
                    height: 44
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

        // =============================================== SECTION
        Rectangle
        {
            Layout.fillWidth: true
            Layout.preferredHeight: trackViewRoot.touchH + 12
            color: trackViewRoot.cPanel
            radius: 4

            RowLayout
            {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 6

                Text
                {
                    Layout.preferredWidth: 90
                    text: qsTr("SECTION")
                    color: trackViewRoot.cDim
                    font.bold: true
                    font.pixelSize: 14
                }

                Repeater
                {
                    model: trackViewRoot.states

                    Button
                    {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        checkable: true
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

                Button
                {
                    Layout.preferredWidth: 95
                    Layout.fillHeight: true
                    text: qsTr("FOLLOW")
                    enabled: trackManager ? trackManager.overrideState !== "" : false
                    onClicked: trackManager.overrideState = ""

                    contentItem: Text
                    {
                        text: parent.text
                        color: parent.enabled ? trackViewRoot.cText : "#666666"
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

                Button
                {
                    Layout.preferredWidth: 130
                    Layout.fillHeight: true
                    checkable: true
                    checked: trackManager ? trackManager.autoRun : false
                    text: checked ? qsTr("AUTO ON") : qsTr("AUTO OFF")
                    onClicked: trackManager.autoRun = checked

                    contentItem: Text
                    {
                        text: parent.text
                        color: parent.checked ? "#000000" : trackViewRoot.cText
                        font.bold: true
                        font.pixelSize: 16
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle
                    {
                        radius: 5
                        color: parent.checked ? "#3FBF3F" : trackViewRoot.cBtn
                        border.width: 1
                        border.color: trackViewRoot.cLine
                    }
                }
            }
        }

        // =============================================== live controls  (LIVE_V14_TOUCH)
        // What a DJ touches while playing. Two bars, one style: ENERGY (how
        // wild - the engine's appetite for effects, pulse and speed; creeps up
        // by the clock unless a hand takes over) and MASTER (how bright). Then
        // colour, and four buttons: CALM, HOLD, NEXT LOOK, FLASH WHITE.
        RowLayout
        {
            id: dialsRow
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: trackViewRoot.touchH * 1.25
            Layout.maximumHeight: trackViewRoot.touchH * 1.25
            spacing: 10
            visible: trackManager ? (trackManager.roleMode && !trackViewRoot.setupOpen) : false

            // ---- energy: 0..100 %, the one dial. No words on it: the DJ
            //      hears what it does
            Rectangle
            {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 4
                color: "#1B1B1B"
                border.width: 1
                border.color: "#555555"

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

                Text
                {
                    anchors.centerIn: parent
                    text: qsTr("ENERGY") + "  " + (trackManager ? trackManager.energyTrim : 50) + "%"
                    color: "#EEEEEE"
                    font.bold: true
                    font.pixelSize: 15
                }

                MouseArea
                {
                    anchors.fill: parent
                    function apply(x)
                    {
                        var v = Math.round(Math.max(0, Math.min(1, x / width)) * 100)
                        if (trackManager) trackManager.energyTrim = v
                    }
                    onPressed: (mouse) => apply(mouse.x)
                    onPositionChanged: (mouse) => { if (pressed) apply(mouse.x) }
                }
            }

            // ---- speed: half, as the music, double - for everything that moves
            Row
            {
                Layout.fillHeight: true
                spacing: 6

                Text
                {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("SPEED")
                    color: "#9A9A9A"
                    font.bold: true
                    font.pixelSize: 13
                }

                Repeater
                {
                    model: [ "\u00bd\u00d7", "1\u00d7", "2\u00d7" ]

                    TrackTile
                    {
                        width: trackViewRoot.touchH * 1.3
                        height: dialsRow.height
                        label: modelData
                        active: trackEngine ? trackEngine.speed === index - 1 : index === 1
                        activeColor: [ "#5A7A9A", "#4FA3E3", "#E3B44F" ][index]
                        onTapped: trackEngine.speed = index - 1
                    }
                }
            }

            // ---- master: how bright
            Rectangle
            {
                Layout.fillWidth: true
                Layout.fillHeight: true
                radius: 4
                color: "#1B1B1B"
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

                Text
                {
                    anchors.centerIn: parent
                    text: qsTr("MASTER") + "  " + Math.round((trackEngine ? trackEngine.master : 1) * 100) + "%"
                    color: "#EEEEEE"
                    font.bold: true
                    font.pixelSize: 15
                }

                MouseArea
                {
                    anchors.fill: parent
                    function apply(x) { if (trackEngine) trackEngine.master = Math.max(0, Math.min(1, x / width)) }
                    onPressed: (mouse) => apply(mouse.x)
                    onPositionChanged: (mouse) => { if (pressed) apply(mouse.x) }
                }
            }

        }

        // ---- colour and the four buttons
        RowLayout
        {
            id: liveRow
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: trackViewRoot.touchH * 1.3
            Layout.maximumHeight: trackViewRoot.touchH * 1.3
            spacing: 10
            visible: trackManager ? (trackManager.roleMode && !trackViewRoot.setupOpen) : false

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

            // ---- colour: AUTO or a locked palette colour
            Row
            {
                Layout.fillHeight: true
                spacing: 6

                TrackTile
                {
                    width: trackViewRoot.touchH * 1.5
                    height: liveRow.height
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
                        width: trackViewRoot.touchH * 1.5
                        height: liveRow.height
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

            Item { Layout.fillWidth: true }

            // ---- calm: panic button. Base group only, one colour, no motion,
            //      for 16 bars - then back to automatic. Tap again to end it.
            TrackTile
            {
                Layout.preferredWidth: trackViewRoot.touchH * 2
                Layout.fillHeight: true
                label: (trackEngine && trackEngine.calmBarsLeft > 0)
                       ? qsTr("CALM") + " " + trackEngine.calmBarsLeft : qsTr("CALM")
                active: trackEngine ? trackEngine.calmBarsLeft > 0 : false
                activeColor: "#4FA3E3"
                onTapped: trackEngine.calm(trackEngine.calmBarsLeft > 0 ? 0 : 16)
            }

            // ---- hold: freeze the look - colour, cast, moves - until released
            TrackTile
            {
                Layout.preferredWidth: trackViewRoot.touchH * 2
                Layout.fillHeight: true
                label: qsTr("HOLD")
                active: trackEngine ? trackEngine.hold : false
                activeColor: "#E3B44F"
                onTapped: trackEngine.hold = !trackEngine.hold
            }

            // ---- next look: a new colour, cast and moves right now - the
            //      DJ's "something else, please"
            TrackTile
            {
                Layout.preferredWidth: trackViewRoot.touchH * 2.4
                Layout.fillHeight: true
                label: qsTr("NEXT LOOK")
                active: false
                onTapped: trackEngine.next()
            }

            // ---- flash: hold to strobe
            Rectangle
            {
                Layout.preferredWidth: trackViewRoot.touchH * 2.6
                Layout.fillHeight: true
                radius: 4
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
        }

        // =============================================== warnings
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

        // =============================================== atmosphere
        RowLayout
        {
            id: atmosRow
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: trackViewRoot.touchH * 0.95
            Layout.maximumHeight: trackViewRoot.touchH * 0.95
            spacing: 10
            visible: trackManager && trackManager.roleMode && trackEngine
                     && trackEngine.hazeAvailable && !trackViewRoot.setupOpen

            Repeater
            {
                model: [ "haze", "fan" ]

                Rectangle
                {
                    id: atmosSlider
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
                        anchors.fill: parent
                        function apply(x)
                        {
                            var v = Math.max(0, Math.min(1, x / width))
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

        // =============================================== SETUP / filler
        Rectangle
        {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: trackViewRoot.touchH * 2.6
            Layout.minimumHeight: trackViewRoot.touchH * 2.6
            color: trackViewRoot.cPanel
            radius: 4

            // The cast, large: one fader per group - the DJ's trim on top of
            // everything the engine does - lit when the group is in the cast,
            // with a switch to leave it out for the night. Reads as a fader
            // without arrows: a scale on the sides, a bright edge on the level,
            // and the level line follows the finger. (CAST_V5_FADER_LOOK)
            Column
            {
                id: castPanel
                anchors.fill: parent
                anchors.margins: 6
                spacing: 4
                visible: !trackViewRoot.setupOpen

                Row
                {
                    width: parent.width
                    height: parent.height - statusLine.height - parent.spacing
                    spacing: 6

                    Repeater
                    {
                        model: trackEngine ? trackEngine.groups : []

                        Rectangle
                        {
                            id: castTile
                            property int n: trackEngine ? trackEngine.groups.length : 1
                            property bool lit: trackEngine ? trackEngine.cast.indexOf(modelData.key) >= 0 : false
                            property bool off: !modelData.enabled
                            property real trim: (trackEngine && trackEngine.trims[modelData.key] !== undefined)
                                                ? trackEngine.trims[modelData.key] : 1.0
                            width: (parent.width - (n - 1) * parent.spacing) / n
                            height: parent.height
                            radius: 6
                            color: off ? "#161616" : "#1E1E1E"
                            border.width: modelData.base ? 2 : 1
                            border.color: lit ? "#9FD3FF" : (modelData.base ? "#4FA3E3" : "#3A3A3A")
                            clip: true

                            // a fader scale along both edges: 25, 50, 75 %
                            Repeater
                            {
                                model: [ 0.25, 0.5, 0.75 ]
                                Item
                                {
                                    width: parent.width
                                    y: 3 + (parent.height - 6) * (1 - modelData) - 1
                                    height: 2
                                    Rectangle { x: 0; width: 10; height: 2; color: "#3A3A3A" }
                                    Rectangle { x: parent.width - 10; width: 10; height: 2; color: "#3A3A3A" }
                                }
                            }

                            // the fader: the trim fills from the bottom, with a
                            // bright edge where the level is
                            Rectangle
                            {
                                id: castFill
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.margins: 3
                                height: (parent.height - 6) * (castTile.off ? 0 : castTile.trim)
                                radius: 4
                                color: castTile.lit ? (modelData.base ? "#2E6FA8" : "#3D86C4")
                                                    : (castArea.pressed ? "#3A3A3A" : "#303030")
                                Behavior on color { ColorAnimation { duration: 150 } }

                                Rectangle
                                {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    height: castArea.pressed ? 4 : 3
                                    radius: 2
                                    visible: parent.height > 4
                                    color: castTile.lit ? "#BFE3FF" : (castArea.pressed ? "#DDDDDD" : "#8A8A8A")
                                }
                            }

                            // drag anywhere: the trim
                            MouseArea
                            {
                                id: castArea
                                anchors.fill: parent
                                enabled: !castTile.off
                                function apply(y)
                                {
                                    var v = 1 - (y - 3) / (height - 6)
                                    v = Math.max(0, Math.min(1, v))
                                    if (v > 0.97) v = 1
                                    if (trackEngine) trackEngine.setGroupTrim(modelData.key, v)
                                }
                                onPressed: (mouse) => apply(mouse.y)
                                onPositionChanged: (mouse) => { if (pressed) apply(mouse.y) }
                            }

                            Column
                            {
                                anchors.centerIn: parent
                                spacing: 4

                                Text
                                {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: modelData.key.toUpperCase()
                                    color: castTile.lit ? "#FFFFFF" : (castTile.off ? "#444444" : "#8A8A8A")
                                    font.bold: true
                                    font.pixelSize: 14
                                }
                                Text
                                {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: castTile.off ? qsTr("OFF")
                                        : (modelData.base ? qsTr("BASE") + "  " : "") + Math.round(castTile.trim * 100) + "%"
                                    color: castTile.lit ? "#E0F0FF" : "#6A6A6A"
                                    font.pixelSize: 12
                                }
                            }

                            // the switch: in or out of tonight's show. The base
                            // (the heads) is always in; SETUP decides which one it is.
                            Rectangle
                            {
                                anchors.top: parent.top
                                anchors.right: parent.right
                                anchors.margins: 5
                                width: 56
                                height: 30
                                radius: 15
                                visible: !modelData.base
                                color: castTile.off ? "#3A3A3A" : "#7ED07E"

                                Text
                                {
                                    anchors.centerIn: parent
                                    text: castTile.off ? qsTr("OFF") : qsTr("ON")
                                    color: castTile.off ? "#9A9A9A" : "#102010"
                                    font.bold: true
                                    font.pixelSize: 12
                                }

                                MouseArea
                                {
                                    anchors.fill: parent
                                    onClicked: trackEngine.setGroupEnabled(modelData.key, castTile.off)
                                }
                            }
                        }
                    }
                }

                Text
                {
                    id: statusLine
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    text: trackEngine ? trackEngine.report : ""
                    color: "#8A8A8A"
                    font.pixelSize: 13
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
    }
}
