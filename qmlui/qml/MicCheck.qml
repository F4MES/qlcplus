/*
  Q Light Controller Plus
  MicCheck.qml

  MIC CHECK (runde 467, Tobias 10-15: "Lav ... 1"): what the lys-PC's microphone
  hears in the room - the level, 16 bands from 40 Hz to 5 kHz, the bass / mids /
  highs next to what the track's own curves say at this beat, and QLC+'s beat
  detector next to the link's beat. RECORD writes 30 s of it to a CSV next to the
  tracklog, so the next round can see whether the mic can tell a bass kill from a
  filter sweep. It only listens: nothing on the rig changes, SHOW ON or OFF.
  The engine half is track_runtime/miccheck.inc.cpp (trackEngine.mic*). Loaded
  by TrackSetup.qml over the whole Track page, from SETUP > LAB.
  Flat (the r403 tones): no gradients, no glow, no shadows.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

import QtQuick

Rectangle
{
    id: micRoot
    objectName: "micCheck"
    color: "#0A0A0D"

    // nothing under the panel takes a finger
    MouseArea { anchors.fill: parent }

    readonly property var eng: trackEngine
    readonly property string device: eng ? eng.micDevice : ""
    readonly property int devIndex: eng ? eng.micDeviceIndex : 0
    readonly property int devCount: eng ? eng.micDeviceCount : 0
    readonly property string problem: eng ? eng.micProblem : ""
    readonly property real level: eng ? eng.micLevel : 0          // 0..1 = -60..0 dBFS
    readonly property real peak: eng ? eng.micPeak : 0
    readonly property bool clip: eng ? eng.micClip : false
    readonly property var bands: eng ? eng.micBands : []          // 16 x 0..1
    // the SHAPE, never the loudness ("hoejere er ikke noedvendigvis = vildere"):
    // bass, mids, highs each against what is usual (1 = usual, 2 = twice it)
    readonly property var room: eng ? eng.micRoom : [0, 0, 0]     // the room's share against its own usual
    readonly property var track: eng ? eng.micTrack : []          // the track's curves against their mean (empty: no track)
    readonly property bool silent: eng ? eng.micSilent : false
    readonly property int beatTick: eng ? eng.micBeatTick : 0     // +1 at each beat QLC+'s detector hears
    readonly property int linkTick: trackManager ? trackManager.currentBeat : 0
    readonly property int agree: eng ? eng.micAgree : -1          // % of link beats with a mic beat near them
    // the LINK and MIC lamps blink on their beat (120 ms), dark in between
    property bool linkLit: false
    property bool micLit: false
    onLinkTickChanged: { linkLit = true; linkOff.restart() }
    onBeatTickChanged: { micLit = true; micOff.restart() }
    Timer { id: linkOff; interval: 120; onTriggered: micRoot.linkLit = false }
    Timer { id: micOff; interval: 120; onTriggered: micRoot.micLit = false }
    readonly property int recLeft: eng ? eng.micRecLeft : 0       // seconds left of a recording, 0 = none
    readonly property bool auto: eng ? eng.micAuto : false
    readonly property var recs: eng ? eng.micRecs : []            // {at, section, title, secs}

    readonly property color cCard: "#141418"
    readonly property color cBtn: "#202027"
    readonly property color cText: "#E9E9EE"
    readonly property color cMute: "#8C8C96"
    readonly property color cDim: "#5E5E68"
    readonly property color cGold: "#E3B44F"
    readonly property color cBlue: "#4FA3E3"
    readonly property color cGreen: "#6FCB82"
    readonly property color cRed: "#E05050"
    readonly property real pad: 18

    function db(v) { return v <= 0.001 ? "-∞" : Math.round(-60 + 60 * v) + " dB" }
    function bandHz(i)
    {
        // the 16 bands QLC+'s capture splits 40 Hz - 5 kHz into (log spaced here for the labels)
        var f = 40 * Math.pow(5000 / 40, (i + 0.5) / 16)
        return f >= 1000 ? (Math.round(f / 100) / 10) + "k" : Math.round(f).toString()
    }
    function zoneOf(i) { var f = 40 * Math.pow(5000 / 40, (i + 0.5) / 16); return f < 250 ? 0 : (f < 2000 ? 1 : 2) }
    readonly property var zoneTone: ["#C0504F", "#4FA3E3", "#E3B44F"]

    component MicTile: Rectangle
    {
        id: tile
        property string text: ""
        property string sub: ""
        property bool on: false
        property color tone: micRoot.cBlue
        property real fontPx: 16
        signal tapped()
        radius: 8
        color: on ? tone : (tileArea.pressed ? "#2C2C33" : micRoot.cBtn)
        opacity: enabled ? 1.0 : 0.35
        Column
        {
            anchors.centerIn: parent
            width: parent.width - 16
            spacing: 4
            Text
            {
                width: parent.width; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight
                text: tile.text
                color: tile.on ? "#101014" : "#D6D6DC"
                font.bold: true; font.pixelSize: tile.fontPx; font.letterSpacing: tile.fontPx * 0.06
            }
            Text
            {
                visible: tile.sub !== ""
                width: parent.width; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight
                text: tile.sub; color: tile.on ? "#2A2A30" : micRoot.cMute; font.pixelSize: 13
            }
        }
        MouseArea { id: tileArea; anchors.fill: parent; onClicked: tile.tapped() }
    }
    component Label: Text
    {
        color: micRoot.cMute; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.6
    }

    // the mockup's 2048 x 1152 board, scaled to the page
    Item
    {
        id: board
        width: 2048; height: 1152
        readonly property real ks: Math.min(micRoot.width / 2048, micRoot.height / 1152)
        scale: ks
        transformOrigin: Item.TopLeft
        x: Math.round((micRoot.width - 2048 * ks) / 2)
        y: Math.round((micRoot.height - 1152 * ks) / 2)

        // ---------------- top bar ----------------
        Item
        {
            x: 20; y: 16; width: parent.width - 40; height: 64
            Rectangle
            {
                width: 210; height: 56; radius: 10; color: micRoot.cGold
                Text { anchors.centerIn: parent; text: "MIC CHECK"; color: "#1A1205"; font.bold: true; font.pixelSize: 22; font.letterSpacing: 2 }
            }
            MicTile
            {
                objectName: "micBackToLab"
                x: 226; width: 96; height: 40; anchors.verticalCenter: parent.verticalCenter
                text: "‹ " + qsTr("LAB"); fontPx: 15
                onTapped: if (eng) eng.micClose()
            }
            Column
            {
                x: 342; anchors.verticalCenter: parent.verticalCenter; spacing: 4
                Text { text: qsTr("what the room sounds like to this PC"); color: micRoot.cText; font.pixelSize: 26 }
                Text { text: qsTr("only listens  ·  the show keeps running  ·  nothing on the rig changes"); color: micRoot.cMute; font.pixelSize: 15 }
            }
            MicTile
            {
                objectName: "micClose"
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: 130; height: 56; text: qsTr("CLOSE"); onTapped: if (eng) eng.micClose()
            }
        }

        // ---------------- the room now ----------------
        Rectangle
        {
            id: nowCard
            x: 20; y: 96; width: 1320; height: 820; radius: 14; color: micRoot.cCard

            Label { x: micRoot.pad + 2; y: 16; text: qsTr("THE ROOM NOW") }

            // ---- no microphone: one line instead of empty meters
            Text
            {
                visible: micRoot.problem !== ""
                x: 40; y: 70; width: parent.width - 80; wrapMode: Text.WordWrap
                text: micRoot.problem; color: micRoot.cGold; font.pixelSize: 22
            }

            Item
            {
                anchors.fill: parent
                visible: micRoot.problem === ""

                // ---- LEVEL
                Label { x: 40; y: 58; text: qsTr("LEVEL") + "   " + micRoot.db(micRoot.level) + "   ·   " + qsTr("PEAK") + " " + micRoot.db(micRoot.peak) + "   ·   " + qsTr("only to set the mic up - nothing follows the loudness") }
                Rectangle
                {
                    id: meter
                    objectName: "micLevel"
                    x: 40; y: 84; width: 1040; height: 56; radius: 6; color: "#0E0E12"
                    // flat zones: green to -12, gold to -3, red above
                    Rectangle { width: meter.width * Math.min(micRoot.level, 0.8); height: parent.height; radius: 6; color: micRoot.cGreen }
                    Rectangle { x: meter.width * 0.8; width: meter.width * Math.max(0, Math.min(micRoot.level, 0.95) - 0.8); height: parent.height; color: micRoot.cGold }
                    Rectangle { x: meter.width * 0.95; width: meter.width * Math.max(0, micRoot.level - 0.95); height: parent.height; color: micRoot.cRed }
                    Rectangle { x: meter.width * micRoot.peak - 2; width: 4; height: parent.height; color: "#F0F0F4"; visible: micRoot.peak > 0.01 }
                    Repeater
                    {
                        model: [0.25, 0.5, 0.75, 0.8, 0.95]
                        Rectangle { x: meter.width * modelData; y: meter.height - 10; width: 2; height: 10; color: "#50505A" }
                    }
                }
                Rectangle
                {
                    objectName: "micClipLamp"
                    x: 1100; y: 84; width: 180; height: 56; radius: 6
                    color: micRoot.clip ? micRoot.cRed : micRoot.cBtn
                    Text { anchors.centerIn: parent; text: "CLIP"; color: micRoot.clip ? "#FFFFFF" : micRoot.cDim; font.bold: true; font.pixelSize: 18; font.letterSpacing: 2 }
                }
                Text
                {
                    x: 40; y: 150; width: 1240; elide: Text.ElideRight
                    text: micRoot.clip ? qsTr("Too loud for the mic - turn its level down in Windows (Sound > the microphone > Input volume)")
                                       : (micRoot.level < 0.25 ? qsTr("Very quiet - is the music on, and is this the right input?")
                                                               : qsTr("Good - the music is well inside the meter"))
                    color: micRoot.clip ? micRoot.cRed : micRoot.cMute; font.pixelSize: 16
                }

                // ---- the 16 bands
                Label { x: 40; y: 202; text: qsTr("16 BANDS  ·  40 Hz - 5 kHz") }
                Row
                {
                    id: zoneKey
                    anchors.right: parent.right; anchors.rightMargin: 40; y: 200; spacing: 22
                    Repeater
                    {
                        model: [qsTr("BASS"), qsTr("MIDS"), qsTr("HIGHS")]
                        Row
                        {
                            spacing: 8
                            Rectangle { width: 14; height: 14; radius: 3; color: micRoot.zoneTone[index]; anchors.verticalCenter: parent.verticalCenter }
                            Text { text: modelData; color: micRoot.cMute; font.pixelSize: 13; font.bold: true }
                        }
                    }
                }
                Item
                {
                    id: spec
                    objectName: "micBands"
                    x: 40; y: 228; width: 1240; height: 290
                    Rectangle { anchors.fill: parent; radius: 6; color: "#0E0E12" }
                    Row
                    {
                        x: 12; y: 12; height: spec.height - 44; spacing: 8
                        Repeater
                        {
                            model: 16
                            Item
                            {
                                width: (spec.width - 24 - 15 * 8) / 16; height: parent.height
                                readonly property real v: micRoot.bands.length > index ? micRoot.bands[index] : 0
                                Rectangle
                                {
                                    anchors.bottom: parent.bottom; width: parent.width; radius: 3
                                    height: Math.max(3, parent.height * v)
                                    color: micRoot.zoneTone[micRoot.zoneOf(index)]
                                }
                                Text
                                {
                                    anchors.top: parent.bottom; anchors.topMargin: 8; anchors.horizontalCenter: parent.horizontalCenter
                                    text: micRoot.bandHz(index); color: micRoot.cDim; font.pixelSize: 12
                                }
                            }
                        }
                    }
                }

                // ---- the room against the track: what the next round would steer on
                Label { x: 40; y: 548; text: qsTr("THE ROOM  vs  THE TRACK  ·  the shape of the sound, not its loudness  ·  the line = usual") }
                Column
                {
                    x: 40; y: 578; spacing: 12
                    Repeater
                    {
                        model: [qsTr("BASS"), qsTr("MIDS"), qsTr("HIGHS")]
                        Item
                        {
                            width: 1240; height: 40
                            readonly property real r: micRoot.room.length > index ? micRoot.room[index] : 0
                            readonly property bool has: micRoot.track.length > index
                            readonly property real t: has ? micRoot.track[index] : 0
                            Text { y: 9; text: modelData; color: micRoot.zoneTone[index]; font.bold: true; font.pixelSize: 16; font.letterSpacing: 1.4 }
                            Rectangle
                            {
                                x: 110; width: 860; height: 40; radius: 6; color: "#0E0E12"
                                // the track: a thin outline bar; the room: the filled bar - half-way is usual
                                Rectangle { width: parent.width * Math.min(1, parent.parent.r / 2); height: parent.height; radius: 6; color: micRoot.zoneTone[index]; opacity: 0.85 }
                                Rectangle
                                {
                                    visible: parent.parent.has
                                    width: parent.width * Math.min(1, parent.parent.t / 2); height: parent.height; radius: 6
                                    color: "transparent"; border.width: 2; border.color: "#F0F0F4"
                                }
                                Rectangle { x: parent.width / 2 - 1; y: -4; width: 2; height: parent.height + 8; color: "#8C8C96" }
                            }
                            Text
                            {
                                x: 990; y: 8; width: 250
                                text: micRoot.silent ? qsTr("the room is silent")
                                      : (!parent.has ? qsTr("no track playing")
                                      : (parent.t < 0.05 ? qsTr("the track is quiet here")
                                      : qsTr("room / track  %1").arg((parent.r / parent.t).toFixed(2))))
                                color: micRoot.cText; font.pixelSize: 17
                            }
                        }
                    }
                }
                Row
                {
                    x: 150; y: 732; spacing: 26
                    Row { spacing: 8; Rectangle { width: 26; height: 14; radius: 3; color: "#8C8C96"; anchors.verticalCenter: parent.verticalCenter } Text { text: qsTr("the room (mic)"); color: micRoot.cMute; font.pixelSize: 13 } }
                    Row { spacing: 8; Rectangle { width: 26; height: 14; radius: 3; color: "transparent"; border.width: 2; border.color: "#F0F0F4"; anchors.verticalCenter: parent.verticalCenter } Text { text: qsTr("the track (BLT)"); color: micRoot.cMute; font.pixelSize: 13 } }
                }

                // ---- beats: the link's and what QLC+'s detector hears
                Label { x: 40; y: 772; text: qsTr("BEAT") }
                Row
                {
                    x: 110; y: 764; spacing: 14
                    Rectangle
                    {
                        objectName: "micLinkBeat"
                        width: 150; height: 34; radius: 6
                        color: micRoot.linkLit ? micRoot.cBlue : micRoot.cBtn
                        Text { anchors.centerIn: parent; text: qsTr("LINK"); color: micRoot.linkLit ? "#101014" : micRoot.cMute; font.bold: true; font.pixelSize: 14; font.letterSpacing: 1.4 }
                    }
                    Rectangle
                    {
                        objectName: "micBeat"
                        width: 150; height: 34; radius: 6
                        color: micRoot.micLit ? micRoot.cGreen : micRoot.cBtn
                        Text { anchors.centerIn: parent; text: qsTr("MIC"); color: micRoot.micLit ? "#101014" : micRoot.cMute; font.bold: true; font.pixelSize: 14; font.letterSpacing: 1.4 }
                    }
                    Rectangle
                    {
                        objectName: "micSilent"
                        width: 150; height: 34; radius: 6
                        color: micRoot.silent ? micRoot.cGold : micRoot.cBtn
                        Text { anchors.centerIn: parent; text: qsTr("SILENT"); color: micRoot.silent ? "#101014" : micRoot.cMute; font.bold: true; font.pixelSize: 14; font.letterSpacing: 1.4 }
                    }
                    Text
                    {
                        anchors.verticalCenter: parent.verticalCenter
                        text: micRoot.agree < 0 ? qsTr("the mic's beat against the link's - needs a track playing")
                                                : qsTr("the mic hears %1 % of the link's beats").arg(micRoot.agree)
                        color: micRoot.cText; font.pixelSize: 17
                    }
                }
            }
        }

        // ---------------- input + recording ----------------
        Rectangle
        {
            id: recCard
            x: 1356; y: 96; width: 672; height: 820; radius: 14; color: micRoot.cCard

            Label { x: micRoot.pad + 2; y: 16; text: qsTr("INPUT") }
            MicTile
            {
                objectName: "micDevice"
                x: 20; y: 44; width: recCard.width - 40; height: 78
                text: micRoot.device !== "" ? micRoot.device : qsTr("NO MICROPHONE")
                sub: micRoot.devCount > 1 ? qsTr("%1 of %2  ·  tap = the next input").arg(micRoot.devIndex + 1).arg(micRoot.devCount)
                                          : qsTr("the only input on this PC")
                fontPx: 17
                enabled: micRoot.devCount > 1
                onTapped: if (eng) eng.micNextDevice()
            }

            Label { x: micRoot.pad + 2; y: 148; text: qsTr("RECORD") }
            MicTile
            {
                objectName: "micRecord"
                x: 20; y: 176; width: recCard.width - 40; height: 96
                on: micRoot.recLeft > 0; tone: micRoot.cRed
                text: micRoot.recLeft > 0 ? qsTr("RECORDING  ·  %1 s LEFT").arg(micRoot.recLeft) : "●  " + qsTr("RECORD 30 s")
                sub: micRoot.recLeft > 0 ? qsTr("tap = stop and keep what is there") : qsTr("the meters + the track's curves, 10 times a second")
                fontPx: 22
                enabled: micRoot.problem === ""
                onTapped: if (eng) eng.micRecord()
            }
            Item
            {
                x: 20; y: 290; width: recCard.width - 40; height: 78
                Column
                {
                    anchors.verticalCenter: parent.verticalCenter; width: parent.width - 190; spacing: 4
                    Text { text: qsTr("EVERY SECTION TONIGHT"); color: micRoot.cText; font.bold: true; font.pixelSize: 16; font.letterSpacing: 1.2 }
                    Text { width: parent.width; wrapMode: Text.WordWrap; text: qsTr("30 s the first time each section type plays in a track - drops, breaks, builds - up to 100 tonight, off again tomorrow"); color: micRoot.cMute; font.pixelSize: 13 }
                }
                Row
                {
                    anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; spacing: 8
                    MicTile { objectName: "micAutoOff"; width: 84; height: 56; text: qsTr("OFF"); on: !micRoot.auto; tone: "#B8B8C4"; onTapped: if (eng) eng.micSetAuto(false) }
                    MicTile { objectName: "micAutoOn"; width: 84; height: 56; text: qsTr("ON"); on: micRoot.auto; tone: micRoot.cGreen; onTapped: if (eng) eng.micSetAuto(true) }
                }
            }

            Label { x: micRoot.pad + 2; y: 392; text: qsTr("TONIGHT  ·  %1 RECORDINGS").arg(micRoot.recs.length) }
            Column
            {
                x: 20; y: 420; spacing: 6
                Repeater
                {
                    model: micRoot.recs.slice(-7).reverse()
                    Rectangle
                    {
                        width: recCard.width - 40; height: 44; radius: 6; color: "#1B1B21"
                        Text { x: 14; anchors.verticalCenter: parent.verticalCenter; text: modelData.at; color: micRoot.cText; font.bold: true; font.pixelSize: 15 }
                        Text { x: 82; anchors.verticalCenter: parent.verticalCenter; text: modelData.section.toUpperCase(); color: micRoot.cGold; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.2 }
                        Text { x: 172; width: parent.width - 240; anchors.verticalCenter: parent.verticalCenter; elide: Text.ElideRight; text: modelData.title; color: micRoot.cMute; font.pixelSize: 15 }
                        Text { anchors.right: parent.right; anchors.rightMargin: 14; anchors.verticalCenter: parent.verticalCenter; text: modelData.secs + " s"; color: micRoot.cDim; font.pixelSize: 14 }
                    }
                }
            }
            Text
            {
                x: 20; y: 750; width: recCard.width - 40; wrapMode: Text.WordWrap
                text: qsTr("Saved next to the tracklog (miccheck-<date>.csv) - SEND TO CLAUDE takes them along.")
                color: micRoot.cDim; font.pixelSize: 14
            }
        }

        // ---------------- good to know ----------------
        Rectangle
        {
            x: 20; y: 932; width: 2008; height: 200; radius: 14; color: micRoot.cCard
            Label { x: micRoot.pad + 2; y: 16; text: qsTr("BEFORE TRUSTING IT") }
            Column
            {
                x: 22; y: 46; spacing: 10
                Repeater
                {
                    model: [
                        qsTr("<b>Windows' audio enhancements off</b> - Settings > System > Sound > the microphone > Audio enhancements: Off. Otherwise Windows turns the room up and down by itself."),
                        qsTr("<b>Level</b> - only to set the mic up: the loudest drop in the gold, never on CLIP (lower the Input volume in the same place). Nothing will ever follow the loudness."),
                        qsTr("<b>Privacy</b> - Settings > Privacy & security > Microphone: \"Let desktop apps access your microphone\" on, or the meters stay flat."),
                        qsTr("<b>A laptop mic hears little below 100 Hz</b> - the kick's click and the bass's overtones are what it catches. A USB sound card on the mixer's booth or rec out is the clean way.")
                    ]
                    Text { width: 1970; text: modelData; textFormat: Text.StyledText; color: micRoot.cMute; font.pixelSize: 16; elide: Text.ElideRight }
                }
            }
        }
    }
}
