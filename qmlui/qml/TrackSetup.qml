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
    property int refresh: 0

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

    Component.onCompleted:
    {
        // guess anything not placed yet, so the page works on a project it
        // has never seen
        if (trackEngine)
            trackEngine.autoAssign(false)
    }

    Connections
    {
        target: trackEngine
        function onTableChanged() { setupRoot.refresh++ }
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
                onTapped: trackEngine.showAll = !trackEngine.showAll
            }

            TrackTile
            {
                Layout.preferredWidth: 100
                Layout.preferredHeight: 34
                label: qsTr("RE-GUESS")
                onTapped: if (trackEngine) trackEngine.autoAssign(true)
            }

            TrackTile
            {
                Layout.preferredWidth: 120
                Layout.preferredHeight: 34
                label: setupRoot.advancedOpen ? qsTr("HIDE ADVANCED") : qsTr("ADVANCED")
                active: setupRoot.advancedOpen
                onTapped: setupRoot.advancedOpen = !setupRoot.advancedOpen
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
                model:
                {
                    setupRoot.refresh
                    return trackEngine ? trackEngine.groups : []
                }

                Rectangle
                {
                    id: groupTile
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 3
                    color: modelData.base ? "#2A3F55" : (modelData.enabled ? "#333333" : "#1F1F1F")
                    border.width: modelData.base ? 2 : 1
                    border.color: modelData.base ? "#4FA3E3" : (modelData.enabled ? "#666666" : "#333333")

                    Column
                    {
                        anchors.centerIn: parent
                        spacing: 1

                        Text
                        {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: modelData.key
                                  + (modelData.base ? "  · BASE" : (modelData.enabled ? "" : "  · OFF"))
                            color: modelData.enabled ? setupRoot.cText : "#666666"
                            font.bold: true
                            font.pixelSize: 12
                        }
                        Text
                        {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: modelData.fixtures + " fx · "
                                  + modelData.colours + " col · "
                                  + modelData.motions + " mot · "
                                  + (modelData.dimmer ? qsTr("dimmer") : qsTr("NO DIMMER"))
                            color: modelData.dimmer ? setupRoot.cDim : "#E36B6B"
                            font.pixelSize: 10
                        }
                    }

                    // tap cycles ON -> BASE -> OFF. The base group is always lit;
                    // the others are effects added on top as the evening rises.
                    MouseArea
                    {
                        anchors.fill: parent
                        onClicked: trackEngine.cycleGroup(modelData.key)
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
                setupRoot.refresh
                if (!trackEngine)
                    return []

                var all = trackEngine.table()
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
                color: modelData.hidden ? "#181818" : "#1F1F1F"
                radius: 3

                property int rowRole: modelData.role
                property int rowStars: modelData.stars
                property var rowId: modelData.id

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
                        color: setupRoot.swatch(modelData.colour)
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
                            text: modelData.name
                            color: modelData.hidden ? "#888888" : setupRoot.cText
                            font.pixelSize: 14
                            elide: Text.ElideRight
                        }
                        Text
                        {
                            Layout.fillWidth: true
                            text: modelData.group
                                  + (modelData.path.length > 0 ? "   ·   " + modelData.path : "")
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
                            onTapped: trackEngine.assignRole(funcRow.rowId, index)
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
                                onTapped: trackEngine.setStars(funcRow.rowId, index + 1)
                            }
                        }
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
                        onTapped: trackEngine.assignRole(funcRow.rowId, -1)
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
                    onTapped: trackEngine.holdBars = modelData
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
                onTapped: trackEngine.accent = !trackEngine.accent
            }

            Item { Layout.fillWidth: true }
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
