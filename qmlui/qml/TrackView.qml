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
    property string state0: trackManager ? trackManager.currentState : "normal"
    property int dragIndex: -1

    function markerColor(type)
    {
        if (type === "drop")
            return "#FF4444"
        if (type === "build")
            return "#FFAA22"
        if (type === "break")
            return "#4499FF"
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

    /** The next marker after the playhead, or null */
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
            border.color: UISettings.bgLight

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

                    var px = w / n
                    var wf = trackManager.waveform

                    var lane = Math.round(h * 0.34)   // top lane for marker flags
                    var base = h - 4                  // waveform sits on this line

                    // ---- waveform bars, one per beat ----
                    ctx.fillStyle = "#2E6DA4"
                    for (var i = 0; i < n; i++)
                    {
                        var v = (i < wf.length ? wf[i] : 0) / 255.0
                        var bh = Math.max(1, v * (base - lane))
                        ctx.fillRect(i * px, base - bh, Math.max(1, px), bh)
                    }

                    // ---- phrase grid every 32 beats ----
                    ctx.strokeStyle = "rgba(255,255,255,0.14)"
                    ctx.lineWidth = 1
                    for (var b = 0; b < n; b += 32)
                    {
                        var gx = b * px
                        ctx.beginPath()
                        ctx.moveTo(gx, lane)
                        ctx.lineTo(gx, base)
                        ctx.stroke()
                    }

                    // ---- markers: full-height line + labelled flag ----
                    var mk = trackManager.markers
                    for (var m = 0; m < mk.length; m++)
                    {
                        var mx = (mk[m].beat - 1) * px
                        var col = trackViewRoot.markerColor(mk[m].type)
                        var label = mk[m].type.toUpperCase()

                        ctx.strokeStyle = col
                        ctx.lineWidth = 2
                        ctx.beginPath()
                        ctx.moveTo(mx, 0)
                        ctx.lineTo(mx, base)
                        ctx.stroke()

                        // flag box, kept inside the canvas at both edges
                        ctx.font = "bold 11px sans-serif"
                        var tw = ctx.measureText(label).width + 10
                        var bx = Math.min(Math.max(mx, 0), w - tw)

                        ctx.fillStyle = col
                        ctx.fillRect(bx, 0, tw, lane - 3)
                        ctx.fillStyle = "#000000"
                        ctx.fillText(label, bx + 5, lane - 8)
                    }

                    // ---- playhead: bright pin with a head ----
                    if (trackViewRoot.currentBeat > 0)
                    {
                        var ph = (trackViewRoot.currentBeat - 1) * px

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

            // Dragging a marker. The beat is rounded, so it always lands on a
            // beat - snapping is inherent.
            MouseArea
            {
                anchors.fill: parent
                enabled: trackViewRoot.beatCount > 0

                function beatAt(mx)
                {
                    var px = width / trackViewRoot.beatCount
                    return Math.round(mx / px) + 1
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

                    var tol = Math.max(4, trackViewRoot.beatCount * 0.02)
                    trackViewRoot.dragIndex = (bestDist <= tol) ? best : -1
                }

                onPositionChanged: function (mouse)
                {
                    if (trackViewRoot.dragIndex >= 0)
                        trackManager.moveMarker(trackViewRoot.dragIndex, beatAt(mouse.x))
                }

                onReleased: trackViewRoot.dragIndex = -1
                onCanceled: trackViewRoot.dragIndex = -1
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

                // track title
                RobotoText
                {
                    Layout.fillWidth: true
                    height: UISettings.iconSizeMedium
                    label: trackManager && trackManager.title !== ""
                           ? trackManager.title : qsTr("No track loaded")
                    fontSize: UISettings.textSizeDefault * 1.5
                }

                // big state badge + next marker countdown
                RowLayout
                {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: UISettings.iconSizeDefault

                    Rectangle
                    {
                        Layout.preferredWidth: UISettings.bigItemHeight * 2.4
                        Layout.fillHeight: true
                        color: trackViewRoot.markerColor(trackViewRoot.state0)
                        radius: 6

                        RobotoText
                        {
                            anchors.centerIn: parent
                            label: trackViewRoot.state0.toUpperCase()
                            color: "#000000"
                            fontSize: UISettings.textSizeDefault * 2.2
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
                                      ? trackManager.stateFunctionName(trackViewRoot.state0)
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

        // =============================================== state functions
        Rectangle
        {
            Layout.fillWidth: true
            height: UISettings.iconSizeMedium * 1.5
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
