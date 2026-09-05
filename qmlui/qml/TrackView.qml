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
            Layout.preferredHeight: Math.max(110, trackViewRoot.height * 0.15)
            color: "#101010"
            border.width: 1
            border.color: trackViewRoot.zoomActive ? "#E0921A" : trackViewRoot.cLine

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
                    Layout.preferredWidth: 105
                    Layout.fillHeight: true
                    text: qsTr("RE-ROLL")
                    onClicked: trackManager.reroll()

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

        // =============================================== SPEED
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
                    Layout.preferredWidth: 150
                    text: qsTr("SPEED") + " · " + trackViewRoot.liveState.toUpperCase()
                    color: trackViewRoot.markerColor(trackViewRoot.liveState)
                    font.bold: true
                    font.pixelSize: 14
                }

                Repeater
                {
                    model: trackViewRoot.divLabels.length

                    Button
                    {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        property int divVal: trackViewRoot.divValues[index]
                        checkable: true
                        checked: trackManager
                                 ? trackManager.stateDivision(trackViewRoot.liveState) === divVal
                                 : false
                        onClicked: trackManager.setStateDivision(trackViewRoot.liveState, divVal)

                        contentItem: Text
                        {
                            text: trackViewRoot.divLabels[index]
                            color: parent.checked ? "#000000" : trackViewRoot.cText
                            font.bold: true
                            font.pixelSize: 20
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle
                        {
                            radius: 5
                            color: parent.checked
                                   ? trackViewRoot.markerColor(trackViewRoot.liveState)
                                   : trackViewRoot.cBtn
                            border.width: 1
                            border.color: trackViewRoot.cLine
                        }
                    }
                }
            }
        }

        // =============================================== LEVEL + ENERGY
        Rectangle
        {
            Layout.fillWidth: true
            Layout.preferredHeight: trackViewRoot.touchH * 2 + 22
            color: trackViewRoot.cPanel
            radius: 4

            Column
            {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 8

                Row
                {
                    width: parent.width
                    height: trackViewRoot.touchH
                    spacing: 10

                    Text
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 150
                        text: qsTr("LEVEL") + " · " + trackViewRoot.liveState.toUpperCase()
                        color: trackViewRoot.markerColor(trackViewRoot.liveState)
                        font.bold: true
                        font.pixelSize: 14
                    }

                    Slider
                    {
                        id: levelSlider
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 150 - 200
                        height: trackViewRoot.touchH
                        from: 0
                        to: 100
                        stepSize: 1
                        value: trackManager
                               ? trackManager.stateIntensity(trackViewRoot.liveState) : 100
                        onMoved: trackManager.setStateIntensity(trackViewRoot.liveState,
                                                                Math.round(value))

                        background: Rectangle
                        {
                            x: levelSlider.leftPadding
                            y: levelSlider.topPadding + levelSlider.availableHeight / 2 - height / 2
                            width: levelSlider.availableWidth
                            height: 14
                            radius: 7
                            color: "#161616"
                            border.width: 1
                            border.color: trackViewRoot.cLine

                            Rectangle
                            {
                                width: levelSlider.visualPosition * (parent.width - 2) + 1
                                height: parent.height - 2
                                x: 1
                                y: 1
                                radius: 6
                                color: trackViewRoot.markerColor(trackViewRoot.liveState)
                            }
                        }

                        handle: Rectangle
                        {
                            x: levelSlider.leftPadding
                               + levelSlider.visualPosition * (levelSlider.availableWidth - width)
                            y: levelSlider.topPadding + levelSlider.availableHeight / 2 - height / 2
                            width: 38
                            height: 38
                            radius: 6
                            color: "#DDDDDD"
                            border.width: 2
                            border.color: "#111111"
                        }
                    }

                    Text
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 190
                        text: (trackManager
                               ? trackManager.stateIntensity(trackViewRoot.liveState) : 100)
                              + "%  " + qsTr("brightness")
                        color: trackViewRoot.cText
                        font.bold: true
                        font.pixelSize: 19
                    }
                }

                Row
                {
                    width: parent.width
                    height: trackViewRoot.touchH
                    spacing: 10

                    Text
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 150
                        text: qsTr("ENERGY")
                        color: trackViewRoot.cDim
                        font.bold: true
                        font.pixelSize: 14
                    }

                    Slider
                    {
                        id: energySlider
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 150 - 380
                        height: trackViewRoot.touchH
                        from: 0
                        to: 200
                        stepSize: 1
                        value: trackManager ? trackManager.energyTrim : 100
                        onMoved: trackManager.energyTrim = Math.round(value)

                        background: Rectangle
                        {
                            x: energySlider.leftPadding
                            y: energySlider.topPadding + energySlider.availableHeight / 2 - height / 2
                            width: energySlider.availableWidth
                            height: 14
                            radius: 7
                            color: "#161616"
                            border.width: 1
                            border.color: trackViewRoot.cLine

                            Rectangle
                            {
                                width: energySlider.visualPosition * (parent.width - 2) + 1
                                height: parent.height - 2
                                x: 1
                                y: 1
                                radius: 6
                                color: "#2E6DA4"
                            }

                            // the 100% mark, so neutral can be found by eye
                            Rectangle
                            {
                                x: parent.width * 0.5 - 1
                                width: 2
                                height: parent.height
                                color: "#AAAAAA"
                            }
                        }

                        handle: Rectangle
                        {
                            x: energySlider.leftPadding
                               + energySlider.visualPosition * (energySlider.availableWidth - width)
                            y: energySlider.topPadding + energySlider.availableHeight / 2 - height / 2
                            width: 38
                            height: 38
                            radius: 6
                            color: "#DDDDDD"
                            border.width: 2
                            border.color: "#111111"
                        }
                    }

                    Text
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 370
                        text:
                        {
                            // what this energy buys: effect groups on top of the base
                            var e = trackManager ? trackManager.energy : 0
                            var groove = e < 0.50 ? 0 : 1
                            var drop = e < 0.30 ? 0 : (e < 0.65 ? 1 : 2)
                            return Math.round(e * 100) + "%   ·   "
                                   + qsTr("groove") + " +" + groove + "   "
                                   + qsTr("drop") + " +" + drop + " " + qsTr("effects")
                        }
                        color: trackViewRoot.cText
                        font.bold: true
                        font.pixelSize: 19
                    }
                }
            }
        }

        // =============================================== live controls
        // What a DJ touches while playing: the colour, the master, and a
        // flash for the strobes. Everything else runs itself.
        RowLayout
        {
            id: liveRow
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: trackViewRoot.touchH * 1.4
            Layout.maximumHeight: trackViewRoot.touchH * 1.4
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

            // ---- master dimmer
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
                    font.pixelSize: 16
                }

                MouseArea
                {
                    anchors.fill: parent
                    function apply(x) { if (trackEngine) trackEngine.master = Math.max(0, Math.min(1, x / width)) }
                    onPressed: (mouse) => apply(mouse.x)
                    onPositionChanged: (mouse) => { if (pressed) apply(mouse.x) }
                }
            }

            // ---- calm: panic button. Base group only, one colour, no motion,
            //      for 16 bars - then back to automatic.
            Rectangle
            {
                Layout.preferredWidth: trackViewRoot.touchH * 2.2
                Layout.fillHeight: true
                radius: 4
                color: (trackEngine && trackEngine.calmBarsLeft > 0) ? "#4FA3E3" : "#2E4A63"
                border.width: 1
                border.color: "#6FB3F3"

                Text
                {
                    anchors.centerIn: parent
                    text: (trackEngine && trackEngine.calmBarsLeft > 0)
                          ? qsTr("CALM") + " " + trackEngine.calmBarsLeft
                          : qsTr("CALM 16")
                    color: "#EEEEEE"
                    font.bold: true
                    font.pixelSize: 16
                }

                // tap again to end it early
                MouseArea
                {
                    anchors.fill: parent
                    onClicked: trackEngine.calm(trackEngine.calmBarsLeft > 0 ? 0 : 16)
                }
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
            Layout.preferredHeight: trackViewRoot.touchH
            Layout.maximumHeight: trackViewRoot.touchH
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
                        color: "#EEEEEE"
                        font.bold: true
                        font.pixelSize: 15
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

        // =============================================== cast
        RowLayout
        {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 30
            Layout.maximumHeight: 30
            spacing: 6
            visible: trackManager ? (trackManager.roleMode && !trackViewRoot.setupOpen) : false

            Repeater
            {
                model: trackEngine ? trackEngine.groups : []

                Rectangle
                {
                    id: castTile
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 3
                    property bool lit: trackEngine ? trackEngine.cast.indexOf(modelData.key) >= 0 : false
                    color: lit ? "#4FA3E3" : "#242424"
                    border.width: 1
                    border.color: lit ? "#7FC3FF" : "#3A3A3A"

                    Text
                    {
                        anchors.centerIn: parent
                        text: modelData.key.toUpperCase()
                              + (modelData.enabled ? "" : "  (off)")
                        color: castTile.lit ? "#101010" : "#6A6A6A"
                        font.bold: castTile.lit
                        font.pixelSize: 12
                    }
                }
            }
        }

        // =============================================== SETUP / filler
        Rectangle
        {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: trackViewRoot.cPanel
            radius: 4

            Text
            {
                anchors.centerIn: parent
                visible: !trackViewRoot.setupOpen
                text: qsTr("Lights are running themselves. "
                           + "Press SETUP to change what each look does.")
                color: "#5A5A5A"
                font.pixelSize: 15
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
