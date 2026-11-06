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
import QtQuick.Dialogs          // TRACK_APP_SHOW_R558: CHANGE's file dialog
import QtQuick.Layouts

import org.qlcplus.classes 1.0
import "."

Rectangle
{
    id: setupRoot
    // TRACK_APP_STYLE_R557: the Track page's card in the TRACK app
    color: setupRoot.trackApp ? "#141418" : "#262626"
    radius: setupRoot.trackApp ? 14 : 4

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
        Text { x: 22; y: 100; width: card.width - 44; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight; text: card.line2; color: "#E0E0E6"; font.pixelSize: 13 }
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

    readonly property color cText: trackApp ? "#E6E6EA" : "#EEEEEE"     // TRACK_APP_STYLE_R557
    readonly property color cDim:  trackApp ? "#8A8A94" : "#9A9A9A"
    readonly property color cLine: trackApp ? "#2A2A33" : (setupRoot.trackApp ? "#2A2A33" : "#555555")

    property string filter: ""
    property bool advancedOpen: false
    // TRACK_APP_SETUP_R550: the TRACK app's SETUP - no RE-GUESS / FULL AUTO / SELF TEST up top, RE-GUESS ROLES
    // in ADVANCED, and a group switches on / off with two taps
    readonly property bool trackApp: typeof trackAppCtl !== "undefined" && !!trackAppCtl && trackAppCtl.active === true
    property bool cacheOpen: false
    property bool roomOpen: false        // TRACK_APP_UI_R560: ROOM - the room map, over the lists
    // TRACK_APP_FIX_R570: what opens under ROOM closes it
    onLabMenuOpenChanged: if (labMenuOpen) roomOpen = false
    onLearnedOpenChanged: if (learnedOpen) roomOpen = false
    onMidiOpenChanged: if (midiOpen) roomOpen = false
    onAdvancedOpenChanged: if (advancedOpen) roomOpen = false
    onFullListChanged: if (fullList) roomOpen = false
    onFilterChanged: if (filter !== "") roomOpen = false
    // TRACK_APP_LIST_R557 (Tobias 11-04: "gemme listen over alle programmer under show all og maaske kun vise en
    // liste over sidste gemte og manuelt vaegtede looks/programmer"): in the TRACK app the list is SAVED LOOKS
    // + YOUR CHANGES; SHOW ALL shows every programme as before (HIDDEN: the hidden ones too)
    property bool fullList: false
    // TRACK_APP_CACHE_R557 ("cache siden i settings skal fylde hele skaermen")
    readonly property bool cacheFull: trackApp && cacheOpen
    readonly property bool listMine: trackApp && !fullList && !cacheFull
    readonly property bool advShown: advancedOpen && !cacheFull
    property var handRows: []
    function reloadHand()
    {
        handRows = (trackApp && trackEngine && typeof trackEngine.handChanges === "function") ? trackEngine.handChanges() : []
    }
    // EDIT on a row of YOUR CHANGES: the full list, searched for it
    function editProgramme(name)
    {
        fullList = true
        if (trackEngine && trackEngine.showAll === false)
            trackEngine.showAll = true           // TRACK_APP_FIX_R570: the hidden ones too
        filterInput.text = name
    }
    property bool learnedOpen: false     // R436_LEARNED
    property bool midiOpen: false        // R486_MI_SETUP_OPEN
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
        return (setupRoot.trackApp ? "#202027" : "#3A3A3A")
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
        reloadHand()                     // TRACK_APP_LIST_R557
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
            setupRoot.reloadHand()       // TRACK_APP_LIST_R557
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
                color: (setupRoot.trackApp ? "#0E0E11" : "#1B1B1B")
                radius: setupRoot.trackApp ? 8 : 3
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
                    color: (setupRoot.trackApp ? "#5E5E68" : "#666666")
                    font.pixelSize: 14
                }
            }

            Item { Layout.fillWidth: true }

            TrackTile
            {
                Layout.preferredWidth: 100
                Layout.preferredHeight: 34
                objectName: "showAllTile"
                label: qsTr("SHOW ALL")
                // TRACK_APP_LIST_R557: in the TRACK app SHOW ALL is the full list of programmes
                active: setupRoot.trackApp ? setupRoot.fullList : (trackEngine ? trackEngine.showAll : false)
                onTapped:
                {
                    if (setupRoot.trackApp)
                        setupRoot.fullList = !setupRoot.fullList
                    else if (trackEngine)
                        trackEngine.showAll = !trackEngine.showAll
                }
            }
            // TRACK_APP_LIST_R557: the full list's own SHOW ALL of before - the hidden rows too
            TrackTile
            {
                objectName: "hiddenTile"
                visible: setupRoot.trackApp && setupRoot.fullList
                Layout.preferredWidth: 90
                Layout.preferredHeight: 34
                label: qsTr("HIDDEN")
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
                visible: !setupRoot.trackApp          // TRACK_APP_SETUP_R550: in ADVANCED (TRACK ANALYSIS)
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
                visible: !setupRoot.trackApp          // TRACK_APP_SETUP_R550: always on in the TRACK app
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
                visible: !setupRoot.trackApp          // TRACK_APP_SETUP_R550
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

            // R486_MI_SETUP_TILE: MIDI LEARN's list (64 wide: the row still fits at 1366)
            TrackTile
            {
                objectName: "midiTile"
                Layout.preferredWidth: 64
                Layout.preferredHeight: 34
                label: qsTr("MIDI")
                active: setupRoot.midiOpen
                activeColor: "#4FA3E3"
                onTapped: setupRoot.midiOpen = !setupRoot.midiOpen
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
                    if (setupRoot.cacheOpen) setupRoot.roomOpen = false      // TRACK_APP_UI_R560
                    if (setupRoot.cacheOpen && trackManager) trackManager.requestCache()
                }
            }

            // TRACK_APP_UI_R560: ROOM - the room map with every lamp as it is lit now
            TrackTile
            {
                objectName: "roomTile"
                visible: setupRoot.trackApp
                Layout.preferredWidth: 90
                Layout.preferredHeight: 34
                label: qsTr("ROOM")
                active: setupRoot.roomOpen
                activeColor: "#4FA3E3"
                onTapped:
                {
                    setupRoot.roomOpen = !setupRoot.roomOpen
                    if (setupRoot.roomOpen) setupRoot.cacheOpen = false
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

        // SAVED_LOOKS_SETUP: separate row keeps the 1366 toolbar intact.
        TrackTile {
            objectName: "savedLooksSetup"
            visible: setupRoot.host && setupRoot.host.savedUiAvailable === true && !setupRoot.trackApp   // TRACK_APP_LIST_R557: in the list
            Layout.preferredWidth: 180; Layout.preferredHeight: visible ? 40 : 0
            label: qsTr("SAVED LOOKS")
            onTapped: if (setupRoot.host) setupRoot.host.savedFavoritesOpen = true
        }

        // ------------------------------------------------- R486_MI_SETUP (fold-out)
        // Tobias 10-21 (mockups 1 + 6 approved): where QLC+ hears MIDI, the last message
        // (input only - the one feedback there is), LEARN, the list with CLEAR, CLEAR ALL (SURE?)
        Rectangle
        {
            id: midiPanel
            objectName: "midiPanel"
            Layout.fillWidth: true
            Layout.fillHeight: false
            readonly property var rows: trackManager && trackManager.midiMappings ? trackManager.midiMappings : []
            readonly property string listen: trackManager && trackManager.midiListen ? trackManager.midiListen : ""
            readonly property real listH: Math.min(Math.max(1, rows.length) * 38, Math.max(38 * 3, setupRoot.height - 330))
            readonly property real wantH: 124 + listH + 10
            Layout.preferredHeight: visible ? wantH : 0
            Layout.maximumHeight: visible ? wantH : 0
            visible: setupRoot.midiOpen
            onVisibleChanged: if (visible && trackManager) trackManager.midiRefresh()
            color: "#1E1E21"
            radius: 6
            readonly property real cName: 190
            readonly property real cFrom: 300
            readonly property real cKind: 96

            Text { x: 20; y: 14; text: qsTr("MIDI CONTROLLER"); color: setupRoot.cText; font.bold: true; font.pixelSize: 16 }
            Text
            {
                x: 190; y: 16; width: parent.width - 190 - 290; elide: Text.ElideRight
                text: qsTr("Input only - nothing is sent back. A fader takes over when it passes the value on the screen.")
                color: setupRoot.cDim; font.pixelSize: 13
            }
            TrackTile
            {
                objectName: "midiLearnTile"
                x: parent.width - 20 - 120 - 8 - 130; y: 8; width: 130; height: 34
                label: qsTr("LEARN"); active: true; activeColor: "#4FA3E3"
                onTapped:
                {
                    if (!trackManager) return
                    trackManager.midiLearnStart()
                    // the controls are on the page: SETUP closes
                    if (setupRoot.host) setupRoot.host.setupOpen = false
                }
            }
            TrackTile
            {
                id: midiClearAllTile
                objectName: "midiClearAll"
                property bool armed: false
                x: parent.width - 20 - 120; y: 8; width: 120; height: 34
                label: armed ? qsTr("SURE?") : qsTr("CLEAR ALL")
                active: armed; activeColor: "#E3B44F"
                enabled: midiPanel.rows.length > 0
                opacity: enabled ? 1 : 0.4
                onTapped:
                {
                    if (armed === false) { armed = true; midiClearArm.restart(); return }
                    armed = false
                    if (trackManager) trackManager.midiClearAll()
                }
                Timer { id: midiClearArm; interval: 4000; onTriggered: midiClearAllTile.armed = false }
            }
            // where QLC+ hears MIDI
            Rectangle { x: 20; y: 56; width: 10; height: 10; radius: 5; color: midiPanel.listen !== "" ? "#6ECD82" : "#F05050" }
            Text
            {
                objectName: "midiListen"
                x: 38; y: 50; width: parent.width - 58; elide: Text.ElideRight
                text: midiPanel.listen !== "" ? midiPanel.listen
                      : (typeof trackAppCtl !== "undefined" && !!trackAppCtl && trackAppCtl.active === true)   // TRACK_APP_NAME_R549
                        ? qsTr("No MIDI input patched in the show - patch your controller once in QLC+ under Inputs/Outputs and save the show")
                        : qsTr("No MIDI input patched in QLC+ - patch your controller under Inputs/Outputs (start QLC+ once without -k, then save the show)")
                color: midiPanel.listen !== "" ? setupRoot.cText : "#F05050"; font.pixelSize: 14; font.bold: midiPanel.listen === ""
            }
            Text
            {
                objectName: "midiLast"
                x: 38; y: 72; width: parent.width - 58; elide: Text.ElideRight
                text: trackManager && trackManager.midiLast ? trackManager.midiLast : qsTr("Last heard: nothing yet")
                color: "#6A6A74"; font.pixelSize: 13
            }
            Repeater
            {
                model: [ { x: 20, t: qsTr("CONTROL") }, { x: 20 + midiPanel.cName, t: qsTr("FROM YOUR CONTROLLER") },
                         { x: 20 + midiPanel.cName + midiPanel.cFrom, t: qsTr("KIND") },
                         { x: 20 + midiPanel.cName + midiPanel.cFrom + midiPanel.cKind, t: qsTr("NOW") } ]
                Text { x: modelData.x; y: 100; text: modelData.t; color: "#6A6A74"; font.bold: true; font.pixelSize: 11; font.letterSpacing: 1 }
            }
            Text
            {
                visible: midiPanel.rows.length === 0
                x: 20; y: 124; width: parent.width - 40; height: 38; verticalAlignment: Text.AlignVCenter
                text: qsTr("Nothing learned yet. Tap LEARN, tap a control on the page, then move a fader or press a button on your controller.")
                color: setupRoot.cDim; font.pixelSize: 14; elide: Text.ElideRight
            }
            ListView
            {
                x: 12; y: 120; width: parent.width - 24; height: midiPanel.listH
                clip: true
                interactive: contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                // R491_M_CLEAR_TAP: the rows by index - a new list (a mapped fader moving, 10 a second) rebuilt
                // every row, and a CLEAR under the finger died between press and release
                model: midiPanel.rows.length
                delegate: Rectangle
                {
                    id: mrow
                    readonly property var r: midiPanel.rows[index] || ({})
                    objectName: "midiRow:" + mrow.r.t
                    width: ListView.view.width; height: 38
                    color: "transparent"
                    Rectangle
                    {
                        anchors.fill: parent; anchors.bottomMargin: 4; radius: 4
                        color: index % 2 === 0 ? "#242428" : "#1E1E21"
                    }
                    Text { x: 8; width: midiPanel.cName - 12; y: 8; elide: Text.ElideRight
                           text: mrow.r.n; color: setupRoot.cText; font.bold: true; font.pixelSize: 14 }
                    Text { x: 8 + midiPanel.cName; width: midiPanel.cFrom - 12; y: 8; elide: Text.ElideRight
                           text: mrow.r.f; color: "#4FA3E3"; font.pixelSize: 14 }
                    Rectangle
                    {
                        x: 8 + midiPanel.cName + midiPanel.cFrom; y: 7
                        width: 70; height: 20; radius: 3; color: "transparent"; border.width: 1; border.color: "#4A4A52"
                        Text { anchors.centerIn: parent; text: mrow.r.k; color: setupRoot.cDim; font.bold: true; font.pixelSize: 11 }
                    }
                    Text { x: 8 + midiPanel.cName + midiPanel.cFrom + midiPanel.cKind; width: mrow.width - x - 100; y: 9
                           elide: Text.ElideRight; text: mrow.r.s; color: mrow.r.w ? "#E3B44F" : setupRoot.cDim; font.pixelSize: 13 }
                    TrackTile
                    {
                        objectName: "midiClear:" + mrow.r.t
                        anchors.right: parent.right; anchors.rightMargin: 4; y: 4
                        width: 84; height: 26; label: qsTr("CLEAR")
                        onTapped: if (trackManager) trackManager.midiClear(mrow.r.t)
                    }
                }
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
                    line2: trackEngine && trackEngine.labMessage !== "" ? trackEngine.labMessage
                           : qsTr("height held at yours · no tilt toward the room")
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
                    line2: trackEngine && trackEngine.slabKind === 0 && trackEngine.slabMessage !== "" ? trackEngine.slabMessage
                           : qsTr("✕ %1 banned  ·  ✓ %2 kept").arg(labMenu.sum(0, "banned", 0)).arg(labMenu.sum(0, "good", 0))
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
                    line2: trackEngine && trackEngine.slabKind === 1 && trackEngine.slabMessage !== "" ? trackEngine.slabMessage
                           : qsTr("✕ %1 banned  ·  ✓ %2 kept").arg(labMenu.sum(1, "banned", 0)).arg(labMenu.sum(1, "good", 0))
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
                    line2: trackEngine && trackEngine.clabMessage !== "" ? trackEngine.clabMessage
                           : trackEngine && trackEngine.clabChanges > 0 ? qsTr("%1 levels set by you").arg(trackEngine.clabChanges)   // R542_CLAB_CARD
                                                                     : qsTr("every colour as it has always been")
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
            Layout.fillHeight: setupRoot.cacheFull          // TRACK_APP_CACHE_R557: all of SETUP in the TRACK app
            Layout.preferredHeight: visible ? 260 : 0
            Layout.maximumHeight: setupRoot.cacheFull ? 100000 : 260
            visible: setupRoot.cacheOpen
            color: (setupRoot.trackApp ? "#17171C" : "#181818")
            radius: setupRoot.trackApp ? 10 : 4
            border.width: 1
            border.color: (setupRoot.trackApp ? "#2A2A33" : "#333333")

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
                                 ? ((typeof trackAppCtl !== "undefined" && !!trackAppCtl && trackAppCtl.active === true) ? qsTr("TRACK's CDJ reader is not running - the cache lives there")   // TRACK_APP_UI_R548
                                                                   : qsTr("Beat Link Trigger is not connected - the cache lives there"))
                                 : ((typeof trackAppCtl !== "undefined" && !!trackAppCtl && trackAppCtl.active === true) ? qsTr("%1 tracks in the cache - MANUAL ones are your corrections and never re-analysed").arg(trackManager.cacheList.length)
                                                                   : qsTr("%1 tracks in BLT's cache - MANUAL ones are your corrections and never re-analysed").arg(trackManager.cacheList.length)))
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
                        color: (setupRoot.trackApp ? "#202027" : "#1F1F1F")
                        radius: setupRoot.trackApp ? 8 : 3

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
                                color: md.manual ? "#E3B44F" : (setupRoot.trackApp ? "#202027" : "#3A3A3A")
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
            visible: !setupRoot.cacheFull        // TRACK_APP_CACHE_R557
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 56
            Layout.maximumHeight: 56
            spacing: 6

            Text
            {
                Layout.preferredWidth: 96
                Layout.minimumWidth: 96
                text: qsTr("GROUPS") + "\n" + (setupRoot.trackApp ? qsTr("2 taps: on / off") + "\n" + qsTr("hold: base")   // TRACK_APP_SETUP_R550
                                                              : qsTr("tap: on / off · hold: base"))   // runde 234 changed the tap (fixed runde 248)
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
                    radius: setupRoot.trackApp ? 8 : 3
                    // TRACK_APP_SETUP_R550: the first tap only asks (SURE?), the second within 4 s switches
                    property bool armed: false
                    Timer { id: groupArm; interval: 4000; onTriggered: groupTile.armed = false }
                    color: armed ? "#3C2E12" : (g.base ? "#2A3F55" : (g.enabled ? (setupRoot.trackApp ? "#2A2A33" : "#333333") : (setupRoot.trackApp ? "#202027" : "#1F1F1F")))
                    border.width: armed || g.base ? 2 : (setupRoot.trackApp ? 0 : 1)
                    border.color: armed ? "#E3B44F" : (g.base ? "#4FA3E3" : (g.enabled ? (setupRoot.trackApp ? "#5E5E68" : "#666666") : (setupRoot.trackApp ? "#2A2A33" : "#333333")))

                    Column
                    {
                        anchors.centerIn: parent
                        spacing: 1

                        Text
                        {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: Math.min(implicitWidth, groupTile.width - 10)   // TRACK_APP_FIX_R568: inside the tile
                            fontSizeMode: Text.HorizontalFit; minimumPixelSize: 9; horizontalAlignment: Text.AlignHCenter
                            text: groupTile.armed ? (groupTile.g.enabled ? qsTr("SURE? - TAP AGAIN TO SWITCH OFF") : qsTr("SURE? - TAP AGAIN TO SWITCH ON"))
                                  : groupTile.g.key
                                  + (groupTile.g.base ? "  · BASE" : (groupTile.g.enabled ? "" : "  · OFF"))
                            color: groupTile.g.enabled ? setupRoot.cText : (setupRoot.trackApp ? "#5E5E68" : "#666666")
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
                        onClicked:
                        {
                            if (!trackEngine || !groupTile.g.key)
                                return
                            if (setupRoot.trackApp && groupTile.armed === false)
                            {
                                groupTile.armed = true
                                groupArm.restart()
                                return
                            }
                            groupTile.armed = false
                            trackEngine.cycleGroup(groupTile.g.key)
                        }
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
            visible: !setupRoot.cacheFull && !setupRoot.listMine   // TRACK_APP_LIST_R557: with SHOW ALL's list
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
                    color: (setupRoot.trackApp ? "#0E0E11" : "#1B1B1B")
                    radius: setupRoot.trackApp ? 8 : 3
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
                color: (setupRoot.trackApp ? "#0E0E11" : "#1B1B1B")
                radius: setupRoot.trackApp ? 8 : 3
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
            objectName: "funcList"
            visible: !setupRoot.cacheFull && !setupRoot.listMine   // TRACK_APP_LIST_R557: SHOW ALL
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
                color: (modelData && modelData.hidden) ? (setupRoot.trackApp ? "#17171C" : "#181818") : (setupRoot.trackApp ? "#202027" : "#1F1F1F")
                radius: setupRoot.trackApp ? 8 : 3

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

        // ------------------------------------------------- TRACK_APP_LIST_R557: SAVED LOOKS + YOUR CHANGES
        // Tobias 11-04 (mockup approved): the TRACK app's SETUP lists what is his - the saved looks (newest first,
        // NEXT queues one for its section, REMOVE two taps) and the programmes he changed (a role, energy stars,
        // a ban, a thumb - newest first; EDIT opens it in SHOW ALL's list, RESET two taps gives it back)
        Flickable
        {
            id: handList
            objectName: "handList"
            visible: setupRoot.listMine
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 120
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            contentWidth: width
            contentHeight: handCol.height + 8
            readonly property var looks: trackEngine && trackEngine.savedLooks ? trackEngine.savedLooks : []

            Column
            {
                id: handCol
                width: handList.width
                spacing: 6

                Row
                {
                    spacing: 12
                    height: 26
                    Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("SAVED LOOKS"); color: "#8A8A94"; font.bold: true; font.pixelSize: 12; font.letterSpacing: 1.6 }
                    Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("newest first · NEXT plays it at its next section"); color: "#6E6E78"; font.pixelSize: 12 }
                }
                Text
                {
                    visible: handList.looks.length === 0
                    leftPadding: 12
                    text: qsTr("No saved looks yet - SAVE on the Track page keeps the look that runs")
                    color: "#6E6E78"; font.pixelSize: 13
                }
                Repeater
                {
                    model: handList.looks
                    Rectangle
                    {
                        id: lookRow
                        objectName: "handLook:" + index
                        width: handCol.width
                        height: 44
                        radius: 10
                        color: "#202027"
                        property bool armed: false
                        Timer { id: lookArm; interval: 4000; onTriggered: lookRow.armed = false }
                        Text
                        {
                            x: 14; width: 300; anchors.verticalCenter: parent.verticalCenter
                            text: modelData.name || ""; color: "#E6E6EA"; font.bold: true; font.pixelSize: 13; elide: Text.ElideRight
                        }
                        Rectangle
                        {
                            x: 324; anchors.verticalCenter: parent.verticalCenter
                            width: 64; height: 22; radius: 8
                            readonly property string sec: (modelData.section || "").toUpperCase()
                            color: sec === "DROP" ? "#402126" : (sec === "BREAK" ? "#183148" : "#303039")
                            Text { anchors.centerIn: parent; text: parent.sec; color: parent.sec === "DROP" ? "#FF9299" : (parent.sec === "BREAK" ? "#7EC5FF" : "#C0C0CE"); font.bold: true; font.pixelSize: 11 }
                        }
                        Text
                        {
                            x: 404; width: parent.width - 404 - 260; anchors.verticalCenter: parent.verticalCenter
                            text: (modelData.title || "") + (modelData.groups && modelData.groups.length ? "  ·  " + modelData.groups.join(" · ") : "")
                            color: "#A0A0AA"; font.pixelSize: 12; elide: Text.ElideRight
                        }
                        TrackTile
                        {
                            objectName: "handLookNext:" + index
                            x: parent.width - 252; anchors.verticalCenter: parent.verticalCenter
                            width: 140; height: 32
                            enabled: modelData.canQueue === true
                            active: modelData.queued === true
                            activeColor: "#6ECD82"
                            label: active ? qsTr("QUEUED") : (enabled ? qsTr("NEXT ") + (modelData.section || "").toUpperCase() : (modelData.reason || qsTr("NO NEXT")))
                            onTapped: if (trackEngine && enabled) trackEngine.queueSavedLook(modelData.id)
                        }
                        TrackTile
                        {
                            objectName: "handLookRemove:" + index
                            x: parent.width - 104; anchors.verticalCenter: parent.verticalCenter
                            width: 92; height: 32
                            label: lookRow.armed ? qsTr("SURE?") : qsTr("REMOVE")
                            active: lookRow.armed
                            activeColor: "#E36B6B"
                            onTapped:
                            {
                                if (lookRow.armed === false) { lookRow.armed = true; lookArm.restart(); return }
                                lookRow.armed = false
                                if (trackEngine) trackEngine.removeSavedLook(modelData.id)
                            }
                        }
                    }
                }

                Item { width: 1; height: 6 }
                Row
                {
                    spacing: 12
                    height: 26
                    Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("YOUR CHANGES"); color: "#8A8A94"; font.bold: true; font.pixelSize: 12; font.letterSpacing: 1.6 }
                    Text { anchors.verticalCenter: parent.verticalCenter; text: qsTr("a role, energy stars, a ban or a thumb you gave - newest first · RESET gives it back to the engine (2 taps)"); color: "#6E6E78"; font.pixelSize: 12 }
                }
                Text
                {
                    visible: setupRoot.handRows.length === 0
                    leftPadding: 12
                    text: qsTr("Nothing changed by hand yet - SHOW ALL lists every programme")
                    color: "#6E6E78"; font.pixelSize: 13
                }
                Repeater
                {
                    model: setupRoot.handRows
                    Rectangle
                    {
                        id: handRow
                        objectName: "handRow:" + index
                        width: handCol.width
                        height: 44
                        radius: 10
                        color: "#202027"
                        property bool armed: false
                        Timer { id: handArm; interval: 4000; onTriggered: handRow.armed = false }
                        readonly property var md: modelData || ({})
                        Text
                        {
                            x: 14; width: 330; anchors.verticalCenter: parent.verticalCenter
                            text: handRow.md.name || ""; color: "#E6E6EA"; font.bold: true; font.pixelSize: 13; elide: Text.ElideRight
                        }
                        Text
                        {
                            x: 352; width: 150; anchors.verticalCenter: parent.verticalCenter
                            text: handRow.md.group || ""; color: "#8A8A94"; font.pixelSize: 12; elide: Text.ElideRight
                        }
                        Row
                        {
                            x: 512; anchors.verticalCenter: parent.verticalCenter
                            spacing: 8
                            // what of it is his
                            Rectangle
                            {
                                visible: handRow.md.role !== handRow.md.guess
                                width: roleChip.implicitWidth + 18; height: 22; radius: 11; color: "#22374D"
                                Text { id: roleChip; anchors.centerIn: parent; color: "#CFE6FF"; font.bold: true; font.pixelSize: 11
                                       text: setupRoot.roleShort(handRow.md.role) + " · " + qsTr("was") + " " + setupRoot.roleShort(handRow.md.guess) }
                            }
                            Rectangle
                            {
                                visible: handRow.md.stars !== handRow.md.starsGuess
                                width: starChip.implicitWidth + 18; height: 22; radius: 11; color: "#4A3C15"
                                Text { id: starChip; anchors.centerIn: parent; color: "#F5D58A"; font.bold: true; font.pixelSize: 11
                                       text: "★".repeat(Math.max(1, handRow.md.stars || 1)) + " " + qsTr("you") + " · " + qsTr("was") + " " + "★".repeat(Math.max(1, handRow.md.starsGuess || 1)) }
                            }
                            Rectangle
                            {
                                visible: handRow.md.banned === true
                                width: banChip.implicitWidth + 18; height: 22; radius: 11; color: "#4A1A1A"
                                Text { id: banChip; anchors.centerIn: parent; text: qsTr("BANNED"); color: "#FFB4B4"; font.bold: true; font.pixelSize: 11 }
                            }
                            Rectangle
                            {
                                visible: (handRow.md.rateUp || 0) > 0 || (handRow.md.rateDown || 0) > 0
                                width: rateChip.implicitWidth + 18; height: 22; radius: 11; color: "#173A22"
                                Text { id: rateChip; anchors.centerIn: parent; color: "#A8F0B8"; font.bold: true; font.pixelSize: 11
                                       text: ((handRow.md.rateUp || 0) > 0 ? "+" + handRow.md.rateUp : "") + ((handRow.md.rateUp || 0) > 0 && (handRow.md.rateDown || 0) > 0 ? "  " : "")
                                             + ((handRow.md.rateDown || 0) > 0 ? "−" + handRow.md.rateDown : "") }
                            }
                        }
                        Text
                        {
                            x: parent.width - 330; width: 110; anchors.verticalCenter: parent.verticalCenter
                            text: handRow.md.when || ""; color: "#8A8A94"; font.pixelSize: 12; horizontalAlignment: Text.AlignRight
                        }
                        TrackTile
                        {
                            objectName: "handEdit:" + index
                            x: parent.width - 204; anchors.verticalCenter: parent.verticalCenter
                            width: 92; height: 32
                            label: qsTr("EDIT")
                            onTapped: setupRoot.editProgramme(handRow.md.name || "")
                        }
                        TrackTile
                        {
                            objectName: "handReset:" + index
                            x: parent.width - 104; anchors.verticalCenter: parent.verticalCenter
                            width: 92; height: 32
                            label: handRow.armed ? qsTr("SURE?") : qsTr("RESET")
                            active: handRow.armed
                            activeColor: "#E3B44F"
                            onTapped:
                            {
                                if (handRow.armed === false) { handRow.armed = true; handArm.restart(); return }
                                handRow.armed = false
                                if (trackEngine) trackEngine.handReset(handRow.md.id)
                            }
                        }
                    }
                }
                Text
                {
                    leftPadding: 4
                    text: setupRoot.handRows.length + " " + qsTr("changes") + "  ·  " + handList.looks.length + " " + qsTr("saved looks")
                          + "  ·  " + qsTr("SHOW ALL = every programme, as the list was - with roles, stars, BAN and the search")
                    color: "#6E6E78"; font.pixelSize: 12
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
            visible: setupRoot.advShown
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
                    Layout.preferredWidth: 66   // TRACK_APP_FIX_R570
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
                Layout.preferredWidth: 150
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
                Layout.preferredWidth: 118
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
                Layout.preferredWidth: 118
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
                Layout.preferredWidth: 132
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
                Layout.preferredWidth: 76
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
                Layout.preferredWidth: 76
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

        // ------------------------------------------------- R526_ANALYSIS_ROW
        // Tobias 11-01 ("slaa punkt 3 til og fra inde i selve programmet ... saa vi kan teste det naeste gang
        // paa riggen"; mockup approved the same day): AI SECTIONS / CLASSIC picks whose sections the track
        // gets, AI DETAILS switches the fine events (R527_AI_ROWDOC: the pre-drop starts the blink early, the
        // dips in a drop reach the pause hit; the outro is not used). A switch reads
        // the playing track again at once; flags set by hand always win.
        RowLayout
        {
            objectName: "analysisRow"
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 40
            visible: setupRoot.advShown
            spacing: 10
            Text { text: qsTr("TRACK ANALYSIS"); color: "#E3B44F"; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.4 }
            TrackTile
            {
                objectName: "analysisAi"
                Layout.preferredWidth: 130
                Layout.preferredHeight: 34
                label: qsTr("AI SECTIONS")
                active: trackManager ? trackManager.aiSections === true : false
                activeColor: "#4FA3E3"
                onTapped: if (trackManager) trackManager.aiSections = true
            }
            TrackTile
            {
                objectName: "analysisClassic"
                Layout.preferredWidth: 110
                Layout.preferredHeight: 34
                label: qsTr("CLASSIC")
                active: trackManager ? trackManager.aiSections === false : false
                activeColor: "#4FA3E3"
                onTapped: if (trackManager) trackManager.aiSections = false
            }
            Item { Layout.preferredWidth: 14 }
            TrackTile
            {
                objectName: "analysisDetails"
                Layout.preferredWidth: 130
                Layout.preferredHeight: 34
                label: qsTr("AI DETAILS")
                active: trackManager ? trackManager.aiDetails === true : false
                activeColor: "#7ED07E"
                onTapped: if (trackManager) trackManager.aiDetails = !trackManager.aiDetails
            }
            Text
            {
                objectName: "analysisSub"
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: (trackManager && trackManager.aiSections === false ? qsTr("our own analysis") : qsTr("sections learnt from Time-line Live"))
                      + "  ·  " + (trackManager && trackManager.aiDetails === true ? qsTr("details on: the blink starts earlier, the pause hit takes the drop's dips too") /* R527_AI_SUB */
                                                                                    : qsTr("details off"))
                      + "  ·  " + qsTr("your own flags always win")
                color: setupRoot.cDim
                font.pixelSize: 12
            }
            // TRACK_APP_SETUP_R550: RE-GUESS, out of the top row - it re-guesses roles set by hand: two taps
            TrackTile
            {
                id: appReguessTile
                objectName: "appReguess"
                visible: setupRoot.trackApp
                property bool armed: false
                Layout.preferredWidth: 150
                Layout.preferredHeight: 34
                label: armed ? qsTr("SURE?") : qsTr("RE-GUESS ROLES")
                active: armed
                activeColor: "#E3B44F"
                onTapped:
                {
                    if (armed === false) { armed = true; appReguessArm.restart(); return }
                    armed = false
                    if (trackEngine) trackEngine.autoAssign(true)
                }
                Timer { id: appReguessArm; interval: 4000; onTriggered: appReguessTile.armed = false }
            }
        }

        // ------------------------------------------------- R536_UI_ROW
        // Tobias 11-02 ("tilfoej en knap i advanced (default off), jeg kan slaa det til og fra paa"; mockup approved
        // the same day): BREAK STABS - quiet breaks from 80 %: a random strobe head blinks on each sharp sound,
        // in the room colour. One tap on, one tap off; kept for the next time.
        RowLayout
        {
            objectName: "breakStabsRow"
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 40
            visible: setupRoot.advShown
            spacing: 10
            Text { text: qsTr("BREAK STABS"); color: "#E3B44F"; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.4 }
            TrackTile
            {
                objectName: "breakStabsTile"
                Layout.preferredWidth: 130
                Layout.preferredHeight: 34
                label: trackEngine && trackEngine.breakStabs === true ? qsTr("ON") : qsTr("OFF")
                active: trackEngine ? trackEngine.breakStabs === true : false
                activeColor: "#7ED07E"
                onTapped: if (trackEngine) trackEngine.breakStabs = !trackEngine.breakStabs
            }
            Text
            {
                objectName: "breakStabsSub"
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: qsTr("quiet breaks from 80 %: a random strobe head blinks on each sharp sound, in the room colour")
                      + "  ·  " + (trackEngine && trackEngine.breakStabs === true ? qsTr("on") : qsTr("off"))
                color: setupRoot.cDim
                font.pixelSize: 12
            }
        }

        // ------------------------------------------------- R484_EH_ROW_LEGEND
        // Tobias 10-20: "energi skal saette by hand i advanced saadan at den saetter tidspunkterne,
        // men vi kan 'override' ved manuelt at aendre i tidspunkt. Men laeringen skal skrives i
        // raekken." - what the row is: gold edge = his hand's, white edge = his own (a tap);
        // LEARN, then a tap on a point, gives it back to the learning
        RowLayout
        {
            objectName: "clockLegend"
            Layout.fillWidth: true
            Layout.fillHeight: false
            visible: setupRoot.advShown
            spacing: 10
            Text { text: qsTr("ENERGY BY CLOCK"); color: "#E3B44F"; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.4 }
            TrackTile
            {
                id: clockLearnTile
                objectName: "clockLearn"
                property bool armed: false
                visible: trackEngine ? trackEngine.clockSource.indexOf(2) >= 0 : false
                Layout.preferredWidth: 110
                Layout.preferredHeight: 30
                label: armed ? qsTr("TAP A POINT") : qsTr("LEARN")
                active: armed
                activeColor: "#E3B44F"
                onTapped: { armed = !armed; if (armed) clockLearnArm.restart() }
                Timer { id: clockLearnArm; interval: 6000; onTriggered: clockLearnTile.armed = false }
            }
            Text
            {
                objectName: "clockLegendSub"
                Layout.fillWidth: true
                elide: Text.ElideRight
                readonly property int view: trackEngine ? trackEngine.clockView : 0
                readonly property var nights: trackEngine ? trackEngine.clockHandNights : []
                text: qsTr("learned from your hand: ") + (view === 0 ? qsTr("Fri + Sat, the last 2 weekends") : view === 1 ? qsTr("every Fri + Sat") : qsTr("the other nights"))
                      + " - " + (nights.length > 0 ? qsTr("%1 nights (%2)").arg(nights.length).arg(nights.join(", ")) : qsTr("no nights yet"))
                      + "  ·  " + qsTr("gold edge = learned, white edge = set by you (a tap)  ·  AUTO plays it")
                color: setupRoot.cDim
                font.pixelSize: 12
            }
            // R484_EH_VIEW (Tobias 10-20: "samme valg som haze ... (efterlign haze)"): HAZE & FAN BY
            // CLOCK's three views - which nights the row learns from (kept; the last 2 weekends first)
            Repeater
            {
                model: [qsTr("LAST 2 WEEKENDS"), qsTr("ALL FRI + SAT"), qsTr("OTHER NIGHTS")]
                TrackTile
                {
                    objectName: "clockView:" + index
                    Layout.preferredWidth: 140
                    Layout.preferredHeight: 34
                    label: modelData
                    active: trackEngine ? trackEngine.clockView === index : false
                    activeColor: "#E3B44F"
                    onTapped: if (trackEngine) trackEngine.clockView = index
                }
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
            visible: setupRoot.advShown
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
                    // R484_EH_ROW_MARK: a tap is his point (white edge); after LEARN a tap gives it back
                    objectName: "clockPoint:" + index
                    readonly property int source: trackEngine && trackEngine.clockSource.length > index ? trackEngine.clockSource[index] : 0
                    onTapped:
                    {
                        if (!trackEngine)
                            return
                        if (clockLearnTile.armed)
                        {
                            clockLearnTile.armed = false
                            trackEngine.learnClockPoint(index)
                        }
                        else
                            trackEngine.cycleClockPoint(index)
                    }
                    Rectangle
                    {
                        objectName: "clockMark:" + index
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: 2
                        anchors.topMargin: 4
                        anchors.bottomMargin: 4
                        width: 5
                        visible: parent.source > 0
                        color: parent.source === 2 ? "#FFFFFF" : "#E3B44F"
                    }
                }
            }
        }

        // ------------------------------------------------- R470_CLOCK_HAND
        // R484_EH_ROW_MERGED (Tobias 10-20: "laeringen skal skrives i raekken"): YOUR HAND is
        // written into ENERGY BY CLOCK's row now - its legend says how many nights; the gold
        // row and USE YOURS are gone

        RowLayout
        {
            objectName: "hazeLimitRow"
            Layout.fillWidth: true
            visible: setupRoot.advShown && trackEngine && trackEngine.hazeAvailable
            Layout.preferredHeight: 48
            spacing: 10
            Text { text: qsTr("HAZE LIMIT"); color: "#E3B44F"; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.4; Layout.preferredWidth: 165 }
            TrackTile {
                objectName: "hazeLimitMinus"
                Layout.preferredWidth: 48; Layout.preferredHeight: 40
                label: "−"; enabled: trackEngine && trackEngine.hazeLimit > 0
                onTapped: if (trackEngine) trackEngine.hazeLimit = Math.max(0, trackEngine.hazeLimit - 5)
            }
            Text {
                objectName: "hazeLimitValue"
                text: (trackEngine ? trackEngine.hazeLimit : 70) + "%"
                color: "#EEEEEE"; font.pixelSize: 20; font.bold: true
                horizontalAlignment: Text.AlignHCenter; Layout.preferredWidth: 70
            }
            TrackTile {
                objectName: "hazeLimitPlus"
                Layout.preferredWidth: 48; Layout.preferredHeight: 40
                label: "+"; enabled: trackEngine && trackEngine.hazeLimit < 100
                onTapped: if (trackEngine) trackEngine.hazeLimit = Math.min(100, trackEngine.hazeLimit + 5)
            }
            Text {
                Layout.leftMargin: 12; Layout.fillWidth: true
                text: qsTr("Release the fader to return here. Hold above it for extra haze.")
                color: "#9A9A9A"; font.pixelSize: 13
            }
            Text { text: qsTr("5% steps · saved"); color: "#9A9A9A"; font.pixelSize: 12 }
        }
        // ------------------------------------------------- R473_ATMOS
        // HAZE & FAN BY CLOCK (R484_HZ_SETUP_ROW, Tobias 10-20: "Det samme princip skal gaelde for
        // haze"): was HAZE & FAN BY HAND (R473) - the row AUTO plays. His hand is learned into it
        // (gold edge); a tap on H or F is his (white edge), LEARN + a tap gives it back
        ColumnLayout
        {
            objectName: "atmosHand"
            Layout.fillWidth: true
            Layout.fillHeight: false
            visible: setupRoot.advShown && trackEngine && (trackEngine.atmosKnown === true || trackEngine.hazeAvailable === true)
            spacing: 6
            RowLayout
            {
                Layout.fillWidth: true
                spacing: 10
                Text { text: qsTr("HAZE & FAN BY CLOCK"); color: "#E3B44F"; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.4 }
                TrackTile
                {
                    id: atmosLearnTile
                    objectName: "atmosLearn"
                    property bool armed: false
                    visible: trackEngine && trackEngine.atmosCurve ? trackEngine.atmosCurve.some(function(p) { return p.src === 2 }) : false
                    Layout.preferredWidth: 110
                    Layout.preferredHeight: 30
                    label: armed ? qsTr("TAP A POINT") : qsTr("LEARN")
                    active: armed
                    activeColor: "#E3B44F"
                    onTapped: { armed = !armed; if (armed) atmosLearnArm.restart() }
                    Timer { id: atmosLearnArm; interval: 6000; onTriggered: atmosLearnTile.armed = false }
                }
                Text
                {
                    objectName: "atmosSub"
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    readonly property int view: trackEngine ? trackEngine.atmosView : 0
                    readonly property var nights: trackEngine ? trackEngine.atmosNights : []
                    text: qsTr("learned from your hand: ") + (view === 0 ? qsTr("Fri + Sat, the last 2 weekends") : view === 1 ? qsTr("every Fri + Sat") : qsTr("the other nights"))
                          + " - " + (nights.length > 0 ? qsTr("%1 nights (%2)").arg(nights.length).arg(nights.join(", ")) : qsTr("no nights yet"))
                          + "  ·  " + qsTr("gold edge = learned, white edge = set by you (tap H or F)  ·  AUTO plays it")
                    color: setupRoot.cDim; font.pixelSize: 12
                }
                Repeater
                {
                    model: [qsTr("LAST 2 WEEKENDS"), qsTr("ALL FRI + SAT"), qsTr("OTHER NIGHTS")]
                    TrackTile
                    {
                        objectName: "atmosView:" + index
                        Layout.preferredWidth: 140; Layout.preferredHeight: 34
                        label: modelData
                        active: trackEngine ? trackEngine.atmosView === index : false
                        activeColor: "#E3B44F"
                        onTapped: if (trackEngine) trackEngine.atmosView = index
                    }
                }
            }
            GridLayout
            {
                Layout.fillWidth: true
                columns: 14; columnSpacing: 4; rowSpacing: 4
                Repeater
                {
                    model: trackEngine ? trackEngine.atmosCurve : []
                    TrackTile
                    {
                        id: atmosPoint
                        objectName: "atmosHand:" + index
                        Layout.preferredWidth: 84         // "H60 F100" beside the edge - wider than ENERGY's 60
                        Layout.preferredHeight: 34
                        property int at: 20 * 60 + 15 * index
                        readonly property int h: modelData !== undefined ? modelData.h : -1
                        readonly property int f: modelData !== undefined ? modelData.f : -1
                        readonly property int src: modelData !== undefined ? modelData.src : 0
                        readonly property bool has: h >= 0 || f >= 0
                        label: ("0" + (Math.floor(at / 60) % 24)).slice(-2) + ":" + ("0" + (at % 60)).slice(-2)
                               + "\n" + (has ? "H" + (h >= 0 ? h : "-") + " F" + (f >= 0 ? f : "-") : "-")
                        active: has
                        activeColor: "#8C8C96"
                        opacity: has ? 1.0 : 0.45
                        function tap(which)
                        {
                            if (!trackEngine)
                                return
                            if (atmosLearnTile.armed)
                            {
                                atmosLearnTile.armed = false
                                trackEngine.learnAtmosPoint(index)
                            }
                            else
                                trackEngine.cycleAtmosPoint(index, which)
                        }
                        // the same marks as ENERGY BY CLOCK (R484_EH): gold = learned, white = his
                        Rectangle
                        {
                            objectName: "atmosMark:" + index
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            anchors.leftMargin: 2
                            anchors.topMargin: 4
                            anchors.bottomMargin: 4
                            width: 5
                            visible: atmosPoint.src > 0
                            color: atmosPoint.src === 2 ? "#FFFFFF" : "#E3B44F"
                        }
                        // the left half is H, the right half F
                        MouseArea { objectName: "atmosH:" + index; x: 0; width: parent.width / 2; height: parent.height; onClicked: atmosPoint.tap(0) }
                        MouseArea { objectName: "atmosF:" + index; x: parent.width / 2; width: parent.width / 2; height: parent.height; onClicked: atmosPoint.tap(1) }
                    }
                }
            }
        }

        // ------------------------------------------------- TRACK_APP_SHOW_R558: SHOW
        // Tobias 11-05 ("PSMAIN (showet) skal jo ogsaa hentes/aabnes ... naar man opdatere ... evt. ogsaa saa man kan
        // aendre stien i advanced"): the show TRACK opens (show.txt - a shortcut needs no file) and the rig's
        // PSMAIN.qxw, which comes with UPDATE TRACK. CHANGE picks another, OPEN NOW starts TRACK on it (two taps,
        // SHOW OFF), FOLLOW THE RIG on/off, TAKE THE RIG'S when his PSMAIN was saved since (two taps)
        RowLayout
        {
            id: showRow
            objectName: "showRow"
            readonly property var sh: setupRoot.trackApp && trackAppCtl && trackAppCtl.show ? trackAppCtl.show : null
            readonly property bool showOn: trackManager ? trackManager.autoRun === true : false
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 40
            visible: setupRoot.advShown && sh !== null
            spacing: 10
            property bool armOpen: false
            property bool armTake: false
            Timer { id: showArm; interval: 4000; onTriggered: { showRow.armOpen = false; showRow.armTake = false } }
            Text { text: qsTr("SHOW"); color: "#E3B44F"; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.4 }
            Text
            {
                objectName: "showText"
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                color: setupRoot.cDim
                font.pixelSize: 12
                text:
                {
                    var s = showRow.sh
                    if (!s) return ""
                    if (!s.available) return qsTr("the file the shortcut names · the show is chosen here only in an installed TRACK")
                    var t = (s.fileName !== "" ? s.fileName + "  ·  " + s.folder : qsTr("no show yet"))
                    var st = ""
                    switch (s.state) {
                    case 1: st = qsTr("the rig's newest (%1)").arg(s.rigDate); break
                    case 2: st = qsTr("the rig's new PSMAIN (%1) is here - comes with UPDATE TRACK").arg(s.rigDate); break
                    case 3: st = qsTr("saved since the rig's - yours stays (the rig's of %1: TAKE THE RIG'S)").arg(s.rigDate); break
                    case 4: st = qsTr("your own show - the rig's PSMAIN is not followed"); break
                    case 7: st = qsTr("FOLLOW THE RIG is off - the rig has another PSMAIN (%1)").arg(s.rigDate); break
                    default: st = s.note; break
                    }
                    if (st !== "") t += "  ·  " + st
                    if (s.restartNeeded) t += "  ·  " + qsTr("%1 is open - OPEN NOW switches").arg(s.opened.split(/[\\/]/).pop())
                    return t
                }
            }
            TrackTile
            {
                objectName: "showTake"
                visible: showRow.sh && showRow.sh.available && showRow.sh.state === 3
                Layout.preferredWidth: 190
                Layout.preferredHeight: 34
                label: showRow.armTake ? qsTr("SURE? - TAP AGAIN") : qsTr("TAKE THE RIG'S")
                active: showRow.armTake
                activeColor: "#E3B44F"
                onTapped:
                {
                    if (!showRow.sh) return
                    if (showRow.armTake === false) { showRow.armTake = true; showRow.armOpen = false; showArm.restart(); return }
                    showRow.armTake = false
                    showRow.sh.takeRig()
                }
            }
            TrackTile
            {
                objectName: "showOpen"
                visible: showRow.sh && showRow.sh.available && showRow.sh.restartNeeded
                Layout.preferredWidth: 170
                Layout.preferredHeight: 34
                readonly property int st: showRow.sh ? trackAppCtl.updateStep : 0
                enabled: st !== 1
                label: st === 3 ? qsTr("SHOW OFF FIRST")
                       : st === 2 ? qsTr("DID NOT START - SEE THE LOG")   // TRACK_APP_FIX_R570
                       : showRow.armOpen ? qsTr("SURE? - TAP AGAIN") : qsTr("OPEN NOW")
                active: true
                activeColor: showRow.armOpen ? "#F0C870" : "#E3B44F"
                onTapped:
                {
                    if (showRow.showOn) { trackAppCtl.restartTrack(true); return }
                    if (showRow.armOpen === false) { showRow.armOpen = true; showRow.armTake = false; showArm.restart(); return }
                    showRow.armOpen = false
                    trackAppCtl.restartTrack(false)
                }
            }
            TrackTile
            {
                objectName: "showFollow"
                visible: showRow.sh && showRow.sh.available
                Layout.preferredWidth: 170
                Layout.preferredHeight: 34
                label: qsTr("FOLLOW THE RIG")
                active: showRow.sh ? showRow.sh.follow === true : false
                activeColor: "#4FA3E3"
                onTapped: if (showRow.sh) showRow.sh.setFollow(!showRow.sh.follow)
            }
            TrackTile
            {
                objectName: "showChange"
                visible: showRow.sh && showRow.sh.available
                Layout.preferredWidth: 120
                Layout.preferredHeight: 34
                label: qsTr("CHANGE")
                onTapped: showDialog.open()
            }
            FileDialog
            {
                id: showDialog
                objectName: "showDialog"
                title: qsTr("The show TRACK opens")
                nameFilters: [ qsTr("QLC+ shows (*.qxw)") ]
                currentFolder: showRow.sh ? showRow.sh.folderUrl : ""
                onAccepted: if (showRow.sh) showRow.sh.setPath(selectedFile.toString())
            }
        }

        // ------------------------------------------------- TRACK_APP_READER_R557: CDJ READER
        // Tobias 11-04 ("Kan vi ogsaa checke og opdatere blt-hjaelperen ligesom vi checker for track? ... under
        // settings"; previews too, "en knap til at rulle tilbage til den sidst installerede"): the Beat Link Trigger
        // jar TRACK's CDJ reader runs on - CHECK NOW, UPDATE (two taps, SHOW OFF), ROLL BACK (two taps, SHOW OFF)
        RowLayout
        {
            id: readerRow
            objectName: "readerRow"
            readonly property var rd: setupRoot.trackApp && trackAppCtl && trackAppCtl.reader ? trackAppCtl.reader : null
            readonly property bool showOn: trackManager ? trackManager.autoRun === true : false
            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 40
            visible: setupRoot.advShown && rd !== null
            spacing: 10
            property bool armUpdate: false
            property bool armBack: false
            Timer { id: readerArm; interval: 4000; onTriggered: { readerRow.armUpdate = false; readerRow.armBack = false } }
            Text { text: qsTr("CDJ READER"); color: "#E3B44F"; font.bold: true; font.pixelSize: 13; font.letterSpacing: 1.4 }
            Text
            {
                objectName: "readerText"
                Layout.fillWidth: true
                elide: Text.ElideRight
                color: setupRoot.cDim
                font.pixelSize: 12
                text:
                {
                    var r = readerRow.rd
                    if (!r) return ""
                    if (!r.available) return qsTr("beat-link from Beat Link Trigger %1 · updates only in an installed TRACK").arg(r.version || "?")
                    var s = qsTr("beat-link from Beat Link Trigger %1").arg(r.version || "?") + (r.bundled ? " " + qsTr("(TRACK's own)") : "")
                    if (r.step === 1 || r.step === 2 || r.step === 3 || r.step === 4 || r.step === 5 || r.step === 6)
                        return s + "  ·  " + r.note
                    if (r.candidate !== "" && r.candidateReady)
                        s += "  ·  " + qsTr("%1%2 is out (%3) - downloaded and checked").arg(r.candidate).arg(r.candidatePreview ? " " + qsTr("PREVIEW") : "").arg(r.candidateDate)
                    else if (r.checkedAt !== "")
                        s += "  ·  " + qsTr("newest %1%2 - up to date · checked %3").arg(r.latestStable)
                                 .arg(r.latestPreview !== "" ? " (" + qsTr("preview") + " " + r.latestPreview + ")" : "").arg(r.checkedAt)
                    if (r.note !== "") s += "  ·  " + r.note
                    return s + "  ·  " + qsTr("reads only, never sends to the CDJs")
                }
            }
            TrackTile
            {
                objectName: "readerCheck"
                visible: readerRow.rd && readerRow.rd.available
                Layout.preferredWidth: 140
                Layout.preferredHeight: 34
                enabled: readerRow.rd && readerRow.rd.step !== 1 && readerRow.rd.step !== 4 && readerRow.rd.step !== 5
                label: qsTr("CHECK NOW")
                onTapped: if (readerRow.rd) readerRow.rd.checkNow()
            }
            TrackTile
            {
                objectName: "readerBack"
                visible: readerRow.rd && readerRow.rd.available && readerRow.rd.previous !== ""
                Layout.preferredWidth: 210
                Layout.preferredHeight: 34
                enabled: readerRow.rd && readerRow.rd.step !== 1
                label: readerRow.armBack ? qsTr("SURE? - TAP AGAIN") : qsTr("ROLL BACK TO %1").arg(readerRow.rd ? readerRow.rd.previous : "")
                active: readerRow.armBack
                activeColor: "#E3B44F"
                onTapped:
                {
                    if (!readerRow.rd) return
                    if (readerRow.showOn) { readerRow.rd.rollback(true); return }
                    if (readerRow.armBack === false) { readerRow.armBack = true; readerRow.armUpdate = false; readerArm.restart(); return }
                    readerRow.armBack = false
                    readerRow.rd.rollback(false)
                }
            }
            TrackTile
            {
                objectName: "readerUpdate"
                visible: readerRow.rd && readerRow.rd.available && readerRow.rd.candidateReady
                Layout.preferredWidth: 230
                Layout.preferredHeight: 34
                enabled: readerRow.rd && readerRow.rd.step !== 1
                label: readerRow.armUpdate ? qsTr("SURE? - TAP AGAIN")
                       : qsTr("UPDATE TO %1").arg(readerRow.rd ? readerRow.rd.candidate : "")
                active: true
                activeColor: readerRow.armUpdate ? "#F0C870" : "#E3B44F"
                onTapped:
                {
                    if (!readerRow.rd) return
                    if (readerRow.showOn) { readerRow.rd.update(true); return }
                    if (readerRow.armUpdate === false) { readerRow.armUpdate = true; readerRow.armBack = false; readerArm.restart(); return }
                    readerRow.armUpdate = false
                    readerRow.rd.update(false)
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

    // ------------------------------------------------- TRACK_APP_UI_R560: ROOM
    // Tobias 11-05: "byg ogsaa rummet i miniformat" - "kun et tab paa telefonen og saa i settings paa rigget". The room
    // map (rig_room.json) seen from above with the DJ at the bottom: every lamp lit as QLC+ sends it now
    // (trackEngine.roomNow(), read 4 times a second while it is open). Only reads - nothing here sends anything.
    Rectangle
    {
        id: roomPanel
        objectName: "roomPanel"
        visible: setupRoot.trackApp && setupRoot.roomOpen
        x: 0; y: 56; width: parent.width; height: parent.height - 56
        radius: setupRoot.radius
        z: 40
        color: setupRoot.color
        property var lamps: []
        function swatch(c)
        {
            switch (c)
            {
            case "white": return "#F2F2F5"
            case "red": return "#E0555A"
            case "blue": return "#3E6FE8"
            case "cyan": return "#3CC8D8"
            case "green": return "#30C050"
            case "magenta": return "#D040C0"
            case "yellow": return "#E0D030"
            case "orange": return "#E08030"
            case "purple": return "#8A4BE0"       // TRACK_APP_FIX_R567
            case "pink": return "#E0609A"
            case "uv": return "#7030C0"
            case "amber": return "#E0A040"
            }
            return "#F2F2F5"
        }
        readonly property color roomColour: swatch(trackEngine ? trackEngine.currentColour : "")
        Timer
        {
            interval: 250; repeat: true; triggeredOnStart: true
            running: roomPanel.visible && !!trackEngine && (trackEngine.roomMap !== undefined || trackEngine.roomNow !== undefined)
            onTriggered: roomPanel.lamps = trackEngine.roomMap !== undefined ? trackEngine.roomMap() : trackEngine.roomNow()   // TRACK_APP_ROOM_R563
        }
        Text
        {
            x: 12; y: 6
            text: qsTr("ROOM")
            color: "#8A8A94"; font.bold: true; font.pixelSize: 12; font.letterSpacing: 1.6
        }
        Text
        {
            x: 76; y: 6
            text: qsTr("seen from above, as the drawing - the DJ at the bottom, each lamp with its DMX address, lit as QLC+ lights it now")
            color: "#6E6E78"; font.pixelSize: 12
        }
        // TRACK_APP_ROOM_R563 (Tobias 11-06: "lav det efter tegningen vi har, og tilfoej gerne DMX adresser paa dem,
        // saa vi hurtigt kan slaa dem op"): the room as his drawing (its proportions, the wine shelf, the pillars,
        // the DJ booth), every lamp where it hangs with its DMX address; the list on the right finds one - a tap
        // on a row (or a lamp) rings it on the map
        Item
        {
            id: roomMap
            objectName: "roomMap"
            x: 12; y: 30; width: parent.width - 24; height: parent.height - 62
            property int pick: -1                         // the DMX address ringed (-1 none)
            readonly property var shape: roomPanel.visible && !!trackEngine && trackEngine.roomShape !== undefined ? trackEngine.roomShape() : ({})
            readonly property real aspect: shape.aspect !== undefined ? shape.aspect : 0.625
            readonly property real listW: Math.min(560, width * 0.36)
            readonly property real roomH: height - 24
            readonly property real roomW: Math.min(roomH * aspect, width - listW - 60)
            readonly property real ox: Math.max(10, (width - listW - 30 - roomW) / 2)
            readonly property real oy: 12
            function px(v) { return ox + v * roomW }
            function py(v) { return oy + (1 - v) * roomH }
            function kindName(k)
            {
                switch (k)
                {
                case "wash": return qsTr("WASH")
                case "strobe": return qsTr("STROBE")
                case "eyes": return qsTr("MINI 4 EYES")
                case "bar": return qsTr("LASER BAR")
                case "laser": return qsTr("ANIMATION LASER")
                case "haze": return qsTr("HAZE")
                }
                return k.toUpperCase()
            }
            // TRACK_APP_ROOM_R566: a lamp's size; its address goes right (left by the right wall) - or the other
            // side, or below, where another lamp sits in the way
            function lampW(L)
            {
                var k = L.k
                if (k === "strobe")
                    return L.r === "v" ? 26 : 44         // a strobe is long: across, or upright (82, 111)
                return k === "bar" ? 14 : (k === "wash" ? 28 : (k === "ball" ? 20 : (k === "laser" ? 18 : (k === "haze" ? 16 : 14))))
            }
            function lampH(L)
            {
                if (L.k === "strobe")
                    return L.r === "v" ? 44 : 26
                return L.k === "bar" ? Math.max(48, Math.min(roomH * 0.085, 96)) : lampW(L)
            }
            function belowX(lamp, tw)
            {
                // under the lamp, kept inside the walls
                return Math.max(ox + 3, Math.min(px(lamp.x) - tw / 2, ox + roomW - 3 - tw))
            }
            function labelSide(lamp, tw, th)
            {
                var X = px(lamp.x), Y = py(lamp.y), w = lampW(lamp), h = lampH(lamp)
                var tries = lamp.x > 0.80 ? [1, 2, 0] : [0, 1, 2]
                var all = roomPanel.lamps
                for (var t = 0; t < tries.length; t++)
                {
                    var s = tries[t]
                    var x0 = s === 0 ? X + w / 2 + 5 : (s === 1 ? X - w / 2 - 5 - tw : belowX(lamp, tw))
                    var y0 = s === 2 ? Y + h / 2 + 2 : Y - th / 2
                    if (x0 < ox + 2 || x0 + tw > ox + roomW - 2)
                        continue
                    var clash = false
                    for (var i = 0; i < all.length && !clash; i++)
                    {
                        var o = all[i]
                        if (o.dmx === lamp.dmx && o.x === lamp.x && o.y === lamp.y)
                            continue
                        var ow = lampW(o), oh = lampH(o), lx = px(o.x) - ow / 2, ly = py(o.y) - oh / 2
                        if (x0 < lx + ow && x0 + tw > lx && y0 < ly + oh && y0 + th > ly)
                            clash = true
                    }
                    if (!clash)
                        return s
                }
                return tries[0]
            }
            // the walls
            Rectangle
            {
                objectName: "roomWalls"
                x: roomMap.ox; y: roomMap.oy; width: roomMap.roomW; height: roomMap.roomH
                radius: 6; color: "#16171B"
                border.width: 2; border.color: "#4A4A54"
            }
            // the wine shelf and the pillars
            Rectangle
            {
                readonly property var b: roomMap.shape.shelf !== undefined ? roomMap.shape.shelf : [0, 0, 0, 0]
                x: roomMap.px(b[0]); y: roomMap.py(b[3]); width: (b[1] - b[0]) * roomMap.roomW; height: (b[3] - b[2]) * roomMap.roomH
                color: "#24262B"; border.width: 1; border.color: "#3A3C44"
                Text
                {
                    anchors.centerIn: parent; rotation: -90
                    text: qsTr("WINE SHELF"); color: "#5E606A"; font.bold: true; font.pixelSize: 12; font.letterSpacing: 2
                }
            }
            Repeater
            {
                model: roomMap.shape.pillars !== undefined ? roomMap.shape.pillars : []
                Rectangle
                {
                    readonly property var b: modelData
                    x: roomMap.px(b[0]); y: roomMap.py(b[3]); width: (b[1] - b[0]) * roomMap.roomW; height: (b[3] - b[2]) * roomMap.roomH
                    color: "#0A0A0C"; border.width: 1; border.color: "#33343A"
                }
            }
            // the DJ booth, against the front wall
            Item
            {
                // a dome on the front wall, as the drawing's arc
                readonly property var b: roomMap.shape.dj !== undefined ? roomMap.shape.dj : [0.236, 0.771, 0, 0.067]
                x: roomMap.px(b[0]); width: (b[1] - b[0]) * roomMap.roomW
                height: Math.max(24, (b[3] - b[2]) * roomMap.roomH)
                y: roomMap.py(0) - height
                clip: true
                Rectangle
                {
                    width: parent.width; height: 2 * parent.height
                    radius: Math.min(width, height) / 2
                    color: "#3A2F16"; border.width: 2; border.color: "#C9A24A"
                }
                Text
                {
                    anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 4
                    text: "DJ"; color: "#E3B44F"; font.bold: true; font.pixelSize: 13; font.letterSpacing: 2
                }
            }
            // the lamps, each with its DMX address
            Repeater
            {
                model: roomPanel.lamps.length                    // TRACK_APP_FIX_R567: a count - the items stay
                Item
                {
                    readonly property var lamp: roomPanel.lamps[index] || ({})
                    readonly property string k: lamp.k
                    readonly property real l: lamp.l
                    readonly property int dmx: lamp.dmx !== undefined ? lamp.dmx : 0
                    readonly property bool picked: dmx > 0 && roomMap.pick === dmx
                    readonly property color c: lamp.c !== "" ? lamp.c : roomPanel.roomColour
                    readonly property real w: roomMap.lampW(lamp)   // TRACK_APP_ROOM_R566
                    readonly property real h: roomMap.lampH(lamp)
                    readonly property var eyes: lamp.e !== undefined ? lamp.e : []
                    objectName: "roomLamp:" + dmx
                    x: roomMap.px(lamp.x) - w / 2
                    y: roomMap.py(lamp.y) - h / 2
                    width: w; height: h
                    z: picked ? 3 : 1
                    Rectangle
                    {
                        anchors.fill: parent
                        radius: parent.k === "strobe" || parent.k === "haze" ? 4 : (parent.k === "bar" ? 3 : (parent.k === "laser" ? 2 : width / 2))
                        rotation: parent.k === "laser" ? 45 : 0
                        color: parent.eyes.length > 0 ? "#1A1A20" : (parent.k === "ball" ? "transparent" : (parent.k === "haze" ? "#2A2C33" : (parent.l > 0.02 ? parent.c : "#2C2C35")))
                        opacity: parent.eyes.length > 0 || parent.k === "ball" || parent.l <= 0.02 ? 1.0 : 0.25 + 0.75 * parent.l
                        border.width: parent.eyes.length > 0 ? 1 : (parent.k === "ball" ? 2 : (parent.k === "haze" ? 1 : 0))
                        border.color: parent.eyes.length > 0 ? "#4A4A54" : "#8A8A94"
                    }
                    Column
                    {
                        // TRACK_APP_ROOM_R566: the bar's eight eyes ("Laser Color 1" at the top), each as it lights
                        id: eyeCol
                        objectName: "roomEyes:" + parent.dmx
                        visible: parent.eyes.length > 0
                        readonly property real lit: parent.l
                        x: 3; y: 3; width: parent.width - 6
                        spacing: 2
                        Repeater
                        {
                            model: eyeCol.visible ? eyeCol.parent.eyes.length : 0    // TRACK_APP_FIX_R567
                            Rectangle
                            {
                                readonly property string eye: eyeCol.parent.eyes[index] !== undefined ? eyeCol.parent.eyes[index] : ""
                                readonly property bool on: eye !== "" && eyeCol.lit > 0.02
                                width: eyeCol.width
                                height: Math.max(2, (eyeCol.parent.height - 6 - 2 * 7) / 8)
                                radius: 1.5
                                color: on ? (eye === "*" ? roomPanel.roomColour : swatch(eye)) : "#2C2C35"
                                opacity: on ? 0.35 + 0.65 * eyeCol.lit : 1.0
                            }
                        }
                    }
                    Rectangle
                    {
                        // found: a white ring
                        visible: parent.picked
                        anchors.centerIn: parent
                        width: Math.max(parent.w, parent.h) + 16; height: width; radius: width / 2
                        color: "transparent"; border.width: 3; border.color: "#FFFFFF"
                    }
                    Text
                    {
                        visible: parent.dmx > 0
                        readonly property int side: roomMap.labelSide(lamp, implicitWidth, implicitHeight)   // TRACK_APP_ROOM_R566
                        x: side === 1 ? -implicitWidth - 5 : (side === 0 ? parent.width + 5 : roomMap.belowX(lamp, implicitWidth) - parent.x)
                        y: side === 2 ? parent.height + 2 : (parent.height - implicitHeight) / 2
                        text: parent.dmx + (lamp.t !== undefined && lamp.t !== "" ? " · " + lamp.t : "")
                        color: parent.picked ? "#FFFFFF" : (parent.l > 0.02 ? "#C8C8D0" : "#7A7A84")
                        font.bold: parent.picked || parent.l > 0.02; font.pixelSize: 12
                    }
                    MouseArea
                    {
                        anchors.fill: parent; anchors.margins: -8
                        onClicked: roomMap.pick = (roomMap.pick === parent.dmx ? -1 : parent.dmx)
                    }
                }
            }
            // the list: every address, what it is, how lit now - a tap finds it on the map
            Rectangle
            {
                id: roomList
                objectName: "roomList"
                x: roomMap.width - roomMap.listW; y: 0; width: roomMap.listW; height: roomMap.height
                radius: 12; color: "#121216"
                readonly property var rows:
                {
                    var r = []
                    for (var i = 0; i < roomPanel.lamps.length; i++)
                        if (roomPanel.lamps[i].dmx > 0)
                            r.push(roomPanel.lamps[i])
                    r.sort(function(a, b) { return a.dmx - b.dmx })
                    return r
                }
                Text
                {
                    x: 16; y: 10
                    text: qsTr("DMX") + "      " + qsTr("LAMP") ; color: "#8A8A94"; font.bold: true; font.pixelSize: 12; font.letterSpacing: 1.4
                }
                Text
                {
                    anchors.right: parent.right; anchors.rightMargin: 16; y: 10
                    text: qsTr("NOW"); color: "#8A8A94"; font.bold: true; font.pixelSize: 12; font.letterSpacing: 1.4
                }
                Flickable
                {
                    x: 8; y: 34; width: parent.width - 16; height: parent.height - 42
                    contentHeight: listCol.implicitHeight; clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    Column
                    {
                        id: listCol
                        width: parent.width
                        spacing: 2
                        Repeater
                        {
                            model: roomList.rows.length                    // TRACK_APP_FIX_R567: a count - the rows stay
                            Rectangle
                            {
                                readonly property var rowData: roomList.rows[index] || ({})
                                objectName: "roomRow:" + rowData.dmx
                                readonly property bool picked: roomMap.pick === rowData.dmx
                                width: listCol.width; height: Math.max(26, Math.min(34, (roomList.height - 50) / Math.max(1, roomList.rows.length) - 2))
                                radius: 6
                                color: picked ? "#2E3A52" : (index % 2 ? "#17171C" : "transparent")
                                Text
                                {
                                    x: 8; anchors.verticalCenter: parent.verticalCenter; width: 56
                                    text: rowData.dmx; color: "#E6E6EA"; font.bold: true; font.pixelSize: 14
                                }
                                Text
                                {
                                    x: 66; anchors.verticalCenter: parent.verticalCenter
                                    text: roomMap.kindName(rowData.k) + (rowData.t !== "" && rowData.k !== "haze" ? "  ·  " + rowData.t : "")
                                          + (rowData.on === false ? "  ·  " + qsTr("not in the show") : "")
                                    color: rowData.on === false ? "#C07A5A" : "#A8A8B2"; font.bold: true; font.pixelSize: 12; font.letterSpacing: 0.6
                                }
                                Rectangle
                                {
                                    // how lit now (the lamp's colour)
                                    anchors.right: parent.right; anchors.rightMargin: 8; anchors.verticalCenter: parent.verticalCenter
                                    width: 64; height: 8; radius: 4; color: "#24242C"
                                    visible: rowData.k !== "haze"
                                    Rectangle
                                    {
                                        width: parent.width * Math.max(0, Math.min(1, rowData.l)); height: parent.height; radius: 4
                                        color: rowData.c !== "" ? rowData.c : roomPanel.roomColour
                                    }
                                }
                                MouseArea
                                {
                                    anchors.fill: parent
                                    onClicked: roomMap.pick = (roomMap.pick === rowData.dmx ? -1 : rowData.dmx)
                                }
                            }
                        }
                    }
                }
            }
        }
        Text
        {
            x: 14; y: parent.height - 26
            text: qsTr("dark = off  ·  bright = full  ·  tap an address or a lamp to find it  ·  AL1 = 203 down left by the DJ, AL2 = 219 up right at the back")   // TRACK_APP_ROOM_R564
            color: "#6E6E78"; font.pixelSize: 12
        }
    }
}
