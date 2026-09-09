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

    readonly property color cText: "#EEEEEE"
    readonly property color cDim:  "#9A9A9A"
    readonly property color cLine: "#555555"

    property string filter: ""
    property bool advancedOpen: false
    property bool cacheOpen: false
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
                text: qsTr("WHAT DOES EACH LOOK DO?")
                color: setupRoot.cText
                font.bold: true
                font.pixelSize: 15
            }

            Rectangle
            {
                Layout.preferredWidth: 200
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
                label: qsTr("FULL AUTO")
                active: trackEngine ? trackEngine.fullAuto : false
                activeColor: "#E3B44F"
                onTapped: if (trackEngine) trackEngine.fullAuto = !trackEngine.fullAuto
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

            TrackTile
            {
                Layout.preferredWidth: 110
                Layout.preferredHeight: 34
                label: qsTr("CLOSE")
                activeColor: "#E36B6B"
                active: true
                onTapped: trackViewRoot.setupOpen = false
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
                    TrackTile
                    {
                        Layout.preferredWidth: 190
                        Layout.preferredHeight: 28
                        label: qsTr("FORGET ALL AUTOMATIC")
                        activeColor: "#E36B6B"
                        onTapped: if (trackManager) trackManager.forgetAutomatic()
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

                    delegate: Rectangle
                    {
                        width: ListView.view.width
                        height: 42
                        color: "#1F1F1F"
                        radius: 3
                        // a model reset re-evaluates the bindings below while
                        // the row is already gone; without a fallback that is a
                        // TypeError per binding per row, on every reset.
                        property var md: modelData ? modelData
                                                   : ({ title: "", flags: 0, manual: false })

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
                text: qsTr("GROUPS") + "\n" + qsTr("tap: on / base / off")
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

                    // tap cycles ON -> BASE -> OFF. The base group is always lit;
                    // the others are effects added on top as the evening rises.
                    MouseArea
                    {
                        anchors.fill: parent
                        onClicked: if (trackEngine && groupTile.g.key) trackEngine.cycleGroup(groupTile.g.key)
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
                                                banned: false, rateUp: 0, rateDown: 0 })

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
                        visible: funcRow.rowRole >= 0
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

            Repeater
            {
                model: [ 16, 32, 64 ]
                TrackTile
                {
                    Layout.preferredWidth: 80
                    Layout.preferredHeight: 34
                    label: modelData + qsTr(" bars")
                    active: trackEngine ? trackEngine.holdBars === modelData : false
                    activeColor: "#4FA3E3"
                    onTapped: if (trackEngine) trackEngine.holdBars = modelData
                }
            }

            Item { Layout.preferredWidth: 20 }

            TrackTile
            {
                Layout.preferredWidth: 220
                Layout.preferredHeight: 34
                label: qsTr("Accent colour in drops")
                active: trackEngine ? trackEngine.accent : false
                activeColor: "#7ED07E"
                onTapped: if (trackEngine) trackEngine.accent = !trackEngine.accent
            }

            Item { Layout.preferredWidth: 20 }

            // the clock creeps the ENERGY slider up through the night; a
            // hand on the slider turns it off, this turns it back on
            TrackTile
            {
                Layout.preferredWidth: 180
                Layout.preferredHeight: 34
                label: qsTr("ENERGY by clock")
                active: trackEngine ? trackEngine.roomAuto : false
                activeColor: "#7ED07E"
                onTapped: if (trackEngine) trackEngine.roomAuto = !trackEngine.roomAuto
            }

            Item { Layout.preferredWidth: 20 }

            // every Track setting to / from Documents/QLC+/track-settings.json
            TrackTile
            {
                Layout.preferredWidth: 110
                Layout.preferredHeight: 34
                label: qsTr("EXPORT")
                onTapped: if (trackEngine) setupRoot.ioMessage = trackEngine.exportSettings()
            }
            TrackTile
            {
                Layout.preferredWidth: 110
                Layout.preferredHeight: 34
                label: qsTr("IMPORT")
                onTapped: if (trackEngine) setupRoot.ioMessage = trackEngine.importSettings()
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
}
