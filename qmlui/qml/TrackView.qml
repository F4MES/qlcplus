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

    property bool setupOpen: false

    // touch sizing: everything a DJ hits mid-set is at least this tall
    property real touchH: Math.max(UISettings.iconSizeMedium * 1.5, 54)

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
            Layout.preferredHeight: Math.max(UISettings.iconSizeMedium * 2.2,
                                             trackViewRoot.height * 0.14)
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
                        var bh = Math.max(1, v * (base - lane))
                        ctx.fillRect(i * px, base - bh, Math.max(1, px), bh)
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

                        ctx.font = "bold 12px sans-serif"
                        var tw = ctx.measureText(label).width + 12
                        var bx = Math.min(Math.max(mx, 0), w - tw)

                        ctx.fillStyle = col
                        ctx.fillRect(bx, 0, tw, lane - 3)
                        ctx.fillStyle = "#000000"
                        ctx.fillText(label, bx + 6, lane - 8)

                        if (held)
                        {
                            ctx.fillStyle = col
                            ctx.font = "bold 11px sans-serif"
                            ctx.fillText("beat " + mb, bx + 6, lane + 13)
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
            height: UISettings.iconSizeMedium * 1.9
            color: UISettings.bgMedium

            RowLayout
            {
                anchors.fill: parent
                anchors.margins: UISettings.iconSizeDefault / 3
                spacing: UISettings.iconSizeDefault

                Rectangle
                {
                    Layout.preferredWidth: UISettings.bigItemHeight * 1.7
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
                        fontSize: UISettings.textSizeDefault * 1.7
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
                        fontSize: UISettings.textSizeDefault * 1.25
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
                        fontSize: UISettings.textSizeDefault * 1.15
                    }
                }

                Column
                {
                    Layout.preferredWidth: UISettings.bigItemHeight * 2.4
                    spacing: 2

                    RobotoText
                    {
                        label: trackViewRoot.fmtTime(trackManager ? trackManager.positionMs : 0)
                               + " / "
                               + trackViewRoot.fmtTime(trackManager ? trackManager.durationMs : 0)
                        fontSize: UISettings.textSizeDefault * 1.2
                    }
                    RobotoText
                    {
                        label: (trackManager && trackManager.playing
                                ? qsTr("PLAYING") : qsTr("PAUSED"))
                               + "   " + (trackManager ? trackManager.liveBpm : 0) + " BPM"
                        color: trackManager && trackManager.playing ? "#22DD22" : "#888888"
                        fontSize: UISettings.textSizeDefault
                    }
                    RobotoText
                    {
                        label: trackManager && trackManager.connected
                               ? qsTr("BLT connected") : qsTr("waiting for BLT")
                        color: trackManager && trackManager.connected ? "#22DD22" : "#AA6622"
                        fontSize: UISettings.textSizeDefault * 0.95
                    }
                }

                Button
                {
                    Layout.preferredWidth: UISettings.bigItemHeight
                    Layout.fillHeight: true
                    text: trackViewRoot.setupOpen ? qsTr("Close setup") : qsTr("Setup")
                    onClicked: trackViewRoot.setupOpen = !trackViewRoot.setupOpen
                }
            }
        }

        // =============================================== LIVE: force section
        Rectangle
        {
            Layout.fillWidth: true
            height: trackViewRoot.touchH + 12
            color: UISettings.bgMedium

            RowLayout
            {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 6

                RobotoText
                {
                    Layout.preferredWidth: UISettings.bigItemHeight * 0.9
                    label: qsTr("SECTION")
                    fontSize: UISettings.textSizeDefault
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
                            color: "#000000"
                            font.bold: true
                            font.pixelSize: UISettings.textSizeDefault * 1.3
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle
                        {
                            radius: 6
                            color: trackViewRoot.markerColor(modelData)
                            opacity: parent.checked ? 1.0
                                     : (trackViewRoot.liveState === modelData ? 0.75 : 0.4)
                            border.width: parent.checked ? 3 : 0
                            border.color: "#FFFFFF"
                        }
                    }
                }

                Button
                {
                    Layout.preferredWidth: UISettings.bigItemHeight * 0.9
                    Layout.fillHeight: true
                    text: qsTr("AUTO")
                    enabled: trackManager ? trackManager.overrideState !== "" : false
                    onClicked: trackManager.overrideState = ""
                }

                Button
                {
                    Layout.preferredWidth: UISettings.bigItemHeight * 0.9
                    Layout.fillHeight: true
                    text: qsTr("RE-ROLL")
                    onClicked: trackManager.reroll()
                }

                Button
                {
                    Layout.preferredWidth: UISettings.bigItemHeight * 0.9
                    Layout.fillHeight: true
                    checkable: true
                    checked: trackManager ? trackManager.autoRun : false
                    text: qsTr("RUN")
                    onClicked: trackManager.autoRun = checked

                    background: Rectangle
                    {
                        radius: 6
                        color: parent.checked ? "#22AA22" : UISettings.bgLight
                    }
                }
            }
        }

        // =============================================== LIVE: speed for this section
        Rectangle
        {
            Layout.fillWidth: true
            height: trackViewRoot.touchH + 12
            color: UISettings.bgMedium

            RowLayout
            {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 6

                RobotoText
                {
                    Layout.preferredWidth: UISettings.bigItemHeight * 0.9
                    label: qsTr("SPEED")
                    color: trackViewRoot.markerColor(trackViewRoot.liveState)
                    fontSize: UISettings.textSizeDefault
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
                            color: parent.checked ? "#000000" : "#DDDDDD"
                            font.bold: true
                            font.pixelSize: UISettings.textSizeDefault * 1.4
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle
                        {
                            radius: 6
                            color: parent.checked
                                   ? trackViewRoot.markerColor(trackViewRoot.liveState)
                                   : UISettings.bgLight
                        }
                    }
                }
            }
        }

        // =============================================== LIVE: level + energy
        Rectangle
        {
            Layout.fillWidth: true
            height: (trackViewRoot.touchH + 10) * 2
            color: UISettings.bgMedium

            Column
            {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 6

                // ---- level for the current section ----
                Row
                {
                    width: parent.width
                    height: trackViewRoot.touchH
                    spacing: 8

                    RobotoText
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        width: UISettings.bigItemHeight * 0.9
                        label: qsTr("LEVEL")
                        color: trackViewRoot.markerColor(trackViewRoot.liveState)
                        fontSize: UISettings.textSizeDefault
                    }

                    Slider
                    {
                        id: levelSlider
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - UISettings.bigItemHeight * 3.2
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
                            color: "#333333"

                            Rectangle
                            {
                                width: levelSlider.visualPosition * parent.width
                                height: parent.height
                                radius: 7
                                color: trackViewRoot.markerColor(trackViewRoot.liveState)
                            }
                        }

                        handle: Rectangle
                        {
                            x: levelSlider.leftPadding
                               + levelSlider.visualPosition * (levelSlider.availableWidth - width)
                            y: levelSlider.topPadding + levelSlider.availableHeight / 2 - height / 2
                            width: 40
                            height: 40
                            radius: 8
                            color: "#EEEEEE"
                            border.width: 2
                            border.color: "#666666"
                        }
                    }

                    RobotoText
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        width: UISettings.bigItemHeight * 2
                        label: (trackManager
                                ? trackManager.stateIntensity(trackViewRoot.liveState) : 100)
                               + "%   → " + (trackManager
                                ? Math.round(trackManager.appliedEnergy * 100) : 0) + "%"
                        fontSize: UISettings.textSizeDefault * 1.3
                    }
                }

                // ---- global energy trim ----
                Row
                {
                    width: parent.width
                    height: trackViewRoot.touchH
                    spacing: 8

                    RobotoText
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        width: UISettings.bigItemHeight * 0.9
                        label: qsTr("ENERGY")
                        fontSize: UISettings.textSizeDefault
                    }

                    Slider
                    {
                        id: energySlider
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - UISettings.bigItemHeight * 3.2
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
                            color: "#333333"

                            // the 100% mark, so you can find neutral by eye
                            Rectangle
                            {
                                x: parent.width * 0.5 - 1
                                width: 2
                                height: parent.height
                                color: "#888888"
                            }

                            Rectangle
                            {
                                width: energySlider.visualPosition * parent.width
                                height: parent.height
                                radius: 7
                                color: "#2E6DA4"
                            }
                        }

                        handle: Rectangle
                        {
                            x: energySlider.leftPadding
                               + energySlider.visualPosition * (energySlider.availableWidth - width)
                            y: energySlider.topPadding + energySlider.availableHeight / 2 - height / 2
                            width: 40
                            height: 40
                            radius: 8
                            color: "#EEEEEE"
                            border.width: 2
                            border.color: "#666666"
                        }
                    }

                    RobotoText
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        width: UISettings.bigItemHeight * 2
                        label: (trackManager ? trackManager.energyTrim : 100) + "%   "
                               + (trackManager ? Math.round(trackManager.energy * 100) : 0) + "%"
                        fontSize: UISettings.textSizeDefault * 1.3
                    }
                }
            }
        }

        // =============================================== SETUP (folded away)
        Rectangle
        {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: trackViewRoot.setupOpen
            color: UISettings.bgMedium

            Column
            {
                anchors.fill: parent
                anchors.margins: UISettings.iconSizeDefault / 3
                spacing: 4

                Row
                {
                    spacing: UISettings.iconSizeDefault / 3

                    Item
                    {
                        width: UISettings.bigItemHeight * 1.4
                        height: UISettings.listItemHeight
                    }

                    Repeater
                    {
                        model: trackViewRoot.states
                        RobotoText
                        {
                            width: UISettings.bigItemHeight * 2.2
                            label: modelData.toUpperCase()
                            color: trackViewRoot.markerColor(modelData)
                            fontSize: UISettings.textSizeDefault
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

                Repeater
                {
                    model: trackManager ? trackManager.slotCount : 0

                    Row
                    {
                        property int slotIndex: index
                        spacing: UISettings.iconSizeDefault / 3

                        RobotoText
                        {
                            width: UISettings.bigItemHeight * 1.4
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

                Row
                {
                    spacing: UISettings.iconSizeDefault / 3

                    RobotoText
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        label: qsTr("BPM range for energy") + ":"
                        fontSize: UISettings.textSizeDefault
                    }
                    SpinBox
                    {
                        from: 40
                        to: 300
                        value: trackManager ? trackManager.bpmLow : 80
                        onValueModified: trackManager.bpmLow = value
                    }
                    SpinBox
                    {
                        from: 40
                        to: 300
                        value: trackManager ? trackManager.bpmHigh : 140
                        onValueModified: trackManager.bpmHigh = value
                    }

                    RobotoText
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        label: "    " + qsTr("Quantize") + ":"
                        fontSize: UISettings.textSizeDefault
                    }
                    ComboBox
                    {
                        width: UISettings.bigItemHeight
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

                    RobotoText
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        label: "    " + qsTr("Running") + ": "
                               + (trackManager ? trackManager.runningLook : "")
                        fontSize: UISettings.textSizeDefault * 0.95
                    }
                }
            }
        }

        // filler so the live controls stay put when setup is closed
        Item
        {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !trackViewRoot.setupOpen
        }
    }
}
