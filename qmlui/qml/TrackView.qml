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
    property int dragIndex: -1

    function markerColor(type)
    {
        if (type === "drop")
            return "#FF4444"
        if (type === "build")
            return "#FFAA22"
        if (type === "break")
            return "#4499FF"
        return "#DDDDDD"
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
        anchors.margins: UISettings.iconSizeDefault / 3
        spacing: UISettings.iconSizeDefault / 3

        // ---------------- header ----------------
        Rectangle
        {
            Layout.fillWidth: true
            height: UISettings.iconSizeMedium
            color: UISettings.bgMedium

            Row
            {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: UISettings.iconSizeDefault / 3
                spacing: UISettings.iconSizeDefault / 2

                RobotoText
                {
                    anchors.verticalCenter: parent.verticalCenter
                    label: trackManager && trackManager.title !== ""
                           ? trackManager.title : qsTr("No track")
                    fontSize: UISettings.textSizeDefault
                }
                RobotoText
                {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: trackViewRoot.beatCount > 0
                    label: trackManager
                           ? trackManager.bpm.toFixed(1) + " BPM  |  "
                             + trackViewRoot.beatCount + " " + qsTr("beats")
                             + "  |  " + qsTr("bar") + " "
                             + (Math.floor(trackViewRoot.currentBeat / 4) + 1)
                           : ""
                    fontSize: UISettings.textSizeDefault
                }
            }

            Row
            {
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                anchors.rightMargin: UISettings.iconSizeDefault / 3
                spacing: UISettings.iconSizeDefault / 3

                RobotoText
                {
                    anchors.verticalCenter: parent.verticalCenter
                    label: trackManager && trackManager.connected
                           ? qsTr("BLT connected") : qsTr("waiting for BLT")
                    color: trackManager && trackManager.connected ? "#22DD22" : "#AA6622"
                    fontSize: UISettings.textSizeDefault
                }
                RobotoText
                {
                    anchors.verticalCenter: parent.verticalCenter
                    label: qsTr("port") + " " + (trackManager ? trackManager.listenPort : 0)
                    fontSize: UISettings.textSizeDefault
                }
            }
        }

        // ---------------- waveform ----------------
        Rectangle
        {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: UISettings.bgMedium
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
                    ctx.fillStyle = "#1A1A1A"
                    ctx.fillRect(0, 0, w, h)

                    var n = trackViewRoot.beatCount
                    if (n <= 0)
                        return

                    var px = w / n
                    var wf = trackManager.waveform
                    var base = h - 14

                    // --- waveform: one bar per beat (bass energy 0-255) ---
                    ctx.fillStyle = "#2E6DA4"
                    for (var i = 0; i < n; i++)
                    {
                        var v = (i < wf.length ? wf[i] : 0) / 255.0
                        var bh = Math.max(1, v * base)
                        ctx.fillRect(i * px, base - bh, Math.max(1, px - 0.4), bh)
                    }

                    // --- beat / bar / phrase grid ---
                    for (var b = 0; b < n; b += 4)
                    {
                        var x = b * px
                        var isPhrase = (b % 32) === 0
                        ctx.strokeStyle = isPhrase ? "rgba(255,255,255,0.35)"
                                                   : "rgba(255,255,255,0.12)"
                        ctx.lineWidth = isPhrase ? 1.5 : 1
                        ctx.beginPath()
                        ctx.moveTo(x, 0)
                        ctx.lineTo(x, base)
                        ctx.stroke()
                    }

                    // --- markers ---
                    var mk = trackManager.markers
                    for (var m = 0; m < mk.length; m++)
                    {
                        var mb = mk[m].beat
                        var mx = (mb - 1) * px
                        var col = trackViewRoot.markerColor(mk[m].type)

                        ctx.strokeStyle = col
                        ctx.lineWidth = 2
                        ctx.beginPath()
                        ctx.moveTo(mx, 0)
                        ctx.lineTo(mx, base)
                        ctx.stroke()

                        // grab handle at the bottom
                        ctx.fillStyle = col
                        ctx.fillRect(mx - 6, base, 12, 14)
                    }

                    // --- playhead ---
                    if (trackViewRoot.currentBeat > 0)
                    {
                        var pxh = (trackViewRoot.currentBeat - 1) * px
                        ctx.strokeStyle = "#FFFFFF"
                        ctx.lineWidth = 2
                        ctx.beginPath()
                        ctx.moveTo(pxh, 0)
                        ctx.lineTo(pxh, base)
                        ctx.stroke()
                    }
                }
            }

            // Marker dragging. The beat is computed by rounding, so it always
            // lands exactly on a beat - snapping is inherent.
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

                    // only grab a marker within ~2% of the track width
                    var tol = Math.max(2, trackViewRoot.beatCount * 0.02)
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
    }
}
