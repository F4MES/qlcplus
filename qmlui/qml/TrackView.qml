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

    // ---- marker dragging with zoom ----
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
        if (ms <= 0)
            return "--:--"
        var total = Math.floor(ms / 1000)
        var mins = Math.floor(total / 60)
        var secs = total % 60
        return mins + ":" + (secs < 10 ? "0" : "") + secs
    }

    // ---- the beat range currently drawn ----
    function viewCount()
    {
        if (beatCount <= 0)
            return 1
        return zoomActive ? Math.min(zoomSpan, beatCount) : beatCount
    }

    function viewFirst()
    {
        if (!zoomActive || beatCount <= 0)
            return 1
        var vc = viewCount()
        var f = Math.round(zoomCenter - vc / 2)
        if (f < 1)
            f = 1
        if (f > beatCount - vc + 1)
            f = beatCount - vc + 1
        return f
    }

    function nextMarker()
    {
        if (!trackManager)
            return null
        var mk = trackManager.markers
        var best = null
        for (var i = 0; i < mk.length; i++)
        {
            if (mk[i].beat > currentBeat && (best === null || mk[i].beat < best.beat))
                best = mk[i]
        }
        return best
    }

    Connections
    {
        target: trackManager
        function onTrackChanged() { wfCanvas.requestPaint() }
        function onMarkersChanged() { wfCanvas.requestPaint() }
        function onPositionChanged() { wfCanvas.requestPaint() }
    }

    // Auto-pan while a dragged marker is held near the edge of the zoomed view
    Timer
    {
        id: panTimer
        interval: 40
        repeat: true
        running: false
        property int dir: 0

        onTriggered:
        {
            if (trackViewRoot.dragIndex < 0)
            {
                running = false
                return
            }

            var vc = trackViewRoot.viewCount()
            var step = Math.max(1, Math.round(vc / 32))
            trackViewRoot.zoomCenter += dir * step

            if (trackViewRoot.zoomCenter < 1)
                trackViewRoot.zoomCenter = 1
            if (trackViewRoot.zoomCenter > trackViewRoot.beatCount)
                trackViewRoot.zoomCenter = trackViewRoot.beatCount

            // keep the marker under the finger as the view slides past
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
            Layout.preferredHeight: Math.max(UISettings.iconSizeMedium * 2.6,
                                             trackViewRoot.height * 0.17)
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
                    var w = width
                    var h = height

                    ctx.reset()
                    ctx.fillStyle = "#141414"
                    ctx.fillRect(0, 0, w, h)

                    var n = trackViewRoot.beatCount
                    if (n <= 0)
                        return

                    var vf = trackViewRoot.viewFirst()
                    var vc = trackViewRoot.viewCount()
                    var px = w / vc
                    var wf = trackManager.waveform

                    var lane = Math.round(h * 0.34)
                    var base = h - 4

                    function xOf(beat) { return (beat - vf) * px }

                    // ---- waveform bars ----
                    ctx.fillStyle = "#2E6DA4"
                    for (var i = 0; i < vc; i++)
                    {
                        var b = vf + i
                        if (b < 1 || b > n)
                            continue
                        var v = ((b - 1) < wf.length ? wf[b - 1] : 0) / 255.0
                        var bh = Math.max(1, v * (base - lane))
                        ctx.fillRect(i * px, base - bh, Math.max(1, px), bh)
                    }

                    // ---- grid: bars when zoomed, phrases when not ----
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

                    // ---- markers ----
                    var mk = trackManager.markers
                    for (var m = 0; m < mk.length; m++)
                    {
                        var mb = mk[m].beat
                        if (mb < vf - 2 || mb > vf + vc + 2)
                            continue

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

                    // ---- playhead ----
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

            // Grab a marker to zoom in around it; drag to the edge to pan.
            MouseArea
            {
                id: wfArea
                anchors.fill: parent
                enabled: trackViewRoot.beatCount > 0

                function beatAt(mx)
                {
                    var vc = trackViewRoot.viewCount()
                    var vf = trackViewRoot.viewFirst()
                    return Math.round(mx / (width / vc)) + vf
                }

                onPressed: function (mouse)
                {
                    var b = beatAt(mouse.x)
                    var mk = trackManager.markers
                    var best = -1
                    var bestDist = 1e9

                    for (var i = 0; i < mk.length; i++)
                    {
                        var d = Math.abs(mk[i].beat - b)
                        if (d < bestDist)
                        {
                            bestDist = d
                            best = i
                        }
                    }

                    // grab tolerance follows the zoom level, so it stays about
                    // the same number of pixels either way
                    var tol = Math.max(2, trackViewRoot.viewCount() * 0.02)

                    if (bestDist <= tol)
                    {
                        trackViewRoot.dragIndex = best
                        trackViewRoot.dragX = mouse.x
                        trackViewRoot.zoomCenter = mk[best].beat
                        trackViewRoot.zoomActive = true
                        wfCanvas.requestPaint()
                    }
                    else
                    {
                        trackViewRoot.dragIndex = -1
                    }
                }

                onPositionChanged: function (mouse)
                {
                    if (trackViewRoot.dragIndex < 0)
                        return

                    trackViewRoot.dragX = mouse.x
                    trackManager.moveMarker(trackViewRoot.dragIndex, beatAt(mouse.x))

                    // pan when the finger reaches either edge
                    var edge = width * 0.08
                    if (mouse.x < edge)
                        panTimer.dir = -1
                    else if (mouse.x > width - edge)
                        panTimer.dir = 1
                    else
                        panTimer.dir = 0

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

        // =============================================== now / next
        Rectangle
        {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: UISettings.bgMedium

            ColumnLayout
            {
                anchors.fill: parent
                anchors.margins: UISettings.iconSizeDefault / 2
                spacing: UISettings.iconSizeDefault / 3

                RobotoText
                {
                    Layout.fillWidth: true
                    height: UISettings.iconSizeMedium
                    label: trackManager && trackManager.title !== ""
                           ? trackManager.title : qsTr("No track loaded")
                    fontSize: UISettings.textSizeDefault * 1.5
                }

                RowLayout
                {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: UISettings.iconSizeDefault

                    Rectangle
                    {
                        Layout.preferredWidth: UISettings.bigItemHeight * 2.4
                        Layout.fillHeight: true
                        color: trackViewRoot.markerColor(trackViewRoot.liveState)
                        radius: 6
                        border.width: trackManager && trackManager.overrideState !== "" ? 4 : 0
                        border.color: "#FFFFFF"

                        Column
                        {
                            anchors.centerIn: parent
                            spacing: 4

                            RobotoText
                            {
                                anchors.horizontalCenter: parent.horizontalCenter
                                label: trackViewRoot.liveState.toUpperCase()
                                color: "#000000"
                                fontSize: UISettings.textSizeDefault * 2.2
                            }
                            RobotoText
                            {
                                anchors.horizontalCenter: parent.horizontalCenter
                                visible: trackManager && trackManager.overrideState !== ""
                                label: qsTr("FORCED")
                                color: "#000000"
                                fontSize: UISettings.textSizeDefault * 0.9
                            }
                        }
                    }

                    Column
                    {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: UISettings.iconSizeDefault / 4

                        RobotoText
                        {
                            label: qsTr("Running") + ": "
                                   + (trackManager
                                      ? trackManager.stateFunctionName(trackViewRoot.liveState)
                                      : "")
                            fontSize: UISettings.textSizeDefault * 1.2
                        }

                        RobotoText
                        {
                            property var nm: trackViewRoot.nextMarker()
                            label:
                            {
                                if (nm === null)
                                    return qsTr("No further points")
                                var d = nm.beat - trackViewRoot.currentBeat
                                return qsTr("Next") + ": " + nm.type.toUpperCase()
                                       + " " + qsTr("in") + " " + d + " " + qsTr("beats")
                                       + "  (" + Math.round(d / 4) + " " + qsTr("bars") + ")"
                            }
                            color: nm === null ? "#BBBBBB" : trackViewRoot.markerColor(nm.type)
                            fontSize: UISettings.textSizeDefault * 1.4
                        }

                        RobotoText
                        {
                            label: qsTr("Output") + ": "
                                   + (trackManager ? Math.round(trackManager.appliedEnergy * 100) : 0)
                                   + "%"
                            fontSize: UISettings.textSizeDefault * 1.1
                        }
                    }

                    Column
                    {
                        Layout.preferredWidth: UISettings.bigItemHeight * 3
                        Layout.fillHeight: true
                        spacing: UISettings.iconSizeDefault / 4

                        RobotoText
                        {
                            label: trackManager && trackManager.playing
                                   ? qsTr("PLAYING") : qsTr("PAUSED")
                            color: trackManager && trackManager.playing ? "#22DD22" : "#888888"
                            fontSize: UISettings.textSizeDefault * 1.3
                        }
                        RobotoText
                        {
                            label: trackViewRoot.fmtTime(trackManager ? trackManager.positionMs : 0)
                                   + " / "
                                   + trackViewRoot.fmtTime(trackManager ? trackManager.durationMs : 0)
                            fontSize: UISettings.textSizeDefault * 1.3
                        }
                        RobotoText
                        {
                            label: qsTr("beat") + " " + trackViewRoot.currentBeat
                                   + " / " + trackViewRoot.beatCount
                                   + "   " + qsTr("bar") + " "
                                   + (trackViewRoot.currentBeat > 0
                                      ? Math.floor((trackViewRoot.currentBeat - 1) / 4) + 1 : 0)
                            fontSize: UISettings.textSizeDefault
                        }
                        RobotoText
                        {
                            label: trackManager && trackManager.connected
                                   ? qsTr("BLT connected") : qsTr("waiting for BLT")
                            color: trackManager && trackManager.connected ? "#22DD22" : "#AA6622"
                            fontSize: UISettings.textSizeDefault
                        }
                    }
                }
            }
        }

        // =============================================== force a section
        Rectangle
        {
            Layout.fillWidth: true
            height: UISettings.iconSizeMedium * 1.2
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
                    label: qsTr("Force section") + ":"
                    fontSize: UISettings.textSizeDefault
                }

                Repeater
                {
                    model: [ "normal", "break", "build", "drop" ]

                    Button
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.toUpperCase()
                        checkable: true
                        checked: trackManager ? trackManager.overrideState === modelData : false
                        onClicked:
                        {
                            if (trackManager.overrideState === modelData)
                                trackManager.overrideState = ""
                            else
                                trackManager.overrideState = modelData
                        }
                    }
                }

                Button
                {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("AUTO")
                    enabled: trackManager ? trackManager.overrideState !== "" : false
                    onClicked: trackManager.overrideState = ""
                }
            }
        }

        // =============================================== state functions
        Rectangle
        {
            Layout.fillWidth: true
            height: UISettings.iconSizeMedium * 1.9
            color: UISettings.bgMedium

            property var functions: trackManager ? trackManager.functionList() : []

            Connections
            {
                target: trackManager
                function onFunctionsChanged() { assignRow.reload() }
            }

            Row
            {
                id: assignRow
                anchors.fill: parent
                anchors.margins: UISettings.iconSizeDefault / 4
                spacing: UISettings.iconSizeDefault / 3

                function reload()
                {
                    parent.functions = trackManager.functionList()
                    for (var i = 0; i < stateRepeater.count; i++)
                        stateRepeater.itemAt(i).syncSelection()
                }

                Repeater
                {
                    id: stateRepeater
                    model: [ "normal", "break", "build", "drop" ]

                    Column
                    {
                        property string stateName: modelData
                        spacing: 2

                        function syncSelection()
                        {
                            var fid = trackManager.stateFunction(stateName)
                            var list = assignRow.parent.functions
                            for (var i = 0; i < list.length; i++)
                            {
                                if (list[i].id === fid)
                                {
                                    funcCombo.currentIndex = i
                                    return
                                }
                            }
                            funcCombo.currentIndex = 0
                        }

                        RobotoText
                        {
                            height: UISettings.listItemHeight * 0.7
                            label: stateName.toUpperCase()
                            color: trackViewRoot.markerColor(stateName)
                            fontSize: UISettings.textSizeDefault * 0.9
                        }

                        ComboBox
                        {
                            id: funcCombo
                            width: UISettings.bigItemHeight * 1.7
                            model: assignRow.parent.functions
                            textRole: "name"

                            Component.onCompleted: parent.syncSelection()

                            onActivated:
                            {
                                var entry = assignRow.parent.functions[currentIndex]
                                if (entry !== undefined)
                                    trackManager.setStateFunction(parent.stateName, entry.id)
                            }
                        }

                        Row
                        {
                            spacing: 4

                            RobotoText
                            {
                                anchors.verticalCenter: parent.verticalCenter
                                height: UISettings.listItemHeight * 0.7
                                label: qsTr("level")
                                fontSize: UISettings.textSizeDefault * 0.8
                            }

                            SpinBox
                            {
                                width: UISettings.bigItemHeight
                                from: 0
                                to: 100
                                stepSize: 5
                                value: trackManager
                                       ? trackManager.stateIntensity(parent.parent.stateName) : 100
                                onValueModified:
                                    trackManager.setStateIntensity(parent.parent.stateName, value)
                            }
                        }
                    }
                }

                Column
                {
                    spacing: 2
                    RobotoText { height: UISettings.listItemHeight * 0.7; label: " " }
                    Button
                    {
                        text: qsTr("Random")
                        onClicked: trackManager.randomize()
                    }
                }

                Column
                {
                    spacing: 2
                    RobotoText
                    {
                        height: UISettings.listItemHeight * 0.7
                        label: qsTr("Auto")
                        fontSize: UISettings.textSizeDefault * 0.9
                    }
                    CheckBox
                    {
                        checked: trackManager ? trackManager.autoRun : false
                        onToggled: trackManager.autoRun = checked
                    }
                }

                Column
                {
                    spacing: 2
                    RobotoText
                    {
                        height: UISettings.listItemHeight * 0.7
                        label: qsTr("Quantize")
                        fontSize: UISettings.textSizeDefault * 0.9
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
                }
            }
        }

        // =============================================== energy
        Rectangle
        {
            Layout.fillWidth: true
            height: UISettings.iconSizeMedium * 1.2
            color: UISettings.bgMedium

            Row
            {
                anchors.fill: parent
                anchors.margins: UISettings.iconSizeDefault / 4
                spacing: UISettings.iconSizeDefault / 3

                RobotoText
                {
                    anchors.verticalCenter: parent.verticalCenter
                    label: qsTr("Energy") + ": "
                           + (trackManager ? Math.round(trackManager.energy * 100) : 0) + "%"
                           + "   (" + (trackManager ? trackManager.liveBpm : 0) + " BPM)"
                    fontSize: UISettings.textSizeDefault * 1.1
                }

                Slider
                {
                    anchors.verticalCenter: parent.verticalCenter
                    width: UISettings.bigItemHeight * 3
                    from: 0
                    to: 200
                    stepSize: 1
                    value: trackManager ? trackManager.energyTrim : 100
                    onMoved: trackManager.energyTrim = Math.round(value)
                }

                RobotoText
                {
                    anchors.verticalCenter: parent.verticalCenter
                    label: qsTr("trim") + " "
                           + (trackManager ? trackManager.energyTrim : 100) + "%"
                    fontSize: UISettings.textSizeDefault
                }

                RobotoText
                {
                    anchors.verticalCenter: parent.verticalCenter
                    label: "   " + qsTr("BPM range")
                    fontSize: UISettings.textSizeDefault
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
