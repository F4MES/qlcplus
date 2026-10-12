/*
  Q Light Controller Plus
  StrobeLab.qml

  STROBE LAB (runde 436, mockup 10b): the strobes' AUTO programmes one at a
  time, with SHOW OFF - BREAK, GROOVE and DROP as the engine files them. NO
  bans one in the show, GOOD has it drawn more often (RATINGS), SKIP decides
  later. Nothing is deleted. The engine half is track_runtime/strobelab.inc.cpp
  (trackEngine.slab*). Loaded by TrackSetup.qml over the whole Track page, from
  SETUP > LAB. Flat (the r403 tones): no gradients, no glow, no shadows.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

import QtQuick

Rectangle
{
    id: slabRoot
    objectName: "strobeLab"
    color: "#0A0A0D"

    // nothing under the lab takes a finger
    MouseArea { anchors.fill: parent }

    readonly property var eng: trackEngine
    readonly property int tab: eng ? eng.slabTab : 0
    readonly property var counts: eng ? eng.slabTabCounts : [0, 0, 0]
    readonly property color cCard: "#141418"
    readonly property color cBtn: "#202027"
    readonly property color cText: "#E9E9EE"
    readonly property color cMute: "#8C8C96"
    readonly property color cDim: "#5E5E68"
    readonly property color cGreen: "#6ECD82"
    readonly property color cRed: "#E0555A"
    readonly property color cGold: "#E3B44F"
    readonly property color cBlue: "#4FA3E3"
    readonly property real pad: 18
    readonly property var tabNames: [ qsTr("BREAK"), qsTr("GROOVE"), qsTr("DROP") ]

    function swatch(c)
    {
        switch (c)
        {
        case "white": return "#F2F2F5"
        case "red":   return slabRoot.cRed
        case "blue":  return "#3E6FE8"
        }
        return "#B8B8C4"
    }

    component LabTile: Rectangle
    {
        id: tile
        property string text: ""
        property string sub: ""
        property bool on: false
        property color tone: slabRoot.cGreen
        property real fontPx: 15
        property bool bar: false
        signal tapped()
        radius: 10
        color: on ? tone : (tileArea.pressed ? "#2C2C33" : slabRoot.cBtn)
        opacity: enabled ? 1.0 : 0.35
        readonly property color ink: on ? (0.299 * tone.r + 0.587 * tone.g + 0.114 * tone.b > 0.5 ? "#101010" : "#FFFFFF") : "#D6D6DC"
        Column
        {
            anchors.centerIn: parent
            width: parent.width - 12
            spacing: 6
            Text
            {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: tile.text
                color: tile.ink
                font.bold: true; font.pixelSize: tile.fontPx; font.letterSpacing: tile.fontPx * 0.08
            }
            Text
            {
                visible: tile.sub !== ""
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: tile.sub
                color: tile.on ? tile.ink : slabRoot.cMute
                font.pixelSize: 16
            }
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
        color: slabRoot.cMute; font.bold: true; font.pixelSize: 12; font.letterSpacing: 1.8
    }
    component Card: Rectangle
    {
        property string title: ""
        color: slabRoot.cCard; radius: 14
        Label { x: slabRoot.pad + 2; y: slabRoot.pad; width: parent.width - 2 * slabRoot.pad; elide: Text.ElideRight; text: parent.title }
    }

    // the mockup's 2048 x 1152 board, scaled to the page
    Item
    {
        id: board
        width: 2048; height: 1152
        readonly property real ks: Math.min(slabRoot.width / 2048, slabRoot.height / 1152)
        scale: ks
        transformOrigin: Item.TopLeft
        x: Math.round((slabRoot.width - 2048 * ks) / 2)
        y: Math.round((slabRoot.height - 1152 * ks) / 2)

        // ---------------- top bar ----------------
        Item
        {
            x: 20; y: 16; width: parent.width - 40; height: 64
            Rectangle
            {
                width: 210; height: 56; radius: 10; color: slabRoot.cGold
                Text { anchors.centerIn: parent; text: "STROBE LAB"; color: "#1A1205"; font.bold: true; font.pixelSize: 22; font.letterSpacing: 2 }
            }
            LabTile
            {
                objectName: "slabBackToLab"
                x: 226; width: 96; height: 40; anchors.verticalCenter: parent.verticalCenter
                text: "‹ " + qsTr("LAB"); fontPx: 15
                onTapped: if (eng) eng.slabClose()
            }
            Column
            {
                x: 340; anchors.verticalCenter: parent.verticalCenter; spacing: 4
                Text
                {
                    text: (eng ? eng.slabGroup : "") + "  ·  " + (eng ? eng.slabLampCount : 0) + ((eng && eng.slabLampCount === 1) ? " lamp" : " lamps")
                    color: slabRoot.cText; font.pixelSize: 26
                }
                Text { text: qsTr("SHOW OFF  ·  the rig is yours  ·  everything is saved as you go"); color: slabRoot.cMute; font.pixelSize: 15 }
            }
            Row
            {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; spacing: 10
                Repeater
                {
                    model: 3
                    LabTile
                    {
                        objectName: "slabTab:" + index
                        width: 170; height: 56; text: slabRoot.tabNames[index] + "  " + (slabRoot.counts.length > index ? slabRoot.counts[index] : 0)
                        on: slabRoot.tab === index; tone: slabRoot.cBlue; fontPx: 16
                        onTapped: if (eng) eng.slabSetTab(index)
                    }
                }
                Item { width: 18; height: 1 }
                LabTile { objectName: "slabClose"; width: 130; height: 56; text: qsTr("CLOSE"); fontPx: 16; onTapped: if (eng) eng.slabClose() }
            }
        }

        // ---------------- left: what plays now ----------------
        Card
        {
            id: nowCard
            title: qsTr("NOW PLAYING") + "  ·  " + slabRoot.tabNames[slabRoot.tab] + "  ·  "
                   + (eng && eng.slabCount > 0 ? (eng.slabIndex + 1) : 0) + " " + qsTr("OF") + " " + (eng ? eng.slabCount : 0)
            x: 20; y: 100; width: 1240; height: 640

            Text
            {
                objectName: "slabTitle"
                x: 40; y: 60; width: 1160
                elide: Text.ElideRight
                text: eng ? (eng.slabCount > 0 ? eng.slabTitle : qsTr("Nothing of this kind")) : ""
                color: slabRoot.cText; font.bold: true
                font.pixelSize: text.length > 16 ? 56 : 84
            }
            Text
            {
                objectName: "slabDetail"
                x: 44; y: 168; width: 1150
                elide: Text.ElideRight
                text: eng ? eng.slabDetail : ""
                color: slabRoot.cMute; font.pixelSize: 20
            }
            Row
            {
                x: 40; y: 212; spacing: 10
                Repeater
                {
                    model: eng ? eng.slabChips : []
                    Rectangle
                    {
                        height: 34; width: chipT.implicitWidth + 28; radius: 17
                        color: modelData === "BANNED" ? "#3A1416" : "#1C1C22"
                        Text
                        {
                            id: chipT; anchors.centerIn: parent; text: modelData
                            color: modelData === "BANNED" ? "#F05050" : slabRoot.cMute
                            font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.2
                        }
                    }
                }
            }

            // the decision - three big tiles
            Row
            {
                x: 40; y: 300; spacing: 16
                enabled: eng ? eng.slabCount > 0 : false
                LabTile
                {
                    objectName: "slabNo"
                    width: 330; height: 150; text: "✕  " + qsTr("NO"); sub: qsTr("banned - never drawn")
                    tone: slabRoot.cRed; bar: true; fontPx: 36
                    on: eng ? eng.slabVerdict === -1 : false
                    onTapped: if (eng) eng.slabDecide(-1)
                }
                LabTile
                {
                    objectName: "slabSkip"
                    width: 330; height: 150; text: "→  " + qsTr("SKIP"); sub: qsTr("decide later")
                    tone: slabRoot.cDim; bar: true; fontPx: 36
                    onTapped: if (eng) eng.slabDecide(0)
                }
                LabTile
                {
                    objectName: "slabGood"
                    width: 480; height: 150; text: "✓  " + qsTr("GOOD"); sub: qsTr("kept - drawn more often")
                    tone: slabRoot.cGreen; bar: true; fontPx: 40
                    on: eng ? eng.slabVerdict === 1 : false
                    onTapped: if (eng) eng.slabDecide(1)
                }
            }
            Text
            {
                x: 44; y: 466; width: 1150
                wrapMode: Text.WordWrap
                text: qsTr("NO bans it in the show - the engine never draws it. GOOD has it drawn more often (with RATINGS on). The same tile again takes it back. Nothing is deleted from the show file.")
                color: slabRoot.cDim; font.pixelSize: 16
            }
            Text
            {
                objectName: "slabMessage"
                x: 44; y: 494; width: 1150
                elide: Text.ElideRight
                visible: text !== ""
                text: eng ? eng.slabMessage : ""
                color: slabRoot.cGold; font.bold: true; font.pixelSize: 16
            }

            // the strip: every programme of the kind, where you are - tap to jump
            Label
            {
                x: 40; y: 522
                text: slabRoot.tabNames[slabRoot.tab] + "  ·  "
                      + (eng && eng.slabCount > 0 ? (eng.slabIndex + 1) : 0) + " " + qsTr("OF") + " " + (eng ? eng.slabCount : 0)
                      + "  ·  ✓ " + (eng ? eng.slabGood : 0) + "   ✕ " + (eng ? eng.slabNo : 0)
            }
            Item
            {
                id: strip
                objectName: "slabStrip"
                x: 40; y: 550; width: 1160; height: 46
                readonly property var marks: eng ? eng.slabStrip : []
                readonly property int n: marks.length
                readonly property real gap: n > 0 && width / n >= 6 ? 3 : (n > 0 && width / n >= 3 ? 1 : 0)
                readonly property real w: n > 0 ? Math.min(14, width / n - gap) : 9
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
                            color: eng && index === eng.slabIndex ? "#FFFFFF"
                                 : v === 1 ? slabRoot.cGreen
                                 : v === -1 ? slabRoot.cRed : "#26262E"
                        }
                    }
                }
                MouseArea
                {
                    anchors.fill: parent
                    onClicked: (mouse) => {
                        if (!eng || strip.n === 0) return
                        eng.slabGoto(Math.floor(mouse.x / Math.max(1, strip.w + strip.gap)))
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
                LabTile { objectName: "slabBack"; width: (playCard.inW - 24) / 3; height: 70; text: "←  " + qsTr("BACK"); fontPx: 18; onTapped: if (eng) eng.slabBack() }
                LabTile
                {
                    objectName: "slabHold"
                    width: (playCard.inW - 24) / 3; height: 70; text: "❚❚  " + qsTr("HOLD"); fontPx: 18
                    on: eng ? eng.slabHold : false; tone: slabRoot.cGold
                    onTapped: if (eng) eng.slabToggleHold()
                }
                LabTile { objectName: "slabNext"; width: (playCard.inW - 24) / 3; height: 70; text: qsTr("NEXT") + "  →"; fontPx: 18; onTapped: if (eng) eng.slabNext() }
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
                        objectName: "slabAuto:" + index
                        width: (playCard.inW - 36) / 4; height: 62; text: modelData; fontPx: 16
                        on: eng ? eng.slabAuto === index : index === 0; tone: slabRoot.cBlue
                        onTapped: if (eng) eng.slabSetAuto(index)
                    }
                }
            }
            Label { x: playCard.inX + 2; y: 258; text: qsTr("SPEED") }
            Row
            {
                x: playCard.inX; y: 282; spacing: 12
                Repeater
                {
                    model: [ qsTr("AS MADE"), "½×", "2×" ]
                    LabTile
                    {
                        objectName: "slabSpeed:" + index
                        width: (playCard.inW - 24) / 3; height: 62; text: modelData; fontPx: 16
                        enabled: index === 0 || (eng ? eng.slabCanSpeed : false)
                        on: eng ? eng.slabSpeed === index : index === 0; tone: slabRoot.cBlue
                        onTapped: if (eng) eng.slabSetSpeed(index)
                    }
                }
            }
            Label
            {
                x: playCard.inX + 2; y: 368
                text: (eng && eng.slabOwnColour) ? qsTr("COLOUR  ·  this one paints its own") : qsTr("COLOUR")
            }
            Row
            {
                x: playCard.inX; y: 392; spacing: 12
                enabled: eng ? !eng.slabOwnColour : true
                Repeater
                {
                    model: [ "room", "red", "blue", "white" ]
                    LabTile
                    {
                        objectName: "slabColour:" + modelData
                        width: (playCard.inW - 36) / 4; height: 62; text: modelData.toUpperCase(); fontPx: 15
                        tone: slabRoot.swatch(modelData); bar: modelData !== "room"
                        on: eng ? eng.slabColour === modelData : modelData === "room"
                        onTapped: if (eng) eng.slabSetColour(modelData)
                    }
                }
            }
            LabTile
            {
                objectName: "slabTest"
                x: playCard.inX; y: 486; width: (playCard.inW - 12) * 0.55; height: 120
                text: qsTr("ALL LAMPS ON - TEST"); fontPx: 17; tone: slabRoot.cGold
                on: eng ? eng.slabTest : false
                onTapped: if (eng) eng.slabToggleTest()
            }
            Rectangle
            {
                objectName: "slabStop"
                x: playCard.inX + (playCard.inW - 12) * 0.55 + 12; y: 486
                width: (playCard.inW - 12) * 0.45; height: 120; radius: 10
                readonly property bool dark: eng ? eng.slabDark : true
                color: stopArea.pressed ? "#8E2626" : (dark ? slabRoot.cBtn : "#B03030")
                Text
                {
                    anchors.centerIn: parent
                    text: parent.dark ? "▶  " + qsTr("PLAY") : qsTr("STOP")
                    color: "#FFFFFF"; font.bold: true; font.pixelSize: 34; font.letterSpacing: 4
                }
                MouseArea { id: stopArea; anchors.fill: parent; onClicked: if (eng) eng.slabStop() }
            }
        }

        // ---------------- bottom: what the engine does with it ----------------
        Card
        {
            id: factCard
            title: qsTr("WHAT THE ENGINE DOES WITH IT")
            x: 20; y: 760; width: parent.width - 40; height: 330
            Column
            {
                x: 30; y: 62; spacing: 18
                Repeater
                {
                    model: [ [ qsTr("BREAK"), qsTr("a break the strobes are in runs one of these 7 times in 10 (runde 433)") ],
                             [ qsTr("GROOVE"), qsTr("drawn about one groove section in three, from 30 % on the slider") ],
                             [ qsTr("DROP"), qsTr("drawn about half the drops; never the whole bank on full (runde 338)") ] ]
                    Row
                    {
                        spacing: 20
                        Rectangle
                        {
                            width: 140; height: 46; radius: 6; color: slabRoot.cBtn
                            Text { anchors.centerIn: parent; text: modelData[0]; color: slabRoot.cText; font.bold: true; font.pixelSize: 15 }
                        }
                        Text { anchors.verticalCenter: parent.verticalCenter; text: modelData[1]; color: slabRoot.cMute; font.pixelSize: 18 }
                    }
                }
            }
            Text
            {
                x: 30; y: 282; width: factCard.width - 60
                elide: Text.ElideRight
                text: qsTr("The lab never fires the strobes' hardware strobe - the programmes blink on the dimmer, and the colour scene under them keeps the shutter shut.")
                color: slabRoot.cDim; font.pixelSize: 15
            }
        }
    }
}
