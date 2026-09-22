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

    // TRACK_TOUCH_LAYOUT_R137
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


    component TouchFader: Rectangle {
        id: fader
        property string caption: ""
        property real value: 0
        property color accent: "#4FA3E3"
        property bool prominent: false
        signal edited(real amount)
        color: trackViewRoot.cPanel; radius: 4
        border.color: trackViewRoot.cLine
        implicitHeight: prominent ? 126 : 82
        Text { x: 14; y: 10; text: fader.caption; color: trackViewRoot.cText; font.bold: true; font.pixelSize: fader.prominent ? 22 : 15 }
        Text { anchors.right: parent.right; anchors.rightMargin: 14; y: 5; text: Math.round(fader.value * 100) + "%"; color: fader.prominent ? fader.accent : trackViewRoot.cText; font.bold: true; font.pixelSize: fader.prominent ? 38 : 24 }
        Rectangle {
            x: 20; y: parent.height - 33; width: parent.width - 40; height: 8; radius: 4; color: "#555555"
            Rectangle { width: parent.width * Math.max(0, Math.min(1, fader.value)); height: 8; radius: 4; color: fader.accent }
            Rectangle { x: (parent.width * Math.max(0, Math.min(1, fader.value))) - width/2; y: -9; width: 26; height: 26; radius: 13; color: "#EEEEEE"; border.color: fader.accent; border.width: 2 }
        }
        MouseArea {
            objectName: fader.objectName + "Drag"
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: 56
            enabled: fader.enabled; preventStealing: true
            function apply(x) { fader.edited(Math.max(0, Math.min(1, (x-20)/Math.max(1,width-40)))) }
            onPressed: (mouse) => apply(mouse.x)
            onPositionChanged: (mouse) => { if (pressed) apply(mouse.x) }
        }
    }
    component TouchButton: Button {
        id: control
        property color selectedColor: "#4FA3E3"
        implicitHeight: 56; implicitWidth: 130
        contentItem: Text { text: control.text; color: control.checked ? "#101010" : trackViewRoot.cText; font.pixelSize: 15; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
        background: Rectangle { radius: 4; color: control.checked ? control.selectedColor : (control.down ? trackViewRoot.cBtnHi : trackViewRoot.cBtn); border.width: control.checked ? 2 : 1; border.color: control.checked ? Qt.lighter(control.selectedColor,1.3) : trackViewRoot.cLine; opacity: control.enabled ? 1 : 0.5 }
    }
    component FixtureIcon: Canvas {
        property string kind: "head"
        width: 28; height: 28
        onKindChanged: requestPaint()
        onPaint: {
            var c=getContext("2d"); c.reset(); c.strokeStyle="#CCCCCC"; c.lineWidth=2
            if (kind === "laser") { c.strokeRect(3,20,22,5); for(var i=0;i<4;i++){c.beginPath();c.moveTo(6+i*5,18);c.lineTo(3+i*7,3);c.stroke()} }
            else if(kind === "strobe") { c.strokeRect(3,3,22,22); for(var j=0;j<4;j++){c.beginPath();c.arc(9+(j%2)*10,9+Math.floor(j/2)*10,2,0,6.283);c.stroke()} }
            else { c.beginPath();c.arc(14,10,7,0,6.283);c.stroke();c.strokeRect(4,22,20,4);c.beginPath();c.moveTo(4,10);c.lineTo(4,18);c.lineTo(24,18);c.lineTo(24,10);c.stroke() }
        }
    }
    Rectangle {
        id: headerPanel
        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 8
        height: 76; color: trackViewRoot.cPanel; radius: 4
        RowLayout {
            anchors.fill: parent; anchors.margins: 10; spacing: 12
            ColumnLayout {
                Layout.fillWidth: true; spacing: 3
                Text { Layout.fillWidth: true; elide: Text.ElideRight; text: trackManager && trackManager.title !== "" ? trackManager.title : qsTr("Waiting for music"); font.pixelSize: 21; font.bold: true; color: trackViewRoot.cText }
                Text { Layout.fillWidth: true; elide: Text.ElideRight; text: (trackManager && trackManager.autoRun ? (trackEngine && trackEngine.fullAuto ? qsTr("FULL AUTO ACTIVE") : qsTr("SHOW ACTIVE")) : qsTr("SHOW STOPPED")) + "   ·   " + (trackManager && trackManager.connected ? (trackManager.linkStale ? qsTr("BLT STALE") : "BLT OK") : qsTr("NO BLT")) + "   ·   " + (trackManager ? trackManager.liveBpm : 0) + " BPM"; font.pixelSize: 14; color: trackManager && trackManager.autoRun ? "#7ED07E" : trackViewRoot.cDim }
            }
            TouchButton { objectName: "startScene"; implicitWidth: 158; text: "◉  " + qsTr("START SCENE"); checked: trackEngine ? trackEngine.startScene : false; selectedColor: "#E3B44F"; visible: trackManager && trackManager.roleMode; onClicked: if(trackEngine) trackEngine.startScene = !checked }
            TouchButton { objectName: "showSwitch"; implicitWidth: 160; text: trackManager && trackManager.autoRun ? "■  " + qsTr("STOP SHOW") : "▶  " + qsTr("START SHOW"); checked: trackManager ? trackManager.autoRun : false; selectedColor: "#7ED07E"; onClicked: if(trackManager) trackManager.autoRun = !checked }
            TouchButton { objectName: "setupSwitch"; implicitWidth: 110; text: "⚙  " + (trackViewRoot.setupOpen ? qsTr("CLOSE") : qsTr("SETUP")); onClicked: trackViewRoot.setupOpen = !trackViewRoot.setupOpen }
        }
    }
    Rectangle {
        id: actionPanel
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 8
        height: 64; color: trackViewRoot.cPanel; radius: 4
        visible: trackManager && trackEngine && trackManager.roleMode && !trackViewRoot.setupOpen
        RowLayout { anchors.fill: parent; anchors.margins: 4; spacing: 10
            // ---- calm: panic button. Base group only, one colour, no motion,
            //      for 16 bars - then back to automatic. Tap again to end it.
            TrackTile
            {
                Layout.preferredWidth: 120
                Layout.fillHeight: true
                objectName: "calm"
                label: (trackEngine && trackEngine.calmBarsLeft > 0)
                       ? "≈  " + qsTr("CALM") + " " + trackEngine.calmBarsLeft : "≈  " + qsTr("CALM")
                active: trackEngine ? trackEngine.calmBarsLeft > 0 : false
                activeColor: "#4FA3E3"
                onTapped: trackEngine.calm(trackEngine.calmBarsLeft > 0 ? 0 : 16)
            }

            // ---- hold: freeze the look - colour, cast, moves - until released
            TrackTile
            {
                Layout.preferredWidth: 120
                Layout.fillHeight: true
                objectName: "hold"
                label: "Ⅱ  " + qsTr("HOLD")
                active: trackEngine ? trackEngine.hold : false
                activeColor: "#E3B44F"
                onTapped: trackEngine.hold = !trackEngine.hold
            }

            // ---- next look: a new colour, cast and moves right now - the
            //      DJ's "something else, please"
            TrackTile
            {
                Layout.preferredWidth: 155
                Layout.fillHeight: true
                objectName: "nextLook"
                label: "↻  " + qsTr("NEXT LOOK")
                active: false
                onTapped: trackEngine.next()
            }

            Item { Layout.fillWidth: true }
            // ---- blackout: HOLD, and it latches if your finger leaves the
            //      button (Tobias, 2026-09-22 - "ligesom det fungerer i
            //      lightrider"). Press and hold: dark. Let go on the button:
            //      light. Slide off the button and let go: it stays dark
            //      until you tap it again. Which is the useful shape - you
            //      can hold a blackout through a breakdown without your
            //      thumb having to stay perfectly still, and you can park it.
            //
            //      The engine keeps following the track underneath the
            //      whole time, so the lights come back on the right look.
            Rectangle
            {
                id: blackoutTile
                objectName: "blackout"
                property bool armed: false          // this press is the one holding it

                // TrackTile's own look, value for value (radius 3, #3A3A3A,
                // border #555555, 13 px, bold and dark text when active), so
                // the row is unchanged to the eye and only the BEHAVIOUR is
                // different. It cannot BE a TrackTile: that one has a
                // TapHandler and no press/release of its own.
                Layout.preferredWidth: 165
                Layout.fillHeight: true
                radius: 3
                color: (trackEngine && trackEngine.blackout) ? "#B03030" : "#3A3A3A"
                border.width: 1
                border.color: (trackEngine && trackEngine.blackout)
                              ? Qt.lighter("#B03030", 1.3) : "#555555"

                Text
                {
                    anchors.centerIn: parent
                    text: "■  " + qsTr("BLACKOUT")
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
            // ---- flash: hold to strobe
            Rectangle
            {
                objectName: "flash"
                Layout.preferredWidth: 190
                Layout.fillHeight: true
                radius: 4
                color: (trackEngine && trackEngine.flashing) ? "#FFFFFF" : "#E36B6B"

                Text
                {
                    anchors.centerIn: parent
                    text: "ϟ  " + qsTr("FLASH WHITE") + "\n" + qsTr("Hold to flash")
                    horizontalAlignment: Text.AlignHCenter
                    color: "#101010"
                    font.bold: true
                    font.pixelSize: 16
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
    }
            RowLayout {
                id: sectionPanel
                anchors.top: headerPanel.bottom; anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 8
                height: 56; spacing: 8
                visible: !trackViewRoot.setupOpen
                Repeater { model: trackViewRoot.states
                    TouchButton { objectName: "section:"+modelData; Layout.fillWidth: true; text: modelData.toUpperCase(); checked: trackManager ? trackManager.overrideState === modelData : false; selectedColor: trackViewRoot.markerColor(modelData); onClicked: if(trackManager) trackManager.overrideState = checked ? "" : modelData }
                }
                TouchButton { objectName: "followMusic"; implicitWidth: 180; text: "↻  " + qsTr("FOLLOW MUSIC"); checked: trackManager ? trackManager.overrideState === "" : true; onClicked: if(trackManager) trackManager.overrideState = "" }
            }
    Flickable {
        id: bodyScroll
        objectName: "bodyScroll"
        anchors.top: sectionPanel.visible ? sectionPanel.bottom : headerPanel.bottom; anchors.bottom: actionPanel.visible ? actionPanel.top : parent.bottom
        anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 8
        clip: true; contentWidth: width; contentHeight: bodyLayout.height; boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }
        ColumnLayout {
            id: bodyLayout
            width: bodyScroll.width
            height: Math.max(bodyScroll.height, implicitHeight)
            spacing: 8
            RowLayout {
                Layout.fillWidth: true; Layout.preferredHeight: 26
                Text { Layout.fillWidth: true; elide: Text.ElideRight; property var nm: trackViewRoot.nextMarker(); text: trackManager && trackManager.beatCount > 0 ? trackViewRoot.liveState.toUpperCase() + (nm ? "   →   " + nm.type.toUpperCase() + " " + qsTr("IN") + " " + Math.ceil((nm.beat-trackViewRoot.currentBeat)/4) + " " + qsTr("BARS") : "") : qsTr("Track data appears here when music is loaded"); color: "#E3B44F"; font.pixelSize: 16; font.bold: true }
                Text { text: trackViewRoot.fmtTime(trackManager ? trackManager.positionMs : 0) + " / " + trackViewRoot.fmtTime(trackManager ? trackManager.durationMs : 0); color: trackViewRoot.cDim; font.pixelSize: 16 }
            }
        // =============================================== waveform strip
        Rectangle
        {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 330
            Layout.preferredHeight: Math.max(330, bodyScroll.height * 0.34)
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
                anchors.bottomMargin: 116
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
                    ctx.font = "bold 13px sans-serif"
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
                            var c = Qt.color(trackViewRoot.swatch(trackEngine.currentColour))
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
            Flickable
            {
                id: flagScroll
                contentWidth: flagTools.width
                clip: true
                flickableDirection: Flickable.HorizontalFlick
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 8
                width: parent.width - 16
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
                            width: 100
                            height: 48
                            label: "+ " + modelData.substring(0, 5).toUpperCase()
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
                        height: 48
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
                        height: 48
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
                        height: 48
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
                anchors.bottomMargin: 64
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
                            height: 48
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
                            height: 48
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
                        height: 48
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
                anchors.bottomMargin: 116
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
                    var lane = 60
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

                        ctx.font = "bold 13px sans-serif"
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
                preventStealing: pressIndex >= 0
                anchors.fill: parent
                anchors.margins: 1
                anchors.bottomMargin: 116          // the canvases are inset by one: pick where we draw
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


            RowLayout {
                id: dialsRow
                Layout.fillWidth: true; Layout.preferredHeight: 134; spacing: 10
                visible: trackManager && trackEngine && trackManager.roleMode
                TouchFader { objectName: "energy"; Layout.fillWidth: true; Layout.preferredWidth: bodyScroll.width * 0.64; Layout.fillHeight: true; caption: "ϟ  ENERGY"; prominent: true; accent: "#E3B44F"; value: trackManager ? trackManager.energyTrim/100 : 0; onEdited: (amount) => { if(trackEngine && trackEngine.roomAuto) trackEngine.roomAuto=false; if(trackManager) trackManager.energyTrim=Math.round(amount*100) } }
                ColumnLayout { Layout.fillWidth: true; Layout.preferredWidth: bodyScroll.width * 0.36; spacing: 4
                    TouchFader { objectName: "master"; Layout.fillWidth: true; Layout.preferredHeight: 78; caption: qsTr("MASTER"); value: trackEngine ? trackEngine.master : 1; onEdited: (amount) => { if(trackEngine) trackEngine.master=amount } }
                    RowLayout { Layout.fillWidth: true; spacing: 6
                        Text { text: "SPEED"; color: trackViewRoot.cDim; font.pixelSize: 13 }
                        Repeater { model: ["½×", "1×", "2×"]
                            TouchButton { objectName: "speed"+index; Layout.fillWidth: true; implicitWidth: 62; implicitHeight: 52; text: modelData; checked: trackEngine ? trackEngine.speed === index-1 : index===1; onClicked: if(trackEngine) trackEngine.speed=index-1 }
                        }
                    }
                }
            }
            Flickable {
                id: paletteScroll
                Layout.fillWidth: true; Layout.preferredHeight: 62
                contentWidth: paletteButtons.width; clip: true; flickableDirection: Flickable.HorizontalFlick
                visible: trackManager && trackEngine && trackManager.roleMode
                Row { id: paletteButtons; spacing: 8
                    TouchButton { objectName: "autoColour"; width: 165; height: 58; text: "↻  " + qsTr("AUTO COLOUR"); checked: trackEngine ? trackEngine.colourOverride === "" : true; selectedColor: "#7ED07E"; onClicked: if(trackEngine) trackEngine.colourOverride="" }
                    Repeater { model: trackEngine ? trackEngine.palette : []
                        TouchButton { objectName: "colour:"+modelData; width: 108; height: 58; text: "   " + modelData.toUpperCase(); checked: trackEngine ? trackEngine.colourOverride === modelData : false; selectedColor: modelData === "uv" ? "#7030C0" : modelData; onClicked: if(trackEngine) trackEngine.colourOverride=checked ? "" : modelData
                            Rectangle { x: 9; anchors.verticalCenter: parent.verticalCenter; width: 12; height: 24; radius: 3; color: modelData === "uv" ? "#7030C0" : modelData; border.color: "#CCCCCC" }
                        }
                    }
                }
            }
            Text { Layout.fillWidth: true; elide: Text.ElideRight; text: (trackEngine ? trackEngine.report : "") + (trackManager && trackManager.nextTitle !== "" ? "    NEXT: " + trackManager.nextTitle + (trackManager.nextFirstDrop > 0 ? " · DROP " + trackManager.nextFirstDrop : "") : ""); color: trackViewRoot.cDim; font.pixelSize: 13 }

            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: 210; color: trackViewRoot.cPanel; radius: 4
                visible: !trackViewRoot.setupOpen
                Flickable {
                    id: groupScroll
                    objectName: "groupScroll"
                    anchors.fill: parent; anchors.margins: 6; clip: true
                    contentWidth: castRow.width; flickableDirection: Flickable.HorizontalFlick
                    Row { id: castRow; spacing: 8; height: groupScroll.height
                        property int n: trackEngine ? Math.max(1,trackEngine.groups.length) : 1
                        Repeater { model: trackEngine ? trackEngine.groups : []
                            Rectangle {
                                id: castTile
                                property var md: modelData ? modelData : ({key:"",enabled:true,base:false,switchOnly:false})
                                property bool off: !md.enabled
                                property bool lit: trackEngine ? trackEngine.cast.indexOf(md.key)>=0 : false
                                property real trim: trackEngine && trackEngine.trims[md.key] !== undefined ? trackEngine.trims[md.key] : 1
                                width: Math.max(225,(groupScroll.width-(castRow.n-1)*8)/castRow.n); height: castRow.height
                                color: off ? "#1B1B1B" : "#303030"; radius: 5; border.width: md.base ? 2 : 1; border.color: md.base ? "#4FA3E3" : trackViewRoot.cLine
                                FixtureIcon { x: 12; y: 16; kind: md.lasers ? "laser" : md.strobes ? "strobe" : "head" }
                                Text { x: 48; y: 12; width: parent.width-60; height: 40; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; text: md.key; color: trackViewRoot.cText; font.pixelSize: 16; font.bold: true }
                                Text { x: 12; y: 68; width: parent.width-114; elide: Text.ElideRight; text: (castTile.off ? qsTr("DISABLED") : castTile.lit ? qsTr("ACTIVE NOW") : qsTr("READY")); color: castTile.off ? trackViewRoot.cDim : castTile.lit ? "#7ED07E" : "#BBBBBB"; font.pixelSize: 12 }
                                TouchButton { objectName: "groupSwitch:"+md.key; anchors.right: parent.right; anchors.rightMargin: 10; y: 52; width: 86; height: 52; implicitWidth: 86; text: md.base ? "BASE" : (castTile.off ? "○  OFF" : "●  ON"); enabled: !md.base; checked: !castTile.off; selectedColor: "#7ED07E"; onClicked: if(trackEngine) trackEngine.setGroupEnabled(md.key,castTile.off) }
                                TouchFader { objectName: "groupTrim:"+md.key; x: 8; anchors.bottom: parent.bottom; anchors.bottomMargin: 6; width: parent.width-16; height: 76; caption: qsTr("LEVEL"); visible: !md.switchOnly; enabled: !castTile.off; opacity: enabled ? 1 : 0.5; value: castTile.trim; onEdited: (amount) => { if(trackEngine) trackEngine.setGroupTrim(md.key,amount) } }
                                Text { visible: md.switchOnly; anchors.bottom: parent.bottom; anchors.bottomMargin: 28; x: 12; text: qsTr("ON / OFF CONTROL"); color: trackViewRoot.cDim; font.pixelSize: 13 }
                            }
                        }
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true; Layout.preferredHeight: 82
                visible: trackManager && trackManager.roleMode && trackEngine && trackEngine.hazeAvailable
                Repeater { model: ["haze","fan"]
                    TouchFader { objectName: modelData; Layout.fillWidth: true; Layout.fillHeight: true; caption: modelData === "haze" ? "≈  HAZE" : "↻  FAN SPEED"; value: trackEngine ? (modelData === "haze" ? trackEngine.haze : trackEngine.fan) : 0; accent: "#8A8A8A"; onEdited: (amount) => { if(trackEngine){ if(modelData === "haze") trackEngine.haze=amount; else trackEngine.fan=amount } } }
                }
            }
            Rectangle {
                Layout.fillWidth: true; Layout.preferredHeight: trackViewRoot.setupOpen && trackManager && !trackManager.roleMode ? 500 : 0
                visible: trackViewRoot.setupOpen
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
        }
    }
}
