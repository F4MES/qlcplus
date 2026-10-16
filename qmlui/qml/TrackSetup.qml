/*
  Q Light Controller Plus
  TrackSetup.qml

  Setup for the automatic busker: what each look DOES (colour, motion,
  position, flash), which fixture groups may be used, and how calm the
  palette is. One tap per row. The engine handles the rest.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

import QtQuick
import QtQuick.Layouts

import org.qlcplus.classes 1.0
import "."

Rectangle
{
    id: setupRoot
    color: "#262626"
    radius: 4

    // First child, so every control below sits above it: a plain Rectangle
    // lets a touch through to the page underneath, and the gaps between the
    // SETUP tiles lie over SHOW ON/OFF, START SCENE and the waveform - a
    // finger slightly off a tile switched the show off mid-set, or dragged a
    // section flag (runde 176).
    MouseArea { anchors.fill: parent }

    // R436_LAB_MENU: one lab in the LAB menu
    component LabCard: Rectangle
    {
        id: card
        property bool showOn: false
        property string title: ""
        property string sub: ""
        property string line1: ""
        property string line2: ""
        property bool live: true
        property bool can: true
        property string openName: ""
        signal open()
        height: 190; radius: 10
        color: live ? "#2A2A31" : "#232327"
        border.width: live ? 0 : 1
        border.color: "#34343A"
        Rectangle { visible: card.live; x: 18; y: 18; width: 5; height: 40; color: "#E3B44F" }
        Text { x: card.live ? 34 : 18; y: 14; text: card.title; color: card.live ? "#EEEEEE" : "#5E5E68"; font.bold: true; font.pixelSize: 20 }
        Text { x: card.live ? 34 : 18; y: 42; width: card.width - 50; elide: Text.ElideRight; text: card.sub; color: card.live ? "#9A9AA4" : "#55555E"; font.pixelSize: 13 }
        Text { x: 22; y: 78; width: card.width - 44; elide: Text.ElideRight; text: card.line1; color: "#E0E0E6"; font.pixelSize: 13 }
        Text { x: 22; y: 100; width: card.width - 44; elide: Text.ElideRight; text: card.line2; color: "#E0E0E6"; font.pixelSize: 13 }
        Text { visible: !card.live; x: 18; y: card.height - 34; text: qsTr("proposal - not built"); color: "#55555E"; font.pixelSize: 13 }
        TrackTile
        {
            objectName: card.openName
            visible: card.live
            x: 22; y: card.height - 56; width: card.width - 44; height: 40
            label: !card.can ? qsTr("NOT IN THIS SHOW") : (card.showOn ? qsTr("SHOW OFF FIRST") : qsTr("OPEN"))
            active: card.can && !card.showOn
            activeColor: "#E3B44F"
            onTapped: if (card.can && !card.showOn) card.open()
        }
    }

    // The TrackView page. Set by the Loader that brings this panel up - an id
    // from the other file is not in scope here, see the note at the Loader.
    property var host: null

    readonly property color cText: "#EEEEEE"
    readonly property color cDim:  "#9A9A9A"
    readonly property color cLine: "#555555"

    property string filter: ""
    property bool advancedOpen: false
    property bool cacheOpen: false
    property bool learnedOpen: false     // R436_LEARNED
    property bool labMenuOpen: false     // R436_LAB_MENU
    property string ioMessage: ""

    function roleColor(role)
    {
        switch (role)
        {
        case 0: return "#4FA3E3"   // colour
        case 1: return "#7ED07E"   // motion
        case 2: return "#E3B44F"   // position
        case 3: return "#E36B6B"   // flash
        case 4: return "#9A7ED0"   // start scene
        }
        return "#5A5A5A"
    }

    function roleShort(role)
    {
        switch (role)
        {
        case 0: return qsTr("COLOUR")
        case 1: return qsTr("MOTION")
        case 2: return qsTr("POSITION")
        case 3: return qsTr("FLASH")
        case 4: return qsTr("START")
        }
        return "–"
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
        return "#3A3A3A"
    }

    // every function with its role, fetched when the table really changes -
    // not on every keystroke in the search box (it marshals the whole project)
    property var allRows: []
    function reloadRows()
    {
        allRows = trackEngine ? trackEngine.table() : []
    }

    Component.onCompleted:
    {
        // guess anything not placed yet, so the page works on a project it
        // has never seen
        if (trackEngine)
            trackEngine.autoAssign(false)
        reloadRows()
    }

    Connections
    {
        target: trackEngine
        function onTableChanged()
        {
            // a rebuilt model puts the ListView back at the top, so remember
            // where the finger was
            var y = funcList.contentY
            setupRoot.reloadRows()
            Qt.callLater(function() { funcList.contentY = Math.min(y, Math.max(0, funcList.contentHeight - funcList.height)) })
        }
    }

    ColumnLayout
    {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // Nested layouts fill the height by default in Qt Quick Layouts, so
        // every row here pins itself and only the list below may grow.

        // ------------------------------------------------- header
        RowLayout
        {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 40
            Layout.maximumHeight: 40
            spacing: 8

            Text
            {
                visible: setupRoot.width >= 1640    // R437_SETUP_FITS
                text: qsTr("WHAT DOES EACH LOOK DO?")
                color: setupRoot.cText
                font.bold: true
                font.pixelSize: 15
            }

            Rectangle
            {
                Layout.preferredWidth: setupRoot.width >= 1640 ? 200 : 120
                Layout.preferredHeight: 34
                color: "#1B1B1B"
                radius: 3
                border.width: 1
                border.color: setupRoot.cLine

                TextInput
                {
                    id: filterInput
                    anchors.fill: parent
                    anchors.margins: 8
                    verticalAlignment: TextInput.AlignVCenter
                    color: setupRoot.cText
                    font.pixelSize: 14
                    clip: true
                    onTextChanged: setupRoot.filter = text.toLowerCase()
                }

                Text
                {
                    anchors.fill: parent
                    anchors.margins: 8
                    verticalAlignment: Text.AlignVCenter
                    visible: filterInput.text.length === 0
                    text: qsTr("search…")
                    color: "#666666"
                    font.pixelSize: 14
                }
            }

            Item { Layout.fillWidth: true }

            TrackTile
            {
                Layout.preferredWidth: 100
                Layout.preferredHeight: 34
                label: qsTr("SHOW ALL")
                active: trackEngine ? trackEngine.showAll : false
                onTapped: if (trackEngine) trackEngine.showAll = !trackEngine.showAll
            }

            // Do the thumbs count? Off by default. The verdicts are recorded
            // either way - this only decides whether they steer what gets
            // picked. Off is the honest starting point: nobody has looked at
            // the numbers yet, and a night should not go quiet because of a
            // score that has three votes behind it.
            TrackTile
            {
                Layout.preferredWidth: 110
                Layout.preferredHeight: 34
                label: qsTr("RATINGS")
                active: trackEngine ? trackEngine.ratingEnabled : false
                activeColor: "#4FA36B"
                onTapped: if (trackEngine) trackEngine.ratingEnabled = !trackEngine.ratingEnabled
            }

            // the forcing variant: it re-guesses roles the operator set by
            // hand, and nothing here can put them back. So: two taps.
            TrackTile
            {
                id: reguessTile
                property bool armed: false
                Layout.preferredWidth: 100
                Layout.preferredHeight: 34
                label: armed ? qsTr("SURE?") : qsTr("RE-GUESS")
                active: armed
                activeColor: "#E3B44F"
                onTapped:
                {
                    if (armed === false) { armed = true; reguessArm.restart(); return }
                    armed = false
                    if (trackEngine) trackEngine.autoAssign(true)
                }
                Timer { id: reguessArm; interval: 4000; onTriggered: reguessTile.armed = false }
            }

            // the engine makes everything from the DMX channels; the user's
            // scenes step aside (animation lasers and laser positions excepted)
            TrackTile
            {
                Layout.preferredWidth: 110
                Layout.preferredHeight: 34
                id: fullAutoTile
                property bool armed: false
                label: armed ? qsTr("SURE?") : qsTr("FULL AUTO")
                active: armed || (trackEngine ? trackEngine.fullAuto : false)
                activeColor: "#E3B44F"
                onTapped:
                {
                    if (!trackEngine) return
                    // runde 296 (Tobias: "skal også have en bekræftelse inden den
                    // slår fra", B20 U5): ON is one tap, OFF asks SURE?
                    if (trackEngine.fullAuto && armed === false) { armed = true; fullAutoArm.restart(); return }
                    armed = false
                    trackEngine.fullAuto = !trackEngine.fullAuto
                }
                Timer { id: fullAutoArm; interval: 4000; onTriggered: fullAutoTile.armed = false }
            }

            // every enabled group in red, green, blue, white - two seconds
            // each, no track needed. Tap again to stop. What it shows is in
            // the report line, so a dark group can be named, not guessed.
            TrackTile
            {
                Layout.preferredWidth: 100
                Layout.preferredHeight: 34
                label: (trackEngine && trackEngine.testing) ? qsTr("STOP TEST") : qsTr("SELF TEST")
                active: trackEngine ? trackEngine.testing : false
                activeColor: "#4FB3C8"
                onTapped: if (trackEngine) trackEngine.selfTest()
            }

            // R436_LAB_MENU (Tobias 10-12: "lav laser-lab om til at hedde LAB"):
            // the tile opens the LAB menu - LASER LAB, STROBE LAB, more later.
            // A lab still opens with SHOW OFF only (the menu's OPEN says so).
            TrackTile
            {
                objectName: "labTile"
                Layout.preferredWidth: 80
                Layout.preferredHeight: 34
                visible: trackEngine ? true : false      // R467_MIC_CHECK: MIC CHECK needs no lamps
                label: qsTr("LAB")
                active: setupRoot.labMenuOpen
                activeColor: "#E3B44F"
                onTapped:
                {
                    setupRoot.labMenuOpen = !setupRoot.labMenuOpen
                    if (setupRoot.labMenuOpen) setupRoot.learnedOpen = false
                }
            }

            // R436_LEARNED: what the engine learned from the taps in RIG
            TrackTile
            {
                objectName: "learnedTile"
                Layout.preferredWidth: 100
                Layout.preferredHeight: 34
                label: qsTr("LEARNED")
                active: setupRoot.learnedOpen
                activeColor: "#E3B44F"
                onTapped:
                {
                    setupRoot.learnedOpen = !setupRoot.learnedOpen
                    if (setupRoot.learnedOpen) setupRoot.labMenuOpen = false
                }
            }

            TrackTile
            {
                Layout.preferredWidth: 120
                Layout.preferredHeight: 34
                label: setupRoot.advancedOpen ? qsTr("HIDE ADVANCED") : qsTr("ADVANCED")
                active: setupRoot.advancedOpen
                onTapped: setupRoot.advancedOpen = !setupRoot.advancedOpen
            }

            // BLT's analysis cache: which tracks are known, hand-corrected
            // or automatic, and a way to forget one (re-analyse next time)
            TrackTile
            {
                Layout.preferredWidth: 100
                Layout.preferredHeight: 34
                label: setupRoot.cacheOpen ? qsTr("HIDE CACHE") : qsTr("CACHE")
                active: setupRoot.cacheOpen
                activeColor: "#4FA3E3"
                onTapped:
                {
                    setupRoot.cacheOpen = !setupRoot.cacheOpen
                    if (setupRoot.cacheOpen && trackManager) trackManager.requestCache()
                }
            }

            // R462_FULLSCREEN (Tobias 10-15): the window in and out of full screen -
            // kiosk (-k) has no menu, so this is the way out without Alt+F4. Lit
            // while it is full screen. SETUP closes with the top bar's CLOSE.
            TrackTile
            {
                objectName: "fullscreenTile"
                Layout.preferredWidth: 120
                Layout.preferredHeight: 34
                readonly property bool on: Window.window ? Window.window.visibility === Window.FullScreen : false
                label: qsTr("FULLSCREEN")
                active: on
                activeColor: "#4FB3C8"
                onTapped: if (typeof qlcplus !== "undefined" && qlcplus) qlcplus.toggleFullscreen()
            }
        }

        // ------------------------------------------------- R436_LEARNED (fold-out)
        Rectangle
        {
            id: learnedPanel
            objectName: "learnedPanel"
            Layout.fillWidth: true
            Layout.fillHeight: false
            readonly property var rows: trackEngine ? trackEngine.tasteTable : []
            readonly property real wantH: 170 + rows.length * 54
            Layout.preferredHeight: visible ? wantH : 0
            Layout.maximumHeight: visible ? wantH : 0
            visible: setupRoot.learnedOpen
            color: "#1E1E21"
            radius: 6
            readonly property var kinds: [ "break", "build", "normal", "drop" ]
            readonly property real nameW: 250
            readonly property real cellW: Math.max(120, Math.min(300, (width - nameW - 40 - 3 * 16) / 4))

            Text { x: 20; y: 14; text: qsTr("LEARNED FROM YOUR TAPS IN RIG"); color: setupRoot.cText; font.bold: true; font.pixelSize: 16 }
            Text
            {
                x: 20; y: 38; width: parent.width - 40; elide: Text.ElideRight
                text: qsTr("Each tap on a name in RIG teaches the engine what that kind of section should hold. It glides 10 % back to neutral every night.")
                color: setupRoot.cDim; font.pixelSize: 13
            }
            Repeater
            {
                model: learnedPanel.kinds
                Text
                {
                    x: 20 + learnedPanel.nameW + index * (learnedPanel.cellW + 16); y: 66
                    width: learnedPanel.cellW; horizontalAlignment: Text.AlignHCenter
                    text: modelData.toUpperCase(); color: setupRoot.cDim; font.bold: true; font.pixelSize: 13
                }
            }
            Repeater
            {
                model: learnedPanel.rows
                Item
                {
                    id: lrow
                    x: 20; y: 92 + index * 54
                    width: learnedPanel.width - 40; height: 46
                    readonly property var md: modelData ? modelData : ({ key: "", base: false, cells: [] })
                    Text
                    {
                        width: learnedPanel.nameW - 12; y: lrow.md.base ? 4 : 12; elide: Text.ElideRight
                        text: lrow.md.key; color: lrow.md.base ? "#4FA3E3" : setupRoot.cText; font.bold: true; font.pixelSize: 15
                    }
                    Text { visible: lrow.md.base === true; y: 26; text: qsTr("the base is never learned"); color: "#6A6A74"; font.pixelSize: 11 }
                    Repeater
                    {
                        model: 4
                        Rectangle
                        {
                            id: cell
                            objectName: "taste:" + learnedPanel.kinds[index] + ":" + lrow.md.key
                            readonly property real v: lrow.md.cells && lrow.md.cells.length > index ? lrow.md.cells[index] : 1.0
                            // 0 more often, 1 less often, 2 left out, 3 the base, 4 neutral
                            readonly property int kind: lrow.md.base === true ? 3 : (v < 0.60 ? 2 : (v < 0.995 ? 1 : (v > 1.005 ? 0 : 4)))
                            readonly property string label: cell.kind === 0 ? "▲  " + qsTr("more often") + "  " + cell.v.toFixed(2)
                                                          : cell.kind === 1 ? "▼  " + qsTr("less often") + "  " + cell.v.toFixed(2)
                                                          : cell.kind === 2 ? "✕  " + qsTr("LEFT OUT") + "  " + cell.v.toFixed(2)
                                                          : cell.kind === 3 ? qsTr("BASE") : "—  " + qsTr("neutral")
                            x: learnedPanel.nameW + index * (learnedPanel.cellW + 16)
                            width: learnedPanel.cellW; height: 46; radius: 6
                            color: cell.kind === 0 ? "#18341F" : cell.kind === 1 ? "#2E2416" : cell.kind === 2 ? "#3A1416"
                                 : cell.kind === 3 ? "#1E1E21" : "#28282E"
                            border.width: cell.kind === 3 ? 1 : 0
                            border.color: "#3A3A42"
                            Rectangle
                            {
                                visible: cell.kind < 3
                                anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                                height: 4
                                color: cell.kind === 0 ? "#6ECD82" : cell.kind === 1 ? "#E3B44F" : "#E03030"
                            }
                            Text
                            {
                                anchors.centerIn: parent
                                width: parent.width - 10; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight
                                text: cell.label
                                color: cell.kind === 0 ? "#6ECD82" : cell.kind === 1 ? "#E3B44F" : cell.kind === 2 ? "#F05050"
                                     : cell.kind === 3 ? "#45698C" : "#6A6A74"
                                font.bold: cell.kind !== 4; font.pixelSize: 14
                            }
                            MouseArea
                            {
                                anchors.fill: parent
                                enabled: cell.kind < 3
                                onClicked: if (trackEngine) trackEngine.resetSectionTaste(learnedPanel.kinds[index], lrow.md.key)
                            }
                        }
                    }
                }
            }
            Text
            {
                x: 20; y: learnedPanel.height - 62; width: parent.width - 280; elide: Text.ElideRight
                text: qsTr("Tap a box = back to neutral (that box only).   ▲ above 1.0 = drawn more often.   ▼ under 1.0 = less often.   ✕ under 0.60 = left out (a wash out of breaks, the strobes not in front).")
                color: setupRoot.cDim; font.pixelSize: 12
            }
            Text
            {
                x: 20; y: learnedPanel.height - 38; width: parent.width - 280; elide: Text.ElideRight
                text: qsTr("Learns only with RATINGS on (now %1).").arg(trackEngine && trackEngine.ratingEnabled ? qsTr("ON") : qsTr("OFF"))
                      + "   " + ((trackEngine && trackEngine.tasteNight !== "") ? qsTr("Last glided back: %1.").arg(trackEngine.tasteNight) : "")
                      + "   " + qsTr("Your tap in RIG always wins over what it learned.")
                color: setupRoot.cDim; font.pixelSize: 12
            }
            TrackTile
            {
                id: tasteResetTile
                objectName: "tasteResetAll"
                property bool armed: false
                x: parent.width - 240; y: parent.height - 56; width: 220; height: 40
                label: armed ? qsTr("SURE? TAP AGAIN") : qsTr("RESET ALL")
                active: armed
                activeColor: "#E36B6B"
                onTapped:
                {
                    if (armed === false) { armed = true; tasteResetArm.restart(); return }
                    armed = false
                    if (trackEngine) trackEngine.resetSectionTaste("", "")
                }
                Timer { id: tasteResetArm; interval: 4000; onTriggered: tasteResetTile.armed = false }
            }
        }

        // ------------------------------------------------- R436_LAB_MENU (fold-out)
        Rectangle
        {
            id: labMenu
            objectName: "labMenu"
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: visible ? 470 : 0
            Layout.maximumHeight: visible ? 470 : 0
            visible: setupRoot.labMenuOpen
            color: "#1E1E21"
            radius: 6
            readonly property bool showOn: trackManager ? trackManager.autoRun : false
            readonly property real cw: Math.max(200, Math.min(340, (width - 40 - 3 * 24) / 4))   // R470_SYNC: four a row
            // R440_BARS_LAB: each card from its own summary (strobes [0], bars [1])
            readonly property var sums: trackEngine ? trackEngine.slabSummaries : []
            function sum(kind, field, dflt) { return (sums.length > kind && sums[kind][field] !== undefined) ? sums[kind][field] : dflt }

            Text { x: 20; y: 14; text: "LAB"; color: "#E3B44F"; font.bold: true; font.pixelSize: 18 }
            Text
            {
                x: 70; y: 17; width: parent.width - 90; elide: Text.ElideRight
                text: qsTr("one lamp type at a time - see every look on the rig, keep it or not.  SHOW OFF only (MIC CHECK, SYNC: any time).")
                color: setupRoot.cDim; font.pixelSize: 13
            }
            Grid
            {
                x: 20; y: 52
                columns: 4
                spacing: 24
                LabCard
                {
                    title: qsTr("LASER LAB")
                    width: labMenu.cw; showOn: labMenu.showOn
                    sub: qsTr("the animation laser")
                    can: trackEngine ? trackEngine.labAvailable : false
                    line1: qsTr("%1 approved looks").arg(trackEngine ? trackEngine.labApprovedCount : 0)
                    line2: qsTr("height held at yours · no tilt toward the room")
                    openName: "labOpen:laser"
                    onOpen: if (trackEngine) trackEngine.labOpen()
                }
                LabCard
                {
                    title: qsTr("STROBE LAB")
                    width: labMenu.cw; showOn: labMenu.showOn
                    sub: labMenu.sum(0, "group", "") !== ""
                         ? labMenu.sum(0, "group", "") + "  ·  " + labMenu.sum(0, "lamps", 0) + " " + qsTr("lamps") : qsTr("the strobes")
                    can: labMenu.sum(0, "available", false)
                    line1: qsTr("%1 calm break  ·  %2 groove  ·  %3 drop").arg(labMenu.sum(0, "counts", [0, 0, 0])[0]).arg(labMenu.sum(0, "counts", [0, 0, 0])[1]).arg(labMenu.sum(0, "counts", [0, 0, 0])[2])
                    line2: qsTr("✕ %1 banned  ·  ✓ %2 kept").arg(labMenu.sum(0, "banned", 0)).arg(labMenu.sum(0, "good", 0))
                    openName: "labOpen:strobe"
                    onOpen: if (trackEngine) trackEngine.slabOpenKind(0)
                }
                // R440_BARS_LAB
                LabCard
                {
                    title: qsTr("BARS LAB")
                    width: labMenu.cw; showOn: labMenu.showOn
                    sub: labMenu.sum(1, "group", "") !== ""
                         ? labMenu.sum(1, "group", "") + "  ·  " + labMenu.sum(1, "lamps", 0) + " " + qsTr("bars") : qsTr("the laser bars")
                    can: labMenu.sum(1, "available", false)
                    line1: qsTr("%1 break  ·  %2 groove  ·  %3 drop").arg(labMenu.sum(1, "counts", [0, 0, 0])[0]).arg(labMenu.sum(1, "counts", [0, 0, 0])[1]).arg(labMenu.sum(1, "counts", [0, 0, 0])[2])
                    line2: qsTr("✕ %1 banned  ·  ✓ %2 kept").arg(labMenu.sum(1, "banned", 0)).arg(labMenu.sum(1, "good", 0))
                    openName: "labOpen:bars"
                    onOpen: if (trackEngine) trackEngine.slabOpenKind(1)
                }
                // R440_COLOUR_LAB
                LabCard
                {
                    title: qsTr("COLOUR LAB")
                    width: labMenu.cw; showOn: labMenu.showOn
                    sub: qsTr("every lamp type  ·  one colour at a time")
                    can: trackEngine ? trackEngine.clabColours.length > 0 : false
                    line1: qsTr("how bright each group is IN each colour")
                    line2: trackEngine && trackEngine.clabChanges > 0 ? qsTr("%1 levels changed from the show file").arg(trackEngine.clabChanges)
                                                                     : qsTr("every level as the show file made it")
                    openName: "labOpen:colour"
                    onOpen: if (trackEngine) trackEngine.clabOpen()
                }
                // R467_MIC_CHECK: only listens, so SHOW ON is fine
                LabCard
                {
                    title: qsTr("MIC CHECK")
                    width: labMenu.cw; showOn: false
                    sub: qsTr("this PC's microphone  ·  SHOW ON is fine")
                    can: true
                    line1: qsTr("the level, 16 bands, the room's shape against the track")
                    line2: trackEngine && trackEngine.micRecs.length > 0 ? qsTr("%1 recordings tonight").arg(trackEngine.micRecs.length)
                                                                        : qsTr("only listens - nothing on the rig changes")
                    openName: "labOpen:mic"
                    onOpen: if (trackEngine) trackEngine.micOpen()
                }
                // R470_SYNC: the light against the sound - live, the show on is fine
                LabCard
                {
                    title: qsTr("SYNC")
                    width: labMenu.cw; showOn: false
                    sub: qsTr("is the light on time with the sound?  ·  SHOW ON is fine")
                    can: true
                    line1: qsTr("two rounds of 16 taps with the music playing")
                    line2: trackEngine && trackEngine.syncOffset !== 0 ? qsTr("the light is moved %1 ms").arg((trackEngine.syncOffset > 0 ? "+" : "") + trackEngine.syncOffset)
                                                                     : qsTr("the light runs as it always did (0 ms)")
                    openName: "labOpen:sync"
                    onOpen: if (trackEngine) trackEngine.syncOpen()
                }
                LabCard { width: labMenu.cw; title: qsTr("WASH LAB"); sub: qsTr("the moving heads"); live: false }
            }
        }

        // ------------------------------------------------- cache (fold-out)
        Rectangle
        {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: visible ? 260 : 0
            Layout.maximumHeight: 260
            visible: setupRoot.cacheOpen
            color: "#181818"
            radius: 4
            border.width: 1
            border.color: "#333333"

            ColumnLayout
            {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 4

                RowLayout
                {
                    Layout.fillWidth: true
                    Layout.fillHeight: false
                    Layout.preferredHeight: 30
                    Layout.maximumHeight: 30
                    spacing: 8

                    Text
                    {
                        Layout.fillWidth: true
                        text: !trackManager ? ""
                              : (trackManager.connected === false
                                 ? qsTr("Beat Link Trigger is not connected - the cache lives there")
                                 : qsTr("%1 tracks in BLT's cache - MANUAL ones are your corrections and never re-analysed").arg(trackManager.cacheList.length))
                        color: setupRoot.cDim
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    TrackTile
                    {
                        Layout.preferredWidth: 90
                        Layout.preferredHeight: 28
                        label: qsTr("REFRESH")
                        onTapped: if (trackManager) trackManager.requestCache()
                    }
                    // two taps, like RE-GUESS: one throws away every analysis
                    // of the night, rekordbox' too (runde 209)
                    TrackTile
                    {
                        id: forgetAutoTile
                        property bool armed: false
                        Layout.preferredWidth: 190
                        Layout.preferredHeight: 28
                        label: armed ? qsTr("SURE?") : qsTr("FORGET ALL AUTOMATIC")
                        active: armed
                        activeColor: "#E36B6B"
                        onTapped:
                        {
                            if (armed === false) { armed = true; forgetAutoArm.restart(); return }
                            armed = false
                            if (trackManager) trackManager.forgetAutomatic()
                        }
                        Timer { id: forgetAutoArm; interval: 4000; onTriggered: forgetAutoTile.armed = false }
                    }
                }

                ListView
                {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    boundsBehavior: Flickable.StopAtBounds
                    model: trackManager ? trackManager.cacheList : []

                    // A date line above the first row of each day. cacheList is
                    // a plain list, not a model with roles, so ListView's own
                    // `section` cannot see into it - the delegate compares its
                    // own day with the row above instead. BLT sends the list
                    // newest first, so the days come out in order by themselves.
                    delegate: Column
                    {
                        width: ListView.view.width
                        // a model reset re-evaluates the bindings below while
                        // the row is already gone; without a fallback that is a
                        // TypeError per binding per row, on every reset.
                        property var md: modelData ? modelData
                                                   : ({ title: "", flags: 0, manual: false, playedAt: 0 })

                        function dayOf(ms)
                        {
                            if (!ms || ms <= 0) return ""
                            return Qt.formatDate(new Date(ms), "yyyy-MM-dd")
                        }
                        property string thisDay: dayOf(md.playedAt)
                        // "\u0000" and not "": an entry with no play time has
                        // an empty day, and the first of THOSE needs its heading
                        // too - comparing against "" would swallow it.
                        property string prevDay:
                            (index > 0 && trackManager && trackManager.cacheList[index - 1])
                                ? dayOf(trackManager.cacheList[index - 1].playedAt)
                                : "\u0000"

                        Text
                        {
                            width: parent.width
                            visible: thisDay !== prevDay
                            height: visible ? 24 : 0
                            verticalAlignment: Text.AlignBottom
                            leftPadding: 4
                            text:
                            {
                                if (thisDay === "")
                                    return qsTr("earlier - no play time recorded")
                                var today = Qt.formatDate(new Date(), "yyyy-MM-dd")
                                var y = new Date(); y.setDate(y.getDate() - 1)
                                if (thisDay === today) return qsTr("today")
                                if (thisDay === Qt.formatDate(y, "yyyy-MM-dd")) return qsTr("yesterday")
                                return thisDay
                            }
                            color: "#E3B44F"
                            font.bold: true
                            font.pixelSize: 11
                        }

                        Rectangle
                        {
                        width: parent.width
                        height: 42
                        color: "#1F1F1F"
                        radius: 3

                        RowLayout
                        {
                            anchors.fill: parent
                            anchors.margins: 3
                            spacing: 8

                            Text
                            {
                                Layout.fillWidth: true
                                text: md.title
                                color: setupRoot.cText
                                font.pixelSize: 13
                                elide: Text.ElideRight
                            }
                            Text
                            {
                                Layout.preferredWidth: 70
                                text: md.flags + " " + qsTr("flags")
                                color: setupRoot.cDim
                                font.pixelSize: 12
                            }
                            Rectangle
                            {
                                Layout.preferredWidth: 72
                                Layout.preferredHeight: 22
                                radius: 11
                                color: md.manual ? "#E3B44F" : "#3A3A3A"
                                Text
                                {
                                    anchors.centerIn: parent
                                    text: md.manual ? qsTr("MANUAL") : qsTr("AUTO")
                                    color: md.manual ? "#101010" : "#AAAAAA"
                                    font.bold: true
                                    font.pixelSize: 10
                                }
                            }
                            // two taps, and a target a finger can hit: this
                            // throws away the flags for that track for good,
                            // and it sits in a list the operator flicks
                            TrackTile
                            {
                                id: forgetTile
                                property bool armed: false
                                Layout.preferredWidth: 88
                                Layout.preferredHeight: 34
                                label: armed ? qsTr("SURE?") : qsTr("FORGET")
                                active: armed
                                activeColor: "#E36B6B"
                                onTapped:
                                {
                                    if (armed === false) { armed = true; forgetArm.restart(); return }
                                    armed = false
                                    if (trackManager) trackManager.forgetTrack(md.title)
                                }
                                Timer { id: forgetArm; interval: 4000; onTriggered: forgetTile.armed = false }
                            }
                        }
                        }
                    }
                }
            }
        }

        // ------------------------------------------------- groups
        RowLayout
        {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 56
            Layout.maximumHeight: 56
            spacing: 6

            Text
            {
                Layout.preferredWidth: 96
                Layout.minimumWidth: 96
                text: qsTr("GROUPS") + "\n" + qsTr("tap: on / off · hold: base")   // runde 234 changed the tap (fixed runde 248)
                lineHeight: 0.9
                color: setupRoot.cDim
                font.bold: true
                font.pixelSize: 12
            }

            Repeater
            {
                // the property's own NOTIFY is tableChanged: no extra trigger
                model: trackEngine ? trackEngine.groups : []

                Rectangle
                {
                    id: groupTile
                    // the model is rebuilt on every tableChanged, and a
                    // delegate can evaluate its bindings while modelData is
                    // already gone: read it once, through a stand-in
                    property var g: modelData ? modelData
                                  : ({ key: "", base: false, enabled: false,
                                       fixtures: 0, colours: 0, motions: 0, dimmer: false })
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 3
                    color: g.base ? "#2A3F55" : (g.enabled ? "#333333" : "#1F1F1F")
                    border.width: g.base ? 2 : 1
                    border.color: g.base ? "#4FA3E3" : (g.enabled ? "#666666" : "#333333")

                    Column
                    {
                        anchors.centerIn: parent
                        spacing: 1

                        Text
                        {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: groupTile.g.key
                                  + (groupTile.g.base ? "  · BASE" : (groupTile.g.enabled ? "" : "  · OFF"))
                            color: groupTile.g.enabled ? setupRoot.cText : "#666666"
                            font.bold: true
                            font.pixelSize: 12
                        }
                        Text
                        {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: groupTile.g.fixtures + " fx · "
                                  + groupTile.g.colours + " col · "
                                  + groupTile.g.motions + " mot · "
                                  + (groupTile.g.dimmer ? qsTr("dimmer") : qsTr("NO DIMMER"))
                            color: groupTile.g.dimmer ? setupRoot.cDim : "#E36B6B"
                            font.pixelSize: 10
                        }
                    }

                    // tap: ON <-> OFF. Press and hold: BASE (or back to the
                    // automatic base) - one tap made the Minis the base by
                    // accident (runde 234). The base group is always lit; the
                    // others are effects added on top as the evening rises.
                    MouseArea
                    {
                        anchors.fill: parent
                        onClicked: if (trackEngine && groupTile.g.key) trackEngine.cycleGroup(groupTile.g.key)
                        onPressAndHold: if (trackEngine && groupTile.g.key) trackEngine.toggleBase(groupTile.g.key)
                    }
                }
            }
        }

        // ------------------------------------------------- role legend
        RowLayout
        {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 28
            Layout.maximumHeight: 28
            spacing: 4

            Item { Layout.fillWidth: true }

            Repeater
            {
                model: trackEngine ? trackEngine.roleCount : 0

                Rectangle
                {
                    Layout.preferredWidth: 110
                    Layout.minimumWidth: 110
                    Layout.maximumWidth: 110
                    Layout.fillHeight: true
                    color: "#1B1B1B"
                    radius: 3
                    border.width: 1
                    border.color: setupRoot.roleColor(index)

                    Text
                    {
                        anchors.fill: parent
                        anchors.margins: 3
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        text: trackEngine ? trackEngine.roleHint(index) : ""
                        color: setupRoot.cDim
                        font.pixelSize: 9
                    }
                }
            }

            Rectangle
            {
                Layout.preferredWidth: 100
                Layout.minimumWidth: 100
                Layout.maximumWidth: 100
                Layout.fillHeight: true
                color: "#1B1B1B"
                radius: 3
                border.width: 1
                border.color: "#E3B44F"

                Text
                {
                    anchors.fill: parent
                    anchors.margins: 3
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    text: qsTr("energy: 1 any, 2 groove, 3 drop only")
                    color: setupRoot.cDim
                    font.pixelSize: 9
                }
            }

            Item { Layout.preferredWidth: 60; Layout.minimumWidth: 60 }
        }

        // ------------------------------------------------- function rows
        ListView
        {
            id: funcList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: 300
            Layout.minimumHeight: 120
            clip: true
            spacing: 3
            boundsBehavior: Flickable.StopAtBounds

            model:
            {
                var all = setupRoot.allRows
                if (setupRoot.filter === "")
                    return all

                var out = []
                for (var i = 0; i < all.length; i++)
                {
                    var row = all[i]
                    if (row.name.toLowerCase().indexOf(setupRoot.filter) >= 0
                        || row.path.toLowerCase().indexOf(setupRoot.filter) >= 0
                        || row.group.toLowerCase().indexOf(setupRoot.filter) >= 0
                        || row.colour.indexOf(setupRoot.filter) >= 0)
                        out.push(row)
                }
                return out
            }

            delegate: Rectangle
            {
                id: funcRow
                width: funcList.width
                height: 44
                color: (modelData && modelData.hidden) ? "#181818" : "#1F1F1F"
                radius: 3

                // a model reset can re-evaluate these while the row is gone
                property int rowRole: modelData ? modelData.role : -1
                property int rowStars: modelData ? modelData.stars : 0
                property var rowId: modelData ? modelData.id : 0
                property var md: modelData ? modelData
                                           : ({ hidden: false, role: -1, stars: 0, id: 0,
                                                colour: 0, name: "", group: "", path: "",
                                                banned: false, rateUp: 0, rateDown: 0,
                                                generated: false })

                RowLayout
                {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 4

                    Rectangle
                    {
                        Layout.preferredWidth: 10
                        Layout.fillHeight: true
                        radius: 2
                        color: setupRoot.swatch(md.colour)
                        visible: true
                    }

                    ColumnLayout
                    {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 0

                        Text
                        {
                            Layout.fillWidth: true
                            text: md.name
                            color: md.hidden ? "#888888" : setupRoot.cText
                            font.pixelSize: 14
                            elide: Text.ElideRight
                        }
                        Text
                        {
                            Layout.fillWidth: true
                            text: md.group
                                  + (md.path.length > 0 ? "   ·   " + md.path : "")
                            color: "#6A6A6A"
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                    }

                    Repeater
                    {
                        model: trackEngine ? trackEngine.roleCount : 0

                        // fixed width: a finger-sized target whatever the name column does
                        TrackTile
                        {
                            Layout.preferredWidth: 110
                            Layout.minimumWidth: 110
                            Layout.maximumWidth: 110
                            Layout.fillHeight: true
                            label: setupRoot.roleShort(index)
                            active: funcRow.rowRole === index
                            activeColor: setupRoot.roleColor(index)
                            onTapped: if (trackEngine) trackEngine.assignRole(funcRow.rowId, index)
                        }
                    }

                    // energy stars: when a motion may run. Guessed from its
                    // tempo and name; tap to overrule.
                    Row
                    {
                        Layout.preferredWidth: 100
                        Layout.minimumWidth: 100
                        Layout.maximumWidth: 100
                        Layout.fillHeight: true
                        spacing: 2
                        visible: funcRow.rowRole === 1

                        Repeater
                        {
                            model: 3

                            TrackTile
                            {
                                width: 32
                                height: parent.height
                                label: "★"
                                active: index < funcRow.rowStars
                                activeColor: "#E3B44F"
                                onTapped: if (trackEngine) trackEngine.setStars(funcRow.rowId, index + 1)
                            }
                        }
                    }

                    // What the thumbs have said about this one. Blank until
                    // there is anything, so 700 rows do not all shout "0 / 0".
                    Text
                    {
                        Layout.preferredWidth: 54
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        Layout.fillHeight: true
                        visible: (md.rateUp + md.rateDown) > 0
                        text: (md.rateUp > 0 ? "+" + md.rateUp : "")
                              + (md.rateDown > 0 ? " -" + md.rateDown : "")
                        color: md.rateUp >= md.rateDown ? "#7ED07E" : "#E36B6B"
                        font.pixelSize: 12
                    }

                    // Never again. Two taps, like RE-GUESS: it is not a thing
                    // to hit by accident on the way past, and unlike a role it
                    // survives every re-guess there is.
                    TrackTile
                    {
                        id: banTile
                        property bool armed: false
                        property bool isBanned: md.banned === true
                        Layout.preferredWidth: 66
                        Layout.fillHeight: true
                        // not on the engine's own scenes: it would leave the
                        // group without a colour, and setBanned() refuses
                        // anyway - a button that does nothing is worse than
                        // no button
                        visible: funcRow.rowRole >= 0 && md.generated !== true
                        label: armed ? qsTr("SURE?") : (isBanned ? qsTr("BANNED") : qsTr("BAN"))
                        active: armed || isBanned
                        activeColor: armed ? "#E3B44F" : "#B03030"
                        opacity: isBanned ? 1.0 : 0.55
                        onTapped:
                        {
                            if (trackEngine === null) return
                            // un-banning is harmless, so it needs no arming
                            if (isBanned) { trackEngine.setBanned(funcRow.rowId, false); return }
                            if (armed === false) { armed = true; banArm.restart(); return }
                            armed = false
                            trackEngine.setBanned(funcRow.rowId, true)
                        }
                        Timer { id: banArm; interval: 4000; onTriggered: banTile.armed = false }
                    }

                    Item
                    {
                        Layout.preferredWidth: 100
                        Layout.minimumWidth: 100
                        Layout.maximumWidth: 100
                        visible: funcRow.rowRole !== 1
                    }

                    TrackTile
                    {
                        Layout.preferredWidth: 52
                        Layout.minimumWidth: 52
                        Layout.maximumWidth: 52
                        Layout.fillHeight: true
                        label: "–"
                        active: funcRow.rowRole < 0
                        onTapped: if (trackEngine) trackEngine.assignRole(funcRow.rowId, -1)
                    }
                }
            }
        }

        // ------------------------------------------------- advanced
        RowLayout
        {
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 40
            Layout.maximumHeight: 40
            visible: setupRoot.advancedOpen
            spacing: 8

            Text
            {
                text: qsTr("Colour holds for")
                color: setupRoot.cDim
                font.pixelSize: 13
            }

            // runde 189: by default the ENERGY fader picks the hold - gliding
            // 64 -> 8 bars since runde 291. A bars tile fixes it (the override
            // Tobias asked for); tap the lit tile again (or ENERGY) to give it
            // back to the fader.
            TrackTile
            {
                Layout.preferredWidth: 96
                Layout.preferredHeight: 34
                label: qsTr("ENERGY")
                active: trackEngine ? trackEngine.holdAuto : false
                activeColor: "#4FA3E3"
                onTapped: if (trackEngine) trackEngine.holdAuto = true
            }

            Repeater
            {
                // 8 added 2026-09-15 on Tobias' ask: "lav en ny knap saa vi
                // kan skifte sceneskift til hvert 8 bar ogsaa". setHoldBars()
                // already bounds to 4..128, so the value itself needed nothing.
                model: [ 8, 16, 32, 64 ]
                TrackTile
                {
                    Layout.preferredWidth: 80
                    Layout.preferredHeight: 34
                    label: modelData + qsTr(" bars")
                    active: trackEngine ? (trackEngine.holdAuto === false && trackEngine.holdBars === modelData) : false
                    activeColor: "#4FA3E3"
                    onTapped: {
                        if (!trackEngine)
                            return
                        if (active)
                            trackEngine.holdAuto = true
                        else
                            trackEngine.holdBars = modelData
                    }
                }
            }

            // (runde 215: the row's spacers went and its tiles narrowed - it was
            // 1478 px wanted in 1264 at 1280x720, the EXPORT/IMPORT reply got no
            // width at all and "Colour holds for" drew over the ENERGY tile)
            TrackTile
            {
                Layout.preferredWidth: 170
                Layout.preferredHeight: 34
                label: qsTr("Accent colour in drops")
                active: trackEngine ? trackEngine.accent : false
                activeColor: "#7ED07E"
                onTapped: if (trackEngine) trackEngine.accent = !trackEngine.accent
            }

            // the clock creeps the ENERGY slider up through the night; a
            // hand on the slider turns it off, this turns it back on
            TrackTile
            {
                Layout.preferredWidth: 130
                Layout.preferredHeight: 34
                label: qsTr("ENERGY by clock")
                active: trackEngine ? trackEngine.roomAuto : false
                activeColor: "#7ED07E"
                onTapped: if (trackEngine) trackEngine.roomAuto = !trackEngine.roomAuto
            }

            // the closing sequence (runde 211): the last five minutes to
            // closing bring the room to dark and the hazer off. Off until
            // switched on since R458_CLOSING_OFF - LAST TRACK is the DJ's end
            TrackTile
            {
                Layout.preferredWidth: 130
                Layout.preferredHeight: 34
                label: qsTr("Closing sequence")
                active: trackEngine ? trackEngine.closingSequence : false
                activeColor: "#7ED07E"
                onTapped: if (trackEngine) trackEngine.closingSequence = !trackEngine.closingSequence
            }

            // the clock curve's points are in their own rows below (runde 211)

            // R440_SEND: everything from tonight in one zip, for Claude
            TrackTile
            {
                objectName: "sendOpen"
                Layout.preferredWidth: 150
                Layout.preferredHeight: 34
                label: qsTr("SEND TO CLAUDE")
                active: true
                activeColor: "#4FA3E3"
                onTapped: setupRoot.openSend()
            }

            // every Track setting to / from Documents/QLC+/track-settings.json
            // two taps each (runde 209): IMPORT replaces every role, star
            // and ban with the file's, and EXPORT overwrites the only backup
            TrackTile
            {
                id: exportTile
                property bool armed: false
                Layout.preferredWidth: 84
                Layout.preferredHeight: 34
                label: armed ? qsTr("SURE?") : qsTr("EXPORT")
                active: armed
                activeColor: "#E3B44F"
                onTapped:
                {
                    if (armed === false) { armed = true; exportArm.restart(); return }
                    armed = false
                    if (trackEngine) setupRoot.ioMessage = trackEngine.exportSettings()
                }
                Timer { id: exportArm; interval: 4000; onTriggered: exportTile.armed = false }
            }
            TrackTile
            {
                id: importTile
                property bool armed: false
                Layout.preferredWidth: 84
                Layout.preferredHeight: 34
                label: armed ? qsTr("SURE?") : qsTr("IMPORT")
                active: armed
                activeColor: "#E3B44F"
                onTapped:
                {
                    if (armed === false) { armed = true; importArm.restart(); return }
                    armed = false
                    if (trackEngine) setupRoot.ioMessage = trackEngine.importSettings()
                }
                Timer { id: importArm; interval: 4000; onTriggered: importTile.armed = false }
            }

            Text
            {
                Layout.fillWidth: true
                text: setupRoot.ioMessage
                color: setupRoot.cDim
                font.pixelSize: 11
                elide: Text.ElideLeft
            }
        }

        // ------------------------------------------------- ENERGY by clock
        // Runde 211 (Tobias, 2026-09-24): a point every fifteen minutes from
        // 20:00 to 02:45 - two rows of fourteen, the evening and the night.
        // A tap steps up ten percent, past a hundred it comes round to nought.
        GridLayout
        {
            Layout.fillWidth: true
            Layout.fillHeight: false
            visible: setupRoot.advancedOpen
            columns: 14
            columnSpacing: 4
            rowSpacing: 4

            Repeater
            {
                model: trackEngine ? trackEngine.clockCurve : []
                TrackTile
                {
                    Layout.preferredWidth: 60
                    Layout.preferredHeight: 34
                    property int at: 20 * 60 + 15 * index
                    label: ("0" + (Math.floor(at / 60) % 24)).slice(-2) + ":" + ("0" + (at % 60)).slice(-2)
                           + "\n" + modelData + "%"
                    active: trackEngine ? trackEngine.roomAuto : false
                    activeColor: "#4FA3E3"
                    onTapped: if (trackEngine) trackEngine.cycleClockPoint(index)
                }
            }
        }

        // ------------------------------------------------- R470_CLOCK_HAND
        // where HIS hand put ENERGY at each quarter hour, the last nights - only a
        // proposal: a gold tile differs 10 % or more; a tap takes that one point,
        // USE YOURS takes them all. The clock never changes by itself.
        ColumnLayout
        {
            objectName: "clockHand"
            Layout.fillWidth: true
            Layout.fillHeight: false
            visible: setupRoot.advancedOpen && trackEngine && trackEngine.clockHandNights.length > 0
            spacing: 6
            RowLayout
            {
                Layout.fillWidth: true
                spacing: 10
                Text { text: qsTr("YOUR HAND"); color: "#E3B44F"; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.4 }
                Text
                {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: trackEngine ? qsTr("where you put ENERGY by hand, %1 nights (%2) - gold = 10 % or more from the clock, tap one to take it")
                                          .arg(trackEngine.clockHandNights.length).arg(trackEngine.clockHandNights.join(", ")) : ""
                    color: setupRoot.cDim; font.pixelSize: 12
                }
                TrackTile
                {
                    objectName: "clockHandUse"
                    Layout.preferredWidth: 120; Layout.preferredHeight: 34
                    label: qsTr("USE YOURS"); active: true; activeColor: "#E3B44F"
                    onTapped: if (trackEngine) trackEngine.useClockHand(-1)
                }
            }
            GridLayout
            {
                Layout.fillWidth: true
                columns: 14; columnSpacing: 4; rowSpacing: 4
                Repeater
                {
                    model: trackEngine ? trackEngine.clockHand : []
                    TrackTile
                    {
                        objectName: "clockHand:" + index
                        Layout.preferredWidth: 60
                        Layout.preferredHeight: 34
                        property int at: 20 * 60 + 15 * index
                        readonly property int clock: trackEngine && trackEngine.clockCurve.length > index ? trackEngine.clockCurve[index] : 0
                        readonly property bool apart: modelData >= 0 && Math.abs(modelData - clock) >= 10
                        label: ("0" + (Math.floor(at / 60) % 24)).slice(-2) + ":" + ("0" + (at % 60)).slice(-2)
                               + "\n" + (modelData >= 0 ? modelData + "%" : "-")
                        active: apart
                        activeColor: "#E3B44F"
                        opacity: modelData >= 0 ? 1.0 : 0.4
                        onTapped: if (trackEngine && apart) trackEngine.useClockHand(index)
                    }
                }
            }
        }

        // ------------------------------------------------- running
        Text
        {
            Layout.fillWidth: true
            Layout.fillHeight: false
            text: qsTr("Running") + ":  " + (trackEngine ? trackEngine.report : "")
            color: setupRoot.cDim
            font.pixelSize: 13
            elide: Text.ElideRight
        }
    }

    // R410_LAB: the lab over the whole Track page (its root, handed over as
    // host), the page's own top bar included - CLOSE is on the lab
    Loader
    {
        id: labLoader
        objectName: "labLoader"
        parent: setupRoot.host ? setupRoot.host : setupRoot
        anchors.fill: parent
        z: 1000
        active: trackEngine ? trackEngine.labActive : false
        visible: active
        source: "qrc:/LaserLab.qml"
    }

    // R436_STROBE_LAB: the same, for STROBE LAB
    Loader
    {
        id: slabLoader
        objectName: "slabLoader"
        parent: setupRoot.host ? setupRoot.host : setupRoot
        anchors.fill: parent
        z: 1000
        active: trackEngine ? trackEngine.slabActive : false
        visible: active
        source: "qrc:/StrobeLab.qml"
    }

    // R440_COLOUR_LAB: the same, for COLOUR LAB
    Loader
    {
        id: clabLoader
        objectName: "clabLoader"
        parent: setupRoot.host ? setupRoot.host : setupRoot
        anchors.fill: parent
        z: 1000
        active: trackEngine ? trackEngine.clabActive : false
        visible: active
        source: "qrc:/ColourLab.qml"
    }

    // R467_MIC_CHECK: the same, for MIC CHECK
    Loader
    {
        id: micLoader
        objectName: "micLoader"
        parent: setupRoot.host ? setupRoot.host : setupRoot
        anchors.fill: parent
        z: 1000
        active: trackEngine ? trackEngine.micActive : false
        visible: active
        source: "qrc:/MicCheck.qml"
    }

    // R470_SYNC: SYNC over the whole page, as MIC CHECK
    Loader
    {
        id: syncLoader
        objectName: "syncLoader"
        parent: setupRoot.host ? setupRoot.host : setupRoot
        anchors.fill: parent
        z: 1000
        active: trackEngine ? trackEngine.syncActive : false
        visible: active
        source: "qrc:/SyncCheck.qml"
    }

    // R440_SEND: SEND TO CLAUDE - over the whole page, from ADVANCED
    property bool sendOpen: false
    property var sendList: []
    property var sendOn: ({})
    function openSend()
    {
        if (!trackEngine) return
        trackEngine.sendReset()
        var items = trackEngine.sendItems()
        var on = {}
        for (var i = 0; i < items.length; i++)
            on[items[i].id] = items[i].on === true
        sendList = items
        sendOn = on
        sendOpen = true
    }
    Rectangle
    {
        id: sendVeil
        objectName: "sendDialog"
        parent: setupRoot.host ? setupRoot.host : setupRoot
        anchors.fill: parent
        z: 1100
        visible: setupRoot.sendOpen
        color: "#B0000000"
        MouseArea { anchors.fill: parent }
        Rectangle
        {
            id: sendBox
            readonly property real ks: Math.min(1.0, Math.min(parent.width / 1180, parent.height / 900))
            width: 1100 * ks; height: 860 * ks
            anchors.centerIn: parent
            radius: 14 * ks; color: "#141418"; border.width: 3; border.color: "#4FA3E3"
            readonly property bool busy: trackEngine ? trackEngine.sendState === 1 : false
            readonly property bool done: trackEngine ? trackEngine.sendState >= 2 : false
            Rectangle
            {
                x: 40 * sendBox.ks; y: 36 * sendBox.ks; width: 260 * sendBox.ks; height: 50 * sendBox.ks; radius: 8 * sendBox.ks; color: "#4FA3E3"
                Text { anchors.centerIn: parent; text: qsTr("SEND TO CLAUDE"); color: "#0A1622"; font.bold: true; font.pixelSize: 18 * sendBox.ks }
            }
            Text
            {
                x: 320 * sendBox.ks; y: 44 * sendBox.ks; width: sendBox.width - 360 * sendBox.ks; elide: Text.ElideRight
                text: qsTr("one zip with everything from tonight"); color: "#E9E9EE"; font.bold: true; font.pixelSize: 24 * sendBox.ks
            }
            Text
            {
                x: 40 * sendBox.ks; y: 104 * sendBox.ks; width: sendBox.width - 80 * sendBox.ks; elide: Text.ElideRight
                text: qsTr("Nothing leaves the PC - the zip lands in Documents\\QLC+, and you move it from there.")
                color: "#8C8C96"; font.pixelSize: 15 * sendBox.ks
            }
            Column
            {
                x: 40 * sendBox.ks; y: 146 * sendBox.ks; spacing: 8 * sendBox.ks
                Repeater
                {
                    model: setupRoot.sendList
                    Rectangle
                    {
                        objectName: "sendItem:" + modelData.id
                        readonly property bool can: modelData.available === true
                        readonly property bool on: can && setupRoot.sendOn[modelData.id] === true
                        width: sendBox.width - 80 * sendBox.ks; height: 64 * sendBox.ks; radius: 8 * sendBox.ks
                        color: on ? "#22222A" : "#1A1A1E"
                        Rectangle
                        {
                            x: 18 * sendBox.ks; anchors.verticalCenter: parent.verticalCenter
                            width: 32 * sendBox.ks; height: width; radius: 4 * sendBox.ks
                            color: parent.on ? "#4FA3E3" : "transparent"; border.width: 2; border.color: parent.on ? "#4FA3E3" : "#5E5E68"
                            Text { anchors.centerIn: parent; visible: parent.parent.on; text: "✓"; color: "#0A1622"; font.bold: true; font.pixelSize: 18 * sendBox.ks }
                        }
                        Text
                        {
                            x: 70 * sendBox.ks; y: 8 * sendBox.ks; width: parent.width - 90 * sendBox.ks; elide: Text.ElideRight
                            text: modelData.title; color: parent.can ? (parent.on ? "#E9E9EE" : "#B8B8C4") : "#5E5E68"; font.bold: true; font.pixelSize: 17 * sendBox.ks
                        }
                        Text
                        {
                            x: 70 * sendBox.ks; y: 34 * sendBox.ks; width: parent.width - 90 * sendBox.ks; elide: Text.ElideRight
                            text: modelData.detail; color: "#8C8C96"; font.pixelSize: 13 * sendBox.ks
                        }
                        MouseArea
                        {
                            anchors.fill: parent
                            enabled: parent.can && !sendBox.busy
                            onClicked:
                            {
                                var on = Object.assign({}, setupRoot.sendOn)
                                on[modelData.id] = !(on[modelData.id] === true)
                                setupRoot.sendOn = on
                            }
                        }
                    }
                }
            }
            Text
            {
                objectName: "sendResult"
                x: 40 * sendBox.ks; y: sendBox.height - 160 * sendBox.ks; width: sendBox.width - 80 * sendBox.ks
                wrapMode: Text.WordWrap
                text: trackEngine && trackEngine.sendResult !== "" ? trackEngine.sendResult
                      : qsTr("Saved as Documents\\QLC+\\to-claude-<date-time>.zip")
                color: trackEngine && trackEngine.sendState === 3 ? "#E3B44F" : "#E9E9EE"
                font.bold: true; font.pixelSize: 16 * sendBox.ks
            }
            Text
            {
                x: 40 * sendBox.ks; y: sendBox.height - 124 * sendBox.ks; width: sendBox.width - 80 * sendBox.ks; elide: Text.ElideRight
                text: qsTr("Then: put it in qlcplus-pr on the Mac (USB stick / AirDrop / OneDrive) and tell me.")
                color: "#8C8C96"; font.pixelSize: 14 * sendBox.ks
            }
            TrackTile
            {
                objectName: "sendCancel"
                x: 40 * sendBox.ks; y: sendBox.height - 92 * sendBox.ks; width: 340 * sendBox.ks; height: 64 * sendBox.ks
                label: sendBox.done ? qsTr("CLOSE") : qsTr("CANCEL")
                enabled: !sendBox.busy
                onTapped: setupRoot.sendOpen = false
            }
            TrackTile
            {
                objectName: "sendMake"
                x: 400 * sendBox.ks; y: sendBox.height - 92 * sendBox.ks; width: sendBox.width - 440 * sendBox.ks; height: 64 * sendBox.ks
                label: sendBox.busy ? qsTr("PACKING ...") : (sendBox.done ? qsTr("MAKE ANOTHER") : qsTr("MAKE THE ZIP"))
                active: !sendBox.busy
                activeColor: "#4FA3E3"
                onTapped:
                {
                    if (!trackEngine || sendBox.busy) return
                    var ids = []
                    for (var k in setupRoot.sendOn)
                        if (setupRoot.sendOn[k] === true) ids.push(k)
                    if (ids.length > 0) trackEngine.sendMake(ids)
                }
            }
        }
    }
}
