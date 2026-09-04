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
    color: UISettings.bgMain

    property int beatCount: trackManager ? trackManager.beatCount : 0
    property int currentBeat: trackManager ? trackManager.currentBeat : 0
    property string liveState: trackManager ? trackManager.currentState : "normal"
    property var states: [ "normal", "break", "build", "drop" ]

    property var divValues: [ 0, 4000, 2000, 1000, 500, 250, 125 ]
    property var divLabels: [ "-", "4/1", "2/1", "1/1", "1/2", "1/4", "1/8" ]

    property int dragIndex: -1
    property real dragX: 0
    property bool zoomActive: false
    property int zoomCenter: 1
    property int zoomSpan: 64

    function markerColor(type)
    {
        if (type === "drop")  return "#FF4444"
        if (type === "build") return "#FFAA22"
        if (type === "break") return "#4499FF"
        return "#BBBBBB"
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
        anchors.margins: UISettings.iconSizeDefault / 4
        spacing: UISettings.iconSizeDefault / 4

        // =============================================== waveform strip
        Rectangle
        {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(UISettings.iconSizeMedium * 2.4,
                                             trackViewRoot.height * 0.15)
            color: "#141414"
            border.width: 1
            border.color: trackViewRoot.zoomActive ? "#FFAA22" : UISettings.bgLight

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
                    ctx.fillStyle = "#141414"
                    ctx.fillRect(0, 0, w, h)

                    var n = trackViewRoot.beatCount
                    if (n <= 0) return

                    var vf = trackViewRoot.viewFirst()
                    var vc = trackViewRoot.viewCount()
                    var px = w / vc
                    var wf = trackManager.waveform
                    var lane = Math.round(h * 0.34)
                    var base = h - 4

                    function xOf(beat) { return (beat - vf) * px }

                    ctx.fillStyle = "#2E6DA4"
                    for (var i = 0; i < vc; i++)
                    {
                        var b = vf + i
                        if (b < 1 || b > n) continue
                        var v = ((b - 1) < wf.length ? wf[b - 1] : 0) / 255.0
                        ctx.fillRect(i * px, base - Math.max(1, v * (base - lane)),
                                     Math.max(1, px), Math.max(1, v * (base - lane)))
                    }

                    var gridStep = trackViewRoot.zoomActive ? 4 : 32
                    ctx.strokeStyle = "rgba(255,255,255,0.14)"
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
                        ctx.moveTo(ph - 6, h)
                        ctx.lineTo(ph + 6, h)
                        ctx.lineTo(ph, h - 9)
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

                    if (bestDist <= Math.max(2, trackViewRoot.viewCount() * 0.02))
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

            RobotoText
            {
                anchors.centerIn: parent
                visible: trackViewRoot.beatCount === 0
                label: qsTr("Waiting for track data from Beat Link Trigger...")
                fontSize: UISettings.textSizeDefault
            }
        }

        // =============================================== status bar
        Rectangle
        {
            Layout.fillWidth: true
            height: UISettings.iconSizeMedium * 2.2
            color: UISettings.bgMedium

            RowLayout
            {
                anchors.fill: parent
                anchors.margins: UISettings.iconSizeDefault / 3
                spacing: UISettings.iconSizeDefault

                Rectangle
                {
                    Layout.preferredWidth: UISettings.bigItemHeight * 1.8
                    Layout.fillHeight: true
                    color: trackViewRoot.markerColor(trackViewRoot.liveState)
                    radius: 6
                    border.width: trackManager && trackManager.overrideState !== "" ? 4 : 0
                    border.color: "#FFFFFF"

                    RobotoText
                    {
                        anchors.centerIn: parent
                        label: trackViewRoot.liveState.toUpperCase()
                        color: "#000000"
                        fontSize: UISettings.textSizeDefault * 1.8
                    }
                }

                Column
                {
                    Layout.fillWidth: true
                    spacing: 2

                    RobotoText
                    {
                        label: trackManager && trackManager.title !== ""
                               ? trackManager.title : qsTr("No track loaded")
                        fontSize: UISettings.textSizeDefault * 1.3
                    }
                    RobotoText
                    {
                        property var nm: trackViewRoot.nextMarker()
                        label:
                        {
                            if (nm === null) return qsTr("No further points")
                            var d = nm.beat - trackViewRoot.currentBeat
                            return qsTr("Next") + ": " + nm.type.toUpperCase()
                                   + " " + qsTr("in") + " " + d + " " + qsTr("beats")
                                   + "  (" + Math.round(d / 4) + " " + qsTr("bars") + ")"
                        }
                        color: nm === null ? "#BBBBBB" : trackViewRoot.markerColor(nm.type)
                        fontSize: UISettings.textSizeDefault * 1.2
                    }
                    RobotoText
                    {
                        label: qsTr("Running") + ": "
                               + (trackManager ? trackManager.runningLook : "")
                        fontSize: UISettings.textSizeDefault * 0.95
                    }
                }

                Column
                {
                    Layout.preferredWidth: UISettings.bigItemHeight * 2.6
                    spacing: 2

                    RobotoText
                    {
                        label: trackManager && trackManager.playing
                               ? qsTr("PLAYING") : qsTr("PAUSED")
                        color: trackManager && trackManager.playing ? "#22DD22" : "#888888"
                        fontSize: UISettings.textSizeDefault * 1.2
                    }
                    RobotoText
                    {
                        label: trackViewRoot.fmtTime(trackManager ? trackManager.positionMs : 0)
                               + " / "
                               + trackViewRoot.fmtTime(trackManager ? trackManager.durationMs : 0)
                        fontSize: UISettings.textSizeDefault * 1.2
                    }
                    RobotoText
                    {
                        label: qsTr("beat") + " " + trackViewRoot.currentBeat
                               + " / " + trackViewRoot.beatCount
                        fontSize: UISettings.textSizeDefault * 0.95
                    }
                    RobotoText
                    {
                        label: trackManager && trackManager.connected
                               ? qsTr("BLT connected") : qsTr("waiting for BLT")
                        color: trackManager && trackManager.connected ? "#22DD22" : "#AA6622"
                        fontSize: UISettings.textSizeDefault * 0.95
                    }
                }
            }
        }

        // =============================================== look grid
        Rectangle
        {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: UISettings.bgMedium

            Column
            {
                anchors.fill: parent
                anchors.margins: UISettings.iconSizeDefault / 3
                spacing: 4

                // ---- header: state names + division per state ----
                Row
                {
                    spacing: UISettings.iconSizeDefault / 3

                    Item
                    {
                        width: UISettings.bigItemHeight * 1.5
                        height: UISettings.listItemHeight
                    }

                    Repeater
                    {
                        model: trackViewRoot.states

                        Column
                        {
                            width: UISettings.bigItemHeight * 2.2
                            spacing: 2

                            RobotoText
                            {
                                label: modelData.toUpperCase()
                                color: trackViewRoot.markerColor(modelData)
                                fontSize: UISettings.textSizeDefault * 1.1
                            }

                            Row
                            {
                                spacing: 4

                                RobotoText
                                {
                                    anchors.verticalCenter: parent.verticalCenter
                                    label: qsTr("speed")
                                    fontSize: UISettings.textSizeDefault * 0.8
                                }

                                ComboBox
                                {
                                    width: UISettings.bigItemHeight
                                    model: trackViewRoot.divLabels
                                    currentIndex:
                                    {
                                        if (!trackManager) return 0
                                        var d = trackManager.stateDivision(modelData)
                                        var i = trackViewRoot.divValues.indexOf(d)
                                        return i < 0 ? 0 : i
                                    }
                                    onActivated:
                                        trackManager.setStateDivision(
                                            modelData, trackViewRoot.divValues[currentIndex])
                                }
                            }
                        }
                    }

                    RobotoText
                    {
                        width: UISettings.bigItemHeight * 1.5
                        label: qsTr("Folder")
                        fontSize: UISettings.textSizeDefault
                    }

                    RobotoText
                    {
                        width: UISettings.bigItemHeight * 0.7
                        label: qsTr("spd")
                        fontSize: UISettings.textSizeDefault * 0.9
                    }
                }

                // ---- one row per slot ----
                Repeater
                {
                    model: trackManager ? trackManager.slotCount : 0

                    Row
                    {
                        property int slotIndex: index
                        spacing: UISettings.iconSizeDefault / 3

                        RobotoText
                        {
                            width: UISettings.bigItemHeight * 1.5
                            height: UISettings.listItemHeight
                            label: trackManager ? trackManager.slotName(slotIndex) : ""
                            fontSize: UISettings.textSizeDefault
                        }

                        Repeater
                        {
                            model: trackViewRoot.states

                            Row
                            {
                                property string stateName: modelData
                                width: UISettings.bigItemHeight * 2.2
                                spacing: 3

                                CheckBox
                                {
                                    id: rndBox
                                    width: UISettings.iconSizeMedium
                                    checked: trackManager
                                             ? trackManager.lookRandom(stateName,
                                                   parent.parent.slotIndex) : false
                                    onToggled: trackManager.setLookRandom(
                                                   stateName, parent.parent.slotIndex, checked)
                                }

                                ComboBox
                                {
                                    width: UISettings.bigItemHeight * 2.2
                                           - UISettings.iconSizeMedium - 6
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
                            width: UISettings.bigItemHeight * 1.5
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
                            width: UISettings.bigItemHeight * 0.7
                            checked: trackManager
                                     ? trackManager.slotFollowsSpeed(parent.slotIndex) : false
                            onToggled: trackManager.setSlotFollowsSpeed(parent.slotIndex, checked)
                        }
                    }
                }
            }
        }

        // =============================================== control bar
        Rectangle
        {
            Layout.fillWidth: true
            height: UISettings.iconSizeMedium * 1.3
            color: UISettings.bgMedium

            Row
            {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: UISettings.iconSizeDefault / 4
                spacing: UISettings.iconSizeDefault / 3

                RobotoText
                {
                    anchors.verticalCenter: parent.verticalCenter
                    label: qsTr("Force") + ":"
                    fontSize: UISettings.textSizeDefault
                }

                Repeater
                {
                    model: trackViewRoot.states
                    Button
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.toUpperCase()
                        checkable: true
                        checked: trackManager ? trackManager.overrideState === modelData : false
                        onClicked: trackManager.overrideState =
                                   (trackManager.overrideState === modelData) ? "" : modelData
                    }
                }

                Button
                {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("AUTO")
                    enabled: trackManager ? trackManager.overrideState !== "" : false
                    onClicked: trackManager.overrideState = ""
                }

                Button
                {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Re-roll")
                    onClicked: trackManager.reroll()
                }

                CheckBox
                {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Auto")
                    checked: trackManager ? trackManager.autoRun : false
                    onToggled: trackManager.autoRun = checked
                }

                RobotoText
                {
                    anchors.verticalCenter: parent.verticalCenter
                    label: "   " + qsTr("Level") + " "
                           + (trackManager
                              ? trackManager.stateIntensity(trackViewRoot.liveState) : 100)
                           + "%   " + qsTr("Output") + " "
                           + (trackManager ? Math.round(trackManager.appliedEnergy * 100) : 0) + "%"
                    fontSize: UISettings.textSizeDefault
                }

                Slider
                {
                    anchors.verticalCenter: parent.verticalCenter
                    width: UISettings.bigItemHeight * 2
                    from: 0
                    to: 100
                    stepSize: 5
                    value: trackManager
                           ? trackManager.stateIntensity(trackViewRoot.liveState) : 100
                    onMoved: trackManager.setStateIntensity(trackViewRoot.liveState,
                                                            Math.round(value))
                }

                RobotoText
                {
                    anchors.verticalCenter: parent.verticalCenter
                    label: "   " + qsTr("Energy") + " "
                           + (trackManager ? Math.round(trackManager.energy * 100) : 0) + "%  ("
                           + (trackManager ? trackManager.liveBpm : 0) + " BPM)  " + qsTr("trim")
                    fontSize: UISettings.textSizeDefault
                }

                Slider
                {
                    anchors.verticalCenter: parent.verticalCenter
                    width: UISettings.bigItemHeight * 2
                    from: 0
                    to: 200
                    stepSize: 1
                    value: trackManager ? trackManager.energyTrim : 100
                    onMoved: trackManager.energyTrim = Math.round(value)
                }

                SpinBox
                {
                    anchors.verticalCenter: parent.verticalCenter
                    from: 40
                    to: 300
                    value: trackManager ? trackManager.bpmLow : 80
                    onValueModified: trackManager.bpmLow = value
                }

                SpinBox
                {
                    anchors.verticalCenter: parent.verticalCenter
                    from: 40
                    to: 300
                    value: trackManager ? trackManager.bpmHigh : 140
                    onValueModified: trackManager.bpmHigh = value
                }
            }
        }
    }
}
