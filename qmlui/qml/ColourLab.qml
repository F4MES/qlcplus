/*
  Q Light Controller Plus
  ColourLab.qml

  COLOUR LAB (runde 440, mockup N2): one colour at a time, every group that
  can show it lit side by side (or one by one), and how bright each group is
  IN that colour - "nogle lamper lyser f.eks. CYAN meget lysere end andre
  grupper". The level is used in the show (trimOf). The engine half is
  track_runtime/colourlab.inc.cpp (trackEngine.clab*). Loaded by
  TrackSetup.qml over the whole Track page, from SETUP > LAB.
  Flat (the r403 tones): no gradients, no glow, no shadows.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

import QtQuick

Rectangle
{
    id: clabRoot
    objectName: "colourLab"
    color: "#0A0A0D"

    // nothing under the lab takes a finger
    MouseArea { anchors.fill: parent }

    readonly property var eng: trackEngine
    readonly property string colour: eng ? eng.clabColour : "blue"
    readonly property var colours: eng ? eng.clabColours : []
    readonly property var rows: eng ? eng.clabRows : []
    readonly property var matrix: eng ? eng.clabMatrix : []
    readonly property string solo: eng ? eng.clabSolo : ""
    readonly property bool dark: eng ? eng.clabDark : true
    readonly property color cCard: "#141418"
    readonly property color cBtn: "#202027"
    readonly property color cText: "#E9E9EE"
    readonly property color cMute: "#8C8C96"
    readonly property color cDim: "#5E5E68"
    readonly property color cGold: "#E3B44F"
    readonly property color cGoldDark: "#3C2E12"
    readonly property color cBlue: "#4FA3E3"
    readonly property color cRed: "#B03030"
    readonly property real pad: 18
    // five groups fit at full height; more share the card
    readonly property real rowH: Math.min(88, Math.max(52, 486 / Math.max(1, rows.length) - 10))
    readonly property real cellH: Math.min(64, Math.max(36, 220 / Math.max(1, matrix.length) - 10))

    function swatch(c)
    {
        switch (c)
        {
        case "red":     return "#E23232"
        case "orange":  return "#E88028"
        case "yellow":  return "#E8D228"
        case "green":   return "#3CC85A"
        case "cyan":    return "#28C8DC"
        case "blue":    return "#3C5AEB"
        case "magenta": return "#DC3CD2"
        case "white":   return "#EBEBF0"
        }
        return "#B8B8C4"
    }
    function ink(c)
    {
        var t = Qt.color(c)
        return 0.299 * t.r + 0.587 * t.g + 0.114 * t.b > 0.5 ? "#101010" : "#FFFFFF"
    }
    function pct(v) { return Math.round(v * 100) + " %" }

    component LabTile: Rectangle
    {
        id: tile
        property string text: ""
        property bool on: false
        property color tone: clabRoot.cBlue
        property real fontPx: 16
        property color bar: "transparent"
        signal tapped()
        radius: 8
        color: on ? tone : (tileArea.pressed ? "#2C2C33" : clabRoot.cBtn)
        opacity: enabled ? 1.0 : 0.35
        Text
        {
            anchors.centerIn: parent
            width: parent.width - 12
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: tile.text
            color: tile.on ? clabRoot.ink(tile.tone) : "#D6D6DC"
            font.bold: true; font.pixelSize: tile.fontPx; font.letterSpacing: tile.fontPx * 0.06
        }
        Rectangle
        {
            visible: !tile.on && tile.bar !== Qt.color("transparent")
            anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
            anchors.leftMargin: 16; anchors.rightMargin: 16; anchors.bottomMargin: 7
            height: 4; color: tile.bar
        }
        MouseArea { id: tileArea; anchors.fill: parent; onClicked: tile.tapped() }
    }
    component Label: Text
    {
        color: clabRoot.cMute; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.6
    }

    // the mockup's 2048 x 1152 board, scaled to the page
    Item
    {
        id: board
        width: 2048; height: 1152
        readonly property real ks: Math.min(clabRoot.width / 2048, clabRoot.height / 1152)
        scale: ks
        transformOrigin: Item.TopLeft
        x: Math.round((clabRoot.width - 2048 * ks) / 2)
        y: Math.round((clabRoot.height - 1152 * ks) / 2)

        // ---------------- top bar ----------------
        Item
        {
            x: 20; y: 16; width: parent.width - 40; height: 64
            Rectangle
            {
                width: 230; height: 56; radius: 10; color: clabRoot.cGold
                Text { anchors.centerIn: parent; text: "COLOUR LAB"; color: "#1A1205"; font.bold: true; font.pixelSize: 22; font.letterSpacing: 2 }
            }
            LabTile
            {
                objectName: "clabBackToLab"
                x: 246; width: 96; height: 40; anchors.verticalCenter: parent.verticalCenter
                text: "‹ " + qsTr("LAB"); fontPx: 15
                onTapped: if (eng) eng.clabClose()
            }
            Column
            {
                x: 362; anchors.verticalCenter: parent.verticalCenter; spacing: 4
                Text { text: qsTr("every lamp type  ·  one colour at a time"); color: clabRoot.cText; font.pixelSize: 26 }
                Text { text: qsTr("SHOW OFF  ·  set how bright each group is IN each colour  ·  saved as you go"); color: clabRoot.cMute; font.pixelSize: 15 }
            }
            LabTile
            {
                objectName: "clabClose"
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                width: 130; height: 56; text: qsTr("CLOSE"); onTapped: if (eng) eng.clabClose()
            }
        }

        // ---------------- the colours ----------------
        Label { x: 30; y: 96; text: qsTr("COLOUR") }
        Row
        {
            x: 30; y: 120; spacing: 10
            Repeater
            {
                model: clabRoot.colours
                LabTile
                {
                    objectName: "clabColour:" + modelData
                    width: (2048 - 60 - 10 * (clabRoot.colours.length - 1)) / Math.max(1, clabRoot.colours.length); height: 66
                    text: modelData.toUpperCase(); fontPx: 19
                    tone: clabRoot.swatch(modelData); bar: clabRoot.swatch(modelData)
                    on: clabRoot.colour === modelData
                    onTapped: if (eng) eng.clabSetColour(modelData)
                }
            }
        }

        // ---------------- the groups in this colour ----------------
        Rectangle
        {
            id: groupCard
            x: 20; y: 206; width: 1312; height: 580; radius: 14; color: clabRoot.cCard
            Label
            {
                x: clabRoot.pad + 2; y: 14
                text: clabRoot.colour.toUpperCase() + "  ·  " + (clabRoot.solo === "" ? qsTr("ALL GROUPS SIDE BY SIDE") : qsTr("ONLY %1").arg(clabRoot.solo.toUpperCase()))
                      + "  ·  " + qsTr("tap a name = only that group (tap again = all)")
            }
            Column
            {
                x: 16; y: 46; spacing: 10
                Repeater
                {
                    model: clabRoot.rows
                    Rectangle
                    {
                        id: row
                        objectName: "clabRow:" + modelData.key
                        width: 1280; height: clabRoot.rowH; radius: 8
                        readonly property bool can: modelData.can === true
                        readonly property real level: modelData.level
                        color: can ? "#22222A" : "#1A1A1E"
                        // the name: tap = only this group
                        Item
                        {
                            objectName: "clabName:" + modelData.key
                            x: 0; y: 0; width: 300; height: parent.height
                            Rectangle
                            {
                                visible: clabRoot.solo === modelData.key
                                x: 0; y: 0; width: 6; height: parent.height; color: clabRoot.cGold
                            }
                            Text
                            {
                                x: 24; y: modelData.note === "" && !modelData.base ? (parent.height - 26) / 2 : parent.height / 2 - 28; width: 270; elide: Text.ElideRight
                                text: modelData.key; color: row.can ? clabRoot.cText : clabRoot.cDim
                                font.bold: true; font.pixelSize: 22
                            }
                            Text
                            {
                                x: 24; y: parent.height / 2 + 6; width: 270; elide: Text.ElideRight
                                visible: text !== ""
                                text: modelData.note !== "" ? modelData.note : (modelData.base ? qsTr("BASE") : "")
                                color: modelData.note !== "" ? clabRoot.cDim : clabRoot.cBlue
                                font.pixelSize: 14
                            }
                            MouseArea { anchors.fill: parent; enabled: row.can; onClicked: if (eng) eng.clabSoloGroup(modelData.key) }
                        }
                        // what it sends: the colour at its level (dark when it is not lit)
                        Rectangle
                        {
                            visible: row.can
                            x: 310; y: (row.height - height) / 2; width: 70; height: row.height - 32; radius: 6
                            color: modelData.lit ? Qt.rgba(Qt.color(clabRoot.swatch(clabRoot.colour)).r * row.level,
                                                           Qt.color(clabRoot.swatch(clabRoot.colour)).g * row.level,
                                                           Qt.color(clabRoot.swatch(clabRoot.colour)).b * row.level, 1) : "#101014"
                            border.width: modelData.lit ? 0 : 1; border.color: "#2C2C33"
                        }
                        // the level - press or drag, 10-100 %
                        Rectangle
                        {
                            id: fader
                            objectName: "clabLevel:" + modelData.key
                            visible: row.can
                            x: 400; y: (row.height - height) / 2; width: 680; height: row.height - 32; radius: 6; color: "#0E0E12"
                            Rectangle
                            {
                                width: Math.max(10, fader.width * row.level); height: parent.height; radius: 6
                                color: row.level >= 0.995 ? Qt.darker(clabRoot.swatch(clabRoot.colour), 1.35) : clabRoot.cGold
                            }
                            Repeater
                            {
                                model: [0.25, 0.5, 0.75]
                                Rectangle { x: fader.width * modelData; y: fader.height - 8; width: 2; height: 8; color: "#50505A" }
                            }
                            Rectangle { x: Math.max(0, fader.width * row.level - 8); y: -4; width: 10; height: fader.height + 8; color: "#F0F0F4" }
                            Text
                            {
                                x: 16; anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("LEVEL IN %1").arg(clabRoot.colour.toUpperCase())
                                color: row.level > 0.3 ? "#101014" : clabRoot.cText
                                font.bold: true; font.pixelSize: 14; font.letterSpacing: 1.2
                            }
                            MouseArea
                            {
                                anchors.fill: parent
                                preventStealing: true
                                function setAt(mx)
                                {
                                    var v = Math.round(Math.max(0.10, Math.min(1.0, mx / fader.width)) * 100) / 100
                                    if (eng && Math.abs(v - row.level) >= 0.01) eng.clabSetLevel(modelData.key, v)
                                }
                                onPressed: (mouse) => setAt(mouse.x)
                                onPositionChanged: (mouse) => { if (pressed) setAt(mouse.x) }
                            }
                        }
                        Text
                        {
                            visible: row.can
                            x: 1090; width: 170; height: parent.height
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                            text: clabRoot.pct(row.level)
                            color: row.level >= 0.995 ? clabRoot.cText : clabRoot.cGold
                            font.bold: true; font.pixelSize: 28
                        }
                    }
                }
            }
            Text
            {
                x: 24; y: groupCard.height - 34; width: groupCard.width - 48
                elide: Text.ElideRight
                text: qsTr("100 % = as the show file made it. The engine multiplies this group's light by the level whenever it shows that colour - in the show and the startscene.")
                color: clabRoot.cMute; font.pixelSize: 14
            }
        }

        // ---------------- compare and test ----------------
        Rectangle
        {
            id: sideCard
            x: 1352; y: 206; width: 2048 - 1352 - 20; height: 580; radius: 14; color: clabRoot.cCard
            readonly property real inW: width - 40
            Label { x: 22; y: 14; text: qsTr("COMPARE") }
            Row
            {
                x: 20; y: 40; spacing: 12
                LabTile
                {
                    objectName: "clabCompare:0"
                    width: (sideCard.inW - 12) / 2; height: 64; text: qsTr("ALL SIDE BY SIDE")
                    on: clabRoot.solo === ""; onTapped: if (eng) eng.clabCompare(0)
                }
                LabTile
                {
                    objectName: "clabCompare:1"
                    width: (sideCard.inW - 12) / 2; height: 64; text: qsTr("ONE BY ONE")
                    on: clabRoot.solo !== ""; onTapped: if (eng) eng.clabCompare(1)
                }
            }
            Label { x: 22; y: 124; text: qsTr("TEST LEVEL") }
            Row
            {
                x: 20; y: 150; spacing: 12
                Repeater
                {
                    model: [0.3, 0.6, 1.0]
                    LabTile
                    {
                        objectName: "clabTest:" + index
                        width: (sideCard.inW - 24) / 3; height: 58; text: Math.round(modelData * 100) + " %"
                        on: eng ? Math.abs(eng.clabTest - modelData) < 0.01 : index === 1
                        onTapped: if (eng) eng.clabSetTest(modelData)
                    }
                }
            }
            Text
            {
                x: 22; y: 218; width: sideCard.inW; wrapMode: Text.WordWrap
                text: qsTr("how bright the groups are lit while you compare (with MASTER) - not saved")
                color: clabRoot.cDim; font.pixelSize: 13
            }
            Label { x: 22; y: 262; text: clabRoot.solo !== "" ? clabRoot.solo.toUpperCase() : qsTr("TAP A NAME TO PICK A GROUP") }
            Row
            {
                x: 20; y: 288; spacing: 12
                enabled: clabRoot.solo !== ""
                LabTile
                {
                    objectName: "clabCopyAll"
                    width: (sideCard.inW - 12) * 0.68; height: 58; fontPx: 15
                    text: qsTr("COPY %1 TO ALL COLOURS").arg(clabRoot.pct(soloLevel()))
                    function soloLevel()
                    {
                        for (var i = 0; i < clabRoot.rows.length; i++)
                            if (clabRoot.rows[i].key === clabRoot.solo) return clabRoot.rows[i].level
                        return 1.0
                    }
                    onTapped: if (eng) eng.clabCopyToAll(clabRoot.solo)
                }
                LabTile
                {
                    objectName: "clabReset"
                    width: (sideCard.inW - 12) * 0.32; height: 58; fontPx: 15
                    text: qsTr("RESET %1").arg(clabRoot.colour.toUpperCase())
                    onTapped: if (eng) eng.clabResetColour(clabRoot.solo)
                }
            }
            Text
            {
                objectName: "clabMessage"
                x: 22; y: 362; width: sideCard.inW; elide: Text.ElideRight
                visible: text !== ""
                text: eng ? eng.clabMessage : ""
                color: clabRoot.cGold; font.bold: true; font.pixelSize: 15
            }
            Rectangle
            {
                objectName: "clabDark"
                x: 20; y: 410; width: (sideCard.inW - 12) / 2; height: 150; radius: 10
                color: darkArea.pressed ? "#2C2C33" : (clabRoot.dark ? clabRoot.cBlue : clabRoot.cBtn)
                Text
                {
                    anchors.centerIn: parent
                    text: clabRoot.dark ? "▶  " + qsTr("ALL ON") : qsTr("ALL OFF")
                    color: clabRoot.dark ? "#0A1622" : "#FFFFFF"; font.bold: true; font.pixelSize: 26; font.letterSpacing: 3
                }
                MouseArea { id: darkArea; anchors.fill: parent; onClicked: if (eng) eng.clabToggleDark() }
            }
            Rectangle
            {
                objectName: "clabStop"
                x: 32 + (sideCard.inW - 12) / 2; y: 410; width: (sideCard.inW - 12) / 2; height: 150; radius: 10
                color: stopArea.pressed ? "#8E2626" : clabRoot.cRed
                Text { anchors.centerIn: parent; text: qsTr("STOP"); color: "#FFFFFF"; font.bold: true; font.pixelSize: 34; font.letterSpacing: 4 }
                MouseArea { id: stopArea; anchors.fill: parent; onClicked: if (eng) eng.clabClose() }
            }
        }

        // ---------------- every level ----------------
        Rectangle
        {
            id: matrixCard
            x: 20; y: 806; width: 2048 - 40; height: 330; radius: 14; color: clabRoot.cCard
            readonly property real cx0: 330
            readonly property real cw: (width - cx0 - 30) / Math.max(1, clabRoot.colours.length)
            Label { x: 22; y: 12; text: qsTr("ALL LEVELS  ·  tap a box = go there") }
            Repeater
            {
                model: clabRoot.colours
                Item
                {
                    x: matrixCard.cx0 + index * matrixCard.cw; y: 36; width: matrixCard.cw - 12; height: 30
                    Text { anchors.horizontalCenter: parent.horizontalCenter; text: modelData.toUpperCase(); color: clabRoot.cMute; font.bold: true; font.pixelSize: 14 }
                    Rectangle { anchors.horizontalCenter: parent.horizontalCenter; y: 24; width: parent.width - 60; height: 3; color: clabRoot.swatch(modelData) }
                }
            }
            Column
            {
                x: 0; y: 76; spacing: 10
                Repeater
                {
                    model: clabRoot.matrix
                    Item
                    {
                        width: matrixCard.width; height: clabRoot.cellH
                        readonly property string gkey: modelData.key
                        readonly property var levels: modelData.levels
                        Text { x: 28; anchors.verticalCenter: parent.verticalCenter; width: 290; elide: Text.ElideRight; text: parent.gkey; color: clabRoot.cText; font.bold: true; font.pixelSize: 18 }
                        Repeater
                        {
                            model: parent.levels
                            Rectangle
                            {
                                objectName: "clabCell:" + parent.gkey + "/" + clabRoot.colours[index]
                                x: matrixCard.cx0 + index * matrixCard.cw; width: matrixCard.cw - 12; height: clabRoot.cellH; radius: 6
                                readonly property real v: modelData
                                readonly property bool here: clabRoot.colours[index] === clabRoot.colour
                                color: v < 0 ? "#18181C" : (v < 0.995 ? clabRoot.cGoldDark : "#22222A")
                                border.width: here ? 2 : 0; border.color: "#F0F0F4"
                                Rectangle { visible: v >= 0 && v < 0.995; anchors.bottom: parent.bottom; width: parent.width; height: 4; color: clabRoot.cGold }
                                Text
                                {
                                    anchors.centerIn: parent
                                    text: v < 0 ? "—" : clabRoot.pct(v)
                                    color: v < 0 ? clabRoot.cDim : (v < 0.995 ? clabRoot.cGold : clabRoot.cDim)
                                    font.bold: v >= 0 && v < 0.995; font.pixelSize: 17
                                }
                                MouseArea { anchors.fill: parent; enabled: v >= 0; onClicked: if (eng) eng.clabSetColour(clabRoot.colours[index]) }
                            }
                        }
                    }
                }
            }
            Text
            {
                x: 28; y: matrixCard.height - 30; width: matrixCard.width - 56; elide: Text.ElideRight
                text: qsTr("Gold = changed from the show file.  — = the group has no scene in that colour.  Strobes never fire their hardware strobe here.  Haze is never touched.")
                color: clabRoot.cDim; font.pixelSize: 14
            }
        }
    }
}
