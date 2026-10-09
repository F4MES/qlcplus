/*
  Q Light Controller Plus
  LaserLab.qml

  LASER LAB (runde 410): the animation laser's looks, one at a time, with SHOW
  OFF. ATLAS walks the laser's channels in steps, VARIANTS tries ways of moving
  each kept look, APPROVED lists what is kept. Every kept look becomes AUTO
  programmes of the show. The engine half is track_runtime/tracklab.inc.cpp
  (trackEngine.lab*). Loaded by TrackSetup.qml over the whole Track page.
  Flat (the r403 tones): no gradients, no glow, no shadows.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

import QtQuick

Rectangle
{
    id: labRoot
    objectName: "laserLab"
    color: "#0A0A0D"

    // nothing under the lab takes a finger
    MouseArea { anchors.fill: parent }

    readonly property var eng: trackEngine
    readonly property int pass: eng ? eng.labPass : 0
    readonly property bool kept: eng ? eng.labTagTitle !== "" : false
    readonly property color cCard: "#141418"
    readonly property color cBtn: "#202027"
    readonly property color cText: "#E9E9EE"
    readonly property color cMute: "#8C8C96"
    readonly property color cDim: "#5E5E68"
    readonly property color cGreen: "#6ECD82"
    readonly property color cRed: "#E0555A"
    readonly property color cGold: "#E3B44F"
    readonly property color cBlue: "#4FA3E3"
    readonly property color cCyan: "#3CC8D8"
    readonly property real pad: 18

    function swatch(c)
    {
        switch (c)
        {
        case "white":   return "#F2F2F5"
        case "red":     return labRoot.cRed
        case "blue":    return "#3E6FE8"
        case "cyan":    return labRoot.cCyan
        case "green":   return "#30C050"
        case "magenta": return "#D040C0"
        case "yellow":  return "#E0D030"
        case "orange":  return "#E08030"
        }
        return labRoot.cDim
    }
    function has(list, v) { return list ? list.indexOf(v) >= 0 : false }

    component LabTile: Rectangle
    {
        id: tile
        property string text: ""
        property bool on: false
        property color tone: labRoot.cGreen
        property real fontPx: 15
        property bool bar: false
        signal tapped()
        radius: 10
        color: on ? tone : (tileArea.pressed ? "#2C2C33" : labRoot.cBtn)
        opacity: enabled ? 1.0 : 0.35
        Text
        {
            anchors.centerIn: parent
            width: parent.width - 12
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: tile.text
            color: tile.on ? (0.299 * tile.tone.r + 0.587 * tile.tone.g + 0.114 * tile.tone.b > 0.5 ? "#101010" : "#FFFFFF") : "#D6D6DC"
            font.bold: true; font.pixelSize: tile.fontPx; font.letterSpacing: tile.fontPx * 0.08
        }
        Rectangle
        {
            visible: tile.bar && !tile.on
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: 14; anchors.rightMargin: 14; anchors.bottomMargin: 7
            height: 5; radius: 2.5; color: tile.tone
        }
        MouseArea { id: tileArea; anchors.fill: parent; onClicked: tile.tapped() }
    }
    component Label: Text
    {
        color: labRoot.cMute; font.bold: true; font.pixelSize: 12; font.letterSpacing: 1.8
    }
    component Card: Rectangle
    {
        property string title: ""
        color: labRoot.cCard; radius: 14
        Label { x: labRoot.pad + 2; y: labRoot.pad; width: parent.width - 2 * labRoot.pad; elide: Text.ElideRight; text: parent.title }
    }

    // the mockup's 2048 x 1152 board, scaled to the page
    Item
    {
        id: board
        width: 2048; height: 1152
        readonly property real ks: Math.min(labRoot.width / 2048, labRoot.height / 1152)
        scale: ks
        transformOrigin: Item.TopLeft
        x: Math.round((labRoot.width - 2048 * ks) / 2)
        y: Math.round((labRoot.height - 1152 * ks) / 2)

        // ---------------- top bar ----------------
        Item
        {
            x: 20; y: 16; width: parent.width - 40; height: 64
            Rectangle
            {
                width: 210; height: 56; radius: 10; color: labRoot.cGold
                Text { anchors.centerIn: parent; text: "LASER LAB"; color: "#1A1205"; font.bold: true; font.pixelSize: 22; font.letterSpacing: 2 }
            }
            Column
            {
                x: 230; anchors.verticalCenter: parent.verticalCenter; spacing: 4
                Text
                {
                    text: (eng ? eng.labGroup : "") + "  ·  " + (eng ? eng.labLampCount : 0) + ((eng && eng.labLampCount === 1) ? " lamp" : " lamps")
                    color: labRoot.cText; font.pixelSize: 26
                }
                Text { text: qsTr("SHOW OFF  ·  the rig is yours  ·  everything is saved as you go"); color: labRoot.cMute; font.pixelSize: 15 }
            }
            Row
            {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; spacing: 10
                Repeater
                {
                    model: [ qsTr("ATLAS"), qsTr("VARIANTS"), qsTr("APPROVED") + "  " + (eng ? eng.labApprovedCount : 0) ]
                    LabTile
                    {
                        objectName: "labPass:" + index
                        width: index === 2 ? 190 : 150; height: 56; text: modelData
                        on: labRoot.pass === index; tone: labRoot.cBlue; fontPx: 16
                        onTapped: if (eng) eng.labSetPass(index)
                    }
                }
                Item { width: 18; height: 1 }
                LabTile { objectName: "labClose"; width: 130; height: 56; text: qsTr("CLOSE"); fontPx: 16; onTapped: if (eng) eng.labClose() }
            }
        }

        // ---------------- left: what plays now ----------------
        Card
        {
            id: nowCard
            title: [qsTr("NOW PLAYING  ·  ATLAS"), qsTr("NOW PLAYING  ·  VARIANTS"), qsTr("NOW PLAYING  ·  APPROVED")][labRoot.pass]
            x: 20; y: 100; width: 1240; height: 640

            Text
            {
                objectName: "labTitle"
                x: 40; y: 60; width: 1160
                elide: Text.ElideRight
                text: eng ? (eng.labCount > 0 ? eng.labTitle : (labRoot.pass === 1 ? qsTr("Nothing to vary yet") : qsTr("Nothing kept yet"))) : ""
                color: labRoot.cText; font.bold: true
                font.pixelSize: text.length > 14 ? 52 : 84
            }
            Text
            {
                objectName: "labDetail"
                x: 44; y: 168; width: 1150
                elide: Text.ElideRight
                text: eng ? eng.labDetail : ""
                color: labRoot.cMute; font.pixelSize: 20
            }
            Row
            {
                x: 40; y: 212; spacing: 10
                Repeater
                {
                    model: eng ? eng.labLocked : []
                    Rectangle
                    {
                        height: 34; width: chipT.implicitWidth + 28; radius: 17; color: "#1C1C22"
                        Text { id: chipT; anchors.centerIn: parent; text: modelData; color: labRoot.cMute; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.2 }
                    }
                }
            }

            // the decision - three big tiles
            Row
            {
                x: 40; y: 300; spacing: 16
                enabled: eng ? eng.labCount > 0 : false
                LabTile
                {
                    objectName: "labNo"
                    width: 330; height: 150; text: "✕  " + qsTr("NO"); tone: labRoot.cRed; bar: true; fontPx: 34
                    on: eng ? eng.labVerdict === -1 : false
                    onTapped: if (eng) eng.labDecide(-1)
                }
                LabTile
                {
                    objectName: "labSame"
                    width: 330; height: 150; tone: labRoot.cDim; bar: true; fontPx: 28
                    text: labRoot.pass === 0 ? "=  " + qsTr("SAME AS LAST") : "½×  " + qsTr("TRY SLOWER")
                    on: eng ? (labRoot.pass === 0 ? eng.labVerdict === 2 : eng.labSlower) : false
                    onTapped: if (eng) { if (labRoot.pass === 0) eng.labDecide(2); else eng.labToggleSlower() }
                }
                LabTile
                {
                    objectName: "labGood"
                    width: 480; height: 150; text: "✓  " + qsTr("GOOD"); tone: labRoot.cGreen; bar: true; fontPx: 40
                    on: eng ? eng.labVerdict === 1 : false
                    onTapped: if (eng) eng.labDecide(1)
                }
            }
            Text
            {
                x: 44; y: 466; width: 1150
                wrapMode: Text.WordWrap
                text: labRoot.pass === 0
                      ? qsTr("SAME AS LAST joins this value to the pattern before it - the atlas learns where one pattern ends and the next begins.")
                      : (labRoot.pass === 1
                         ? qsTr("Every variant you keep becomes an AUTO programme of its own for the Track page. TRY SLOWER plays it again at half pace.")
                         : qsTr("What you keep plays on the Track page, in the room's colour. NO takes it out again."))
                color: labRoot.cDim; font.pixelSize: 16
            }
            Text
            {
                objectName: "labMessage"
                x: 44; y: 494; width: 1150
                elide: Text.ElideRight
                visible: text !== ""
                text: eng ? eng.labMessage : ""
                color: labRoot.cGold; font.bold: true; font.pixelSize: 16
            }

            // the strip: every candidate, where you are - tap to jump
            Label
            {
                x: 40; y: 522
                text: [qsTr("ATLAS"), qsTr("VARIANTS"), qsTr("APPROVED")][labRoot.pass] + "  ·  "
                      + (eng && eng.labCount > 0 ? (eng.labIndex + 1) : 0) + " " + qsTr("OF") + " " + (eng ? eng.labCount : 0)
                      + "  ·  ✓ " + (eng ? eng.labGood : 0) + "   ✕ " + (eng ? eng.labNo : 0)
                      + (labRoot.pass === 0 ? "   = " + (eng ? eng.labSame : 0) : "")
            }
            Item
            {
                id: strip
                objectName: "labStrip"
                x: 40; y: 550; width: 1160; height: 46
                readonly property var marks: eng ? eng.labStrip : []
                readonly property int n: marks.length
                readonly property real gap: n > 0 && width / n >= 6 ? 3 : (n > 0 && width / n >= 3 ? 1 : 0)
                readonly property real w: n > 0 ? Math.min(9, width / n - gap) : 9
                Row
                {
                    spacing: strip.gap
                    Repeater
                    {
                        model: strip.n
                        Rectangle
                        {
                            width: Math.max(1, strip.w); height: 46; radius: strip.w >= 4 ? 2 : 0
                            readonly property int v: strip.marks[index]
                            color: eng && index === eng.labIndex ? "#FFFFFF"
                                 : v === 1 ? labRoot.cGreen
                                 : v === -1 ? labRoot.cRed
                                 : v === 2 ? labRoot.cDim : "#26262E"
                        }
                    }
                }
                MouseArea
                {
                    anchors.fill: parent
                    onClicked: (mouse) => {
                        if (!eng || strip.n === 0) return
                        eng.labGoto(Math.floor(mouse.x / Math.max(1, strip.w + strip.gap)))
                    }
                }
            }
        }

        // ---------------- right: how it plays ----------------
        Card
        {
            id: playCard
            title: qsTr("PLAY")
            x: 1280; y: 100; width: parent.width - 1300; height: 640
            readonly property real inX: 20
            readonly property real inW: width - 40

            Row
            {
                x: playCard.inX; y: 56; spacing: 12
                LabTile { objectName: "labBack"; width: (playCard.inW - 24) / 3; height: 70; text: "←  " + qsTr("BACK"); fontPx: 18; onTapped: if (eng) eng.labBack() }
                LabTile
                {
                    objectName: "labHold"
                    width: (playCard.inW - 24) / 3; height: 70; text: "❚❚  " + qsTr("HOLD"); fontPx: 18
                    on: eng ? eng.labHold : false; tone: labRoot.cGold
                    onTapped: if (eng) eng.labToggleHold()
                }
                LabTile { objectName: "labNext"; width: (playCard.inW - 24) / 3; height: 70; text: qsTr("NEXT") + "  →"; fontPx: 18; onTapped: if (eng) eng.labNext() }
            }
            Label { x: playCard.inX + 2; y: 148; text: qsTr("AUTO-PLAY") }
            Row
            {
                x: playCard.inX; y: 172; spacing: 12
                Repeater
                {
                    model: [ qsTr("OFF"), qsTr("2 s"), qsTr("4 s"), qsTr("8 BEATS") ]
                    LabTile
                    {
                        objectName: "labAuto:" + index
                        width: (playCard.inW - 36) / 4; height: 62; text: modelData; fontPx: 16
                        on: eng ? eng.labAuto === index : index === 0; tone: labRoot.cBlue
                        onTapped: if (eng) eng.labSetAuto(index)
                    }
                }
            }
            Label { x: playCard.inX + 2; y: 258; text: qsTr("TRY IN COLOUR") }
            Row
            {
                id: colourRow
                x: playCard.inX; y: 282; spacing: 12
                readonly property var colours: eng ? eng.labColours : []
                Repeater
                {
                    model: colourRow.colours
                    LabTile
                    {
                        objectName: "labColour:" + modelData
                        width: (playCard.inW - 12 * (colourRow.colours.length - 1)) / Math.max(1, colourRow.colours.length); height: 62
                        text: modelData.toUpperCase(); tone: labRoot.swatch(modelData); bar: true
                        fontPx: colourRow.colours.length > 4 ? 13 : 15
                        on: eng ? eng.labColour === modelData : false
                        onTapped: if (eng) eng.labSetColour(modelData)
                    }
                }
            }
            Label { x: playCard.inX + 2; y: 368; text: qsTr("LAMPS") }
            Row
            {
                x: playCard.inX; y: 392; spacing: 12
                Repeater
                {
                    model: [ qsTr("BOTH"), qsTr("LEFT"), qsTr("RIGHT") ]
                    LabTile
                    {
                        objectName: "labLamps:" + index
                        width: (playCard.inW - 24) / 3; height: 62; text: modelData; fontPx: 15
                        enabled: index === 0 || (eng ? eng.labLampCount >= 2 : false)
                        on: eng ? eng.labLamps === index : index === 0; tone: labRoot.cBlue
                        onTapped: if (eng) eng.labSetLamps(index)
                    }
                }
            }
            // the two safety tiles
            LabTile
            {
                objectName: "labBan"
                x: playCard.inX; y: 486; width: (playCard.inW - 12) * 0.55; height: 120
                text: "⚠  " + qsTr("TOO LOW / AUDIENCE"); tone: "#C9782C"; bar: true; fontPx: 17
                onTapped: if (eng) eng.labBan()
            }
            Rectangle
            {
                objectName: "labStop"
                x: playCard.inX + (playCard.inW - 12) * 0.55 + 12; y: 486
                width: (playCard.inW - 12) * 0.45; height: 120; radius: 10
                readonly property bool dark: eng ? eng.labDark : false
                color: stopArea.pressed ? "#8E2626" : (dark ? labRoot.cBtn : "#B03030")
                Text
                {
                    anchors.centerIn: parent
                    text: parent.dark ? "▶  " + qsTr("PLAY") : qsTr("STOP")
                    color: "#FFFFFF"; font.bold: true; font.pixelSize: 34; font.letterSpacing: 4
                }
                MouseArea { id: stopArea; anchors.fill: parent; onClicked: if (eng) eng.labStop() }
            }
        }

        // ---------------- bottom: where it belongs ----------------
        Card
        {
            id: tagCard
            objectName: "labTags"
            title: labRoot.kept ? qsTr("WHERE IT BELONGS  ·  for %1").arg(eng.labTagTitle)
                                : qsTr("WHERE IT BELONGS  ·  keep a look first (✓ GOOD)")
            x: 20; y: 760; width: parent.width - 40; height: 330
            readonly property real cX: 180

            Item
            {
                anchors.fill: parent
                enabled: labRoot.kept
                opacity: enabled ? 1.0 : 0.4

                Label { x: 32; y: 76; text: qsTr("SECTION") }
                Row
                {
                    x: tagCard.cX; y: 56; spacing: 12
                    Repeater
                    {
                        model: [ ["BREAK", "break", "#4F8FE3"], ["GROOVE", "groove", "#9A9AA6"], ["BUILD", "build", "#E0921A"], ["DROP", "drop", "#E0555A"], ["ANYWHERE", "any", "#6ECD82"] ]
                        LabTile
                        {
                            objectName: "labSec:" + modelData[1]
                            width: 190; height: 58; text: qsTr(modelData[0]); tone: modelData[2]; bar: true; fontPx: 15
                            on: eng ? (modelData[1] === "any" ? labRoot.kept && eng.labSections.length === 0 : labRoot.has(eng.labSections, modelData[1])) : false
                            onTapped: if (eng) eng.labToggleTag("sec", modelData[1])
                        }
                    }
                }
                Label { x: 32; y: 146; text: qsTr("ENERGY") }
                Row
                {
                    x: tagCard.cX; y: 126; spacing: 12
                    Repeater
                    {
                        model: [ ["LOW · –80 %", "low"], ["MID · 75–90 %", "mid"], ["HIGH · 85 %+", "high"], ["TOP · 95 %+", "top"] ]
                        LabTile
                        {
                            objectName: "labEn:" + modelData[1]
                            width: 190; height: 58; text: modelData[0]; tone: labRoot.cGold; bar: true; fontPx: 15
                            on: eng ? labRoot.has(eng.labEnergy, modelData[1]) : false
                            onTapped: if (eng) eng.labToggleTag("en", modelData[1])
                        }
                    }
                }
                Label { x: 32; y: 216; text: qsTr("CHARACTER") }
                Row
                {
                    x: tagCard.cX; y: 196; spacing: 12
                    Repeater
                    {
                        model: [ ["CALM", "calm"], ["BUSY", "busy"], ["HARD", "hard"], ["WIDE", "wide"], ["TIGHT", "tight"] ]
                        LabTile
                        {
                            objectName: "labCh:" + modelData[1]
                            width: 150; height: 58; text: qsTr(modelData[0]); tone: labRoot.cBlue; fontPx: 15
                            on: eng ? labRoot.has(eng.labCharacter, modelData[1]) : false
                            onTapped: if (eng) eng.labToggleTag("ch", modelData[1])
                        }
                    }
                }
                Label { x: tagCard.width - 560; y: 76; text: qsTr("HOW OFTEN") }
                Row
                {
                    x: tagCard.width - 560; y: 98; spacing: 12
                    Repeater
                    {
                        model: [ "★", "★★", "★★★" ]
                        LabTile
                        {
                            objectName: "labStars:" + (index + 1)
                            width: 168; height: 58; text: modelData; tone: labRoot.cGold; fontPx: 20
                            on: eng ? (eng.labStars === index + 1 || (index === 0 && labRoot.kept && eng.labStars === 0)) : false
                            onTapped: if (eng) eng.labSetStars(index + 1)
                        }
                    }
                }
                Label { x: tagCard.width - 560; y: 176; text: qsTr("NOTE") }
                Rectangle
                {
                    x: tagCard.width - 560; y: 198; width: 528; height: 58; radius: 10; color: "#0A0A0D"
                    TextInput
                    {
                        id: noteInput
                        objectName: "labNote"
                        x: 18; width: parent.width - 60; anchors.verticalCenter: parent.verticalCenter
                        color: labRoot.cText; font.pixelSize: 17
                        clip: true
                        maximumLength: 200
                        text: eng ? eng.labNote : ""
                        onEditingFinished: if (eng) eng.labSetNote(text)
                        onActiveFocusChanged: if (!activeFocus && eng) eng.labSetNote(text)
                    }
                    Text
                    {
                        x: 18; anchors.verticalCenter: parent.verticalCenter
                        visible: noteInput.text.length === 0 && !noteInput.activeFocus
                        text: qsTr("where it fits, what it goes with …"); color: labRoot.cDim; font.pixelSize: 17
                    }
                    Text { anchors.right: parent.right; anchors.rightMargin: 16; anchors.verticalCenter: parent.verticalCenter; text: "⌨"; color: labRoot.cMute; font.pixelSize: 22 }
                    Connections
                    {
                        target: eng
                        function onLabChanged() { if (!noteInput.activeFocus) noteInput.text = eng.labNote }
                    }
                }
            }
            Text
            {
                x: 32; y: 282; width: tagCard.width - 64
                elide: Text.ElideRight
                text: qsTr("Tags are optional. A plain ✓ GOOD is a look for anywhere, one star. The engine uses ★ as it does for the show's other programmes.")
                color: labRoot.cDim; font.pixelSize: 15
            }
        }
    }
}
