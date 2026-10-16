/*
  Q Light Controller Plus
  SyncCheck.qml

  SYNC (runde 470, Tobias 10-16: "lav den tap beat i settings ... saa vi faar det helt
  praecist"): is the light on time with the sound in the room? Two rounds of 16 taps -
  with the kick you HEAR, then with the blink you SEE (the strobes blink white on the
  link's beat). Both rounds carry the same habit of the finger (most people tap a little
  early), so the difference between them is the light against the sound. USE moves the
  light that many ms later (or earlier). The engine half is trackEngine.sync*.
  Flat (the r403 tones): no gradients, no glow, no shadows.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

import QtQuick

Rectangle
{
    id: syncRoot
    objectName: "syncCheck"
    color: "#0A0A0D"

    MouseArea { anchors.fill: parent }

    readonly property var eng: trackEngine
    readonly property int step: eng ? eng.syncStep : 0            // 0 sound, 1 light, 2 result
    readonly property int taps: eng ? eng.syncTaps : 0            // taps in this round (of 16)
    readonly property var soundMs: eng ? eng.syncSound : []       // each tap against its beat, ms (+ late)
    readonly property var lightMs: eng ? eng.syncLight : []
    readonly property int result: eng ? eng.syncResult : 0        // the sound after the light, ms
    readonly property int spread: eng ? eng.syncSpread : 0        // how even the taps were, +- ms
    readonly property int offset: eng ? eng.syncOffset : 0        // what the light is moved by now
    readonly property bool linked: trackManager ? trackManager.playing === true : false
    readonly property int beatTick: trackManager ? trackManager.currentBeat : 0
    property bool beatLit: false
    onBeatTickChanged: { beatLit = true; beatOff.restart() }
    Timer { id: beatOff; interval: 110; onTriggered: syncRoot.beatLit = false }

    readonly property color cCard: "#141418"
    readonly property color cBtn: "#202027"
    readonly property color cText: "#E9E9EE"
    readonly property color cMute: "#8C8C96"
    readonly property color cDim: "#5E5E68"
    readonly property color cGold: "#E3B44F"
    readonly property color cBlue: "#4FA3E3"
    readonly property color cGreen: "#6FCB82"

    function ms(v) { return (v > 0 ? "+" : "") + v + " ms" }

    component SyncTile: Rectangle
    {
        id: tile
        property string text: ""
        property string sub: ""
        property bool on: false
        property color tone: syncRoot.cBlue
        property real fontPx: 16
        signal tapped()
        radius: 8
        color: on ? tone : (tileArea.pressed ? "#2C2C33" : syncRoot.cBtn)
        opacity: enabled ? 1.0 : 0.35
        Column
        {
            anchors.centerIn: parent; width: parent.width - 16; spacing: 4
            Text { width: parent.width; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight; text: tile.text
                   color: tile.on ? "#101014" : "#D6D6DC"; font.bold: true; font.pixelSize: tile.fontPx; font.letterSpacing: tile.fontPx * 0.06 }
            Text { visible: tile.sub !== ""; width: parent.width; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap; text: tile.sub
                   color: tile.on ? "#2A2A30" : syncRoot.cMute; font.pixelSize: 14 }
        }
        MouseArea { id: tileArea; anchors.fill: parent; onClicked: tile.tapped() }
    }
    component Label: Text { color: syncRoot.cMute; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.6 }

    Item
    {
        id: board
        width: 2048; height: 1152
        readonly property real ks: Math.min(syncRoot.width / 2048, syncRoot.height / 1152)
        scale: ks; transformOrigin: Item.TopLeft
        x: Math.round((syncRoot.width - 2048 * ks) / 2)
        y: Math.round((syncRoot.height - 1152 * ks) / 2)

        // ---------------- top bar ----------------
        Item
        {
            x: 20; y: 16; width: parent.width - 40; height: 64
            Rectangle { width: 150; height: 56; radius: 10; color: syncRoot.cGold
                        Text { anchors.centerIn: parent; text: "SYNC"; color: "#1A1205"; font.bold: true; font.pixelSize: 22; font.letterSpacing: 2 } }
            Column
            {
                x: 170; anchors.verticalCenter: parent.verticalCenter; spacing: 4
                Text { text: qsTr("is the light on time with the sound?"); color: syncRoot.cText; font.pixelSize: 26 }
                Text { text: qsTr("two rounds of 16 taps  ·  music playing on the CDJs  ·  stand where the dance floor is"); color: syncRoot.cMute; font.pixelSize: 15 }
            }
            SyncTile { objectName: "syncClose"; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                       width: 130; height: 56; text: qsTr("CLOSE"); onTapped: if (eng) eng.syncClose() }
        }

        // ---------------- the two rounds ----------------
        Row
        {
            x: 20; y: 96; spacing: 16
            Repeater
            {
                model: [ { t: qsTr("1 · THE SOUND"), s: qsTr("tap with the KICK you hear - eyes closed helps"), k: 0 },
                         { t: qsTr("2 · THE LIGHT"), s: qsTr("the strobes blink white on the beat - tap with the BLINK"), k: 1 } ]
                Rectangle
                {
                    width: 996; height: 150; radius: 14
                    color: syncRoot.step === modelData.k ? "#1C1A12" : syncRoot.cCard
                    border.width: syncRoot.step === modelData.k ? 2 : 0; border.color: syncRoot.cGold
                    readonly property var list: modelData.k === 0 ? syncRoot.soundMs : syncRoot.lightMs
                    readonly property bool done: list.length >= 16
                    Text { x: 22; y: 18; text: modelData.t; color: parent.done ? syncRoot.cGreen : (syncRoot.step === modelData.k ? syncRoot.cGold : syncRoot.cMute)
                           font.bold: true; font.pixelSize: 20; font.letterSpacing: 2 }
                    Text { x: 22; y: 50; width: 940; text: modelData.s; color: syncRoot.cMute; font.pixelSize: 16 }
                    Row
                    {
                        x: 22; y: 92; spacing: 10
                        Repeater
                        {
                            model: 16
                            Rectangle { width: 46; height: 34; radius: 6
                                        color: index < parent.parent.list.length ? (parent.parent.done ? "#2E5A3A" : syncRoot.cGold) : syncRoot.cBtn
                                        Text { anchors.centerIn: parent; visible: index < parent.parent.parent.list.length
                                               text: index < parent.parent.parent.list.length ? parent.parent.parent.list[index] : ""
                                               color: parent.parent.parent.done ? "#BFE6C8" : "#1A1205"; font.pixelSize: 12; font.bold: true } }
                        }
                    }
                }
            }
        }

        // ---------------- the pad / the result ----------------
        Rectangle
        {
            x: 20; y: 262; width: 2008; height: 640; radius: 14; color: syncRoot.cCard

            // the pad: a round's taps
            Item
            {
                anchors.fill: parent
                visible: syncRoot.step < 2
                Rectangle
                {
                    objectName: "syncPad"
                    x: 40; y: 40; width: 1320; height: 560; radius: 14
                    color: padArea.pressed ? "#3A3220" : "#24221C"
                    Column
                    {
                        anchors.centerIn: parent; spacing: 14
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: syncRoot.step === 0 ? qsTr("TAP WITH THE KICK") : qsTr("TAP WITH THE BLINK")
                               color: syncRoot.cGold; font.bold: true; font.pixelSize: 54; font.letterSpacing: 4 }
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("%1 of 16").arg(syncRoot.taps); color: syncRoot.cText; font.pixelSize: 26 }
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: qsTr("a tap far off the beat is left out"); color: syncRoot.cMute; font.pixelSize: 16 }
                    }
                    MouseArea { id: padArea; anchors.fill: parent; onPressed: if (eng) eng.syncTap() }
                }
                Column
                {
                    x: 1400; y: 40; width: 568; spacing: 16
                    Label { text: qsTr("THE LINK'S BEAT") }
                    Rectangle { width: 568; height: 90; radius: 10; color: syncRoot.beatLit ? syncRoot.cBlue : syncRoot.cBtn
                                Text { anchors.centerIn: parent; text: syncRoot.linked ? qsTr("BEAT") : qsTr("NO MUSIC - play a track")
                                       color: syncRoot.beatLit ? "#06182A" : syncRoot.cMute; font.bold: true; font.pixelSize: 20; font.letterSpacing: 2 } }
                    Text { width: 568; wrapMode: Text.WordWrap; color: syncRoot.cMute; font.pixelSize: 16
                           text: qsTr("Taps are measured against this - the beat the CDJs say. Only the strobes blink in round 2, for its 16 beats.") }
                    SyncTile { width: 568; height: 64; text: qsTr("START AGAIN"); onTapped: if (eng) eng.syncRestart() }
                    Item { width: 1; height: 8 }
                    Label { text: qsTr("THE LIGHT IS MOVED") }
                    Text { objectName: "syncNow"; text: syncRoot.offset === 0 ? qsTr("0 ms - as it always ran") : syncRoot.ms(syncRoot.offset)
                           color: syncRoot.offset === 0 ? syncRoot.cText : syncRoot.cGold; font.bold: true; font.pixelSize: 30 }
                    Row
                    {
                        spacing: 12
                        SyncTile { objectName: "syncMinus"; width: 180; height: 64; text: qsTr("- 10 ms"); onTapped: if (eng) eng.syncNudge(-10) }
                        SyncTile { objectName: "syncPlus"; width: 180; height: 64; text: qsTr("+ 10 ms"); onTapped: if (eng) eng.syncNudge(10) }
                        SyncTile { objectName: "syncReset"; width: 184; height: 64; text: qsTr("RESET"); enabled: syncRoot.offset !== 0; onTapped: if (eng) eng.syncReset() }
                    }
                }
            }

            // the result
            Item
            {
                anchors.fill: parent
                visible: syncRoot.step === 2
                Label { x: 40; y: 34; text: qsTr("THE RESULT") }
                Text { x: 40; y: 70; color: syncRoot.cText; font.pixelSize: 40; font.bold: true
                       text: syncRoot.result === 0 ? qsTr("The light and the sound are together")
                             : (syncRoot.result > 0 ? qsTr("The sound comes %1 ms after the light").arg(syncRoot.result)
                                                     : qsTr("The sound comes %1 ms before the light").arg(-syncRoot.result)) }
                Text { x: 40; y: 128; color: syncRoot.spread <= 15 ? syncRoot.cGreen : syncRoot.cGold; font.pixelSize: 18
                       text: syncRoot.spread <= 15 ? qsTr("your taps were even (± %1 ms) - this is a good measurement").arg(syncRoot.spread)
                                                   : qsTr("your taps were uneven (± %1 ms) - START AGAIN for a better one").arg(syncRoot.spread) }
                // a beat, drawn: where the light is, where the sound is
                Item
                {
                    x: 40; y: 190; width: 1928; height: 200
                    Rectangle { y: 100; width: parent.width; height: 2; color: "#3A3A44" }
                    readonly property real pxPerMs: 8
                    readonly property real mid: width / 2
                    Repeater
                    {
                        model: [-100, -50, 0, 50, 100]
                        Item { x: parent.mid + modelData * parent.pxPerMs; y: 92
                               Rectangle { x: -1; width: 2; height: 16; color: "#3A3A44" }
                               Text { anchors.horizontalCenter: parent.horizontalCenter; y: 22; text: (modelData > 0 ? "+" : "") + modelData + " ms"; color: syncRoot.cDim; font.pixelSize: 13 } }
                    }
                    Item { x: parent.mid; y: 10
                           Rectangle { x: -4; width: 8; height: 90; radius: 3; color: "#F2F2F6" }
                           Text { anchors.horizontalCenter: parent.horizontalCenter; y: -2 - height; text: qsTr("LIGHT"); color: syncRoot.cText; font.bold: true; font.pixelSize: 15 } }
                    Item { x: parent.mid + syncRoot.result * parent.pxPerMs; y: 100
                           Rectangle { x: -4; width: 8; height: 90; radius: 3; color: syncRoot.cGold }
                           Text { anchors.horizontalCenter: parent.horizontalCenter; y: 96; text: qsTr("SOUND"); color: syncRoot.cGold; font.bold: true; font.pixelSize: 15 } }
                }
                Row
                {
                    x: 40; y: 470; spacing: 16
                    SyncTile { objectName: "syncUse"; width: 520; height: 110; on: true; tone: syncRoot.cGold; fontPx: 22
                               text: qsTr("MOVE THE LIGHT %1").arg(syncRoot.ms(syncRoot.result))
                               sub: qsTr("now %1").arg(syncRoot.ms(syncRoot.offset))
                               onTapped: if (eng) eng.syncUse() }
                    SyncTile { width: 400; height: 110; text: qsTr("KEEP %1").arg(syncRoot.ms(syncRoot.offset)); fontPx: 20; onTapped: if (eng) eng.syncClose() }
                    SyncTile { width: 400; height: 110; text: qsTr("START AGAIN"); fontPx: 20; onTapped: if (eng) eng.syncRestart() }
                }
            }
        }

        // ---------------- good to know ----------------
        Rectangle
        {
            x: 20; y: 918; width: 2008; height: 214; radius: 14; color: syncRoot.cCard
            Label { x: 20; y: 16; text: qsTr("GOOD TO KNOW") }
            Column
            {
                x: 22; y: 46; spacing: 10
                Repeater
                {
                    model: [
                        qsTr("<b>Why two rounds</b> - most fingers tap a little before the beat. Both rounds carry the same habit, so the difference between them is the light against the sound."),
                        qsTr("<b>Where you stand counts</b> - sound travels about 3 ms a metre. Stand where the guests dance, not in the booth."),
                        qsTr("<b>What it moves</b> - every beat-timed change of the light (hits, strobes, steps), by up to ± 150 ms. 0 ms is how it runs today."),
                        qsTr("<b>Live is fine</b> - SETUP > LAB > SYNC with the show on; the number is kept through a restart. - 10 / + 10 by hand, RESET = 0 ms.")
                    ]
                    Text { width: 1970; text: modelData; textFormat: Text.StyledText; color: syncRoot.cMute; font.pixelSize: 16; elide: Text.ElideRight }
                }
            }
        }
    }
}
