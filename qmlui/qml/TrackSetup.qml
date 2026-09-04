/*
  Q Light Controller Plus
  TrackSetup.qml

  Role picker for the Track page.

  One row per function, and one tap to say what that function DOES. No
  dropdowns, no per-section grid to fill in: the engine works out the rest.
  The advanced fold at the bottom is for pinning a role inside one section
  when the automatic choice is not what you want.

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
    id: setupRoot
    color: "#262626"
    radius: 4

    readonly property color cBtn:   "#3A3A3A"
    readonly property color cBtnHi: "#4A4A4A"
    readonly property color cText:  "#EEEEEE"
    readonly property color cDim:   "#9A9A9A"
    readonly property color cLine:  "#555555"

    property var states: [ "normal", "break", "build", "drop" ]
    property string filter: ""
    property bool advancedOpen: false
    property int refresh: 0

    // colours mirror the marker colours on the waveform
    function roleColor(role)
    {
        switch (role)
        {
        case 0: return "#4FA3E3"   // colour
        case 1: return "#7ED07E"   // movement
        case 2: return "#E3B44F"   // beams
        case 3: return "#E36B6B"   // strobe
        case 4: return "#9A7ED0"   // ambient
        }
        return "#5A5A5A"
    }

    function roleShort(role)
    {
        switch (role)
        {
        case 0: return qsTr("COLOUR")
        case 1: return qsTr("MOVE")
        case 2: return qsTr("BEAM")
        case 3: return qsTr("STROBE")
        case 4: return qsTr("AMBIENT")
        }
        return "–"
    }

    Component.onCompleted:
    {
        // fill in anything the operator has not placed, so the page works
        // straight away on a project it has never seen
        if (trackManager)
            trackManager.autoAssignRoles(false)
    }

    Connections
    {
        target: trackManager
        function onLooksChanged() { setupRoot.refresh++ }
    }

    // ------------------------------------------------------------------ tile
    component Tile: Rectangle
    {
        property alias label: tileText.text
        property bool active: false
        property color activeColor: "#4A4A4A"
        signal tapped()

        radius: 3
        color: active ? activeColor : setupRoot.cBtn
        border.width: 1
        border.color: active ? Qt.lighter(activeColor, 1.3) : setupRoot.cLine

        Text
        {
            id: tileText
            anchors.centerIn: parent
            color: parent.active ? "#101010" : setupRoot.cText
            font.bold: parent.active
            font.pixelSize: 13
        }

        MouseArea
        {
            anchors.fill: parent
            onClicked: parent.tapped()
        }
    }

    ColumnLayout
    {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 8

        // ------------------------------------------------- header
        RowLayout
        {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
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
                Layout.preferredWidth: 220
                Layout.preferredHeight: 36
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

            Tile
            {
                Layout.preferredWidth: 110
                Layout.preferredHeight: 36
                label: qsTr("RE-GUESS")
                onTapped: if (trackManager) trackManager.autoAssignRoles(true)
            }

            Tile
            {
                Layout.preferredWidth: 130
                Layout.preferredHeight: 36
                label: setupRoot.advancedOpen ? qsTr("HIDE ADVANCED")
                                              : qsTr("ADVANCED")
                active: setupRoot.advancedOpen
                onTapped: setupRoot.advancedOpen = !setupRoot.advancedOpen
            }
        }

        // ------------------------------------------------- role legend
        RowLayout
        {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            spacing: 6

            Repeater
            {
                model: trackManager ? trackManager.roleCount : 0

                Rectangle
                {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#1B1B1B"
                    radius: 3
                    border.width: 1
                    border.color: setupRoot.roleColor(index)

                    Text
                    {
                        anchors.fill: parent
                        anchors.margins: 4
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        text: trackManager
                              ? trackManager.roleHint(index) : ""
                        color: setupRoot.cDim
                        font.pixelSize: 10
                    }
                }
            }

            Item { Layout.preferredWidth: 64 }
        }

        // ------------------------------------------------- function rows
        ListView
        {
            id: funcList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 4

            model:
            {
                setupRoot.refresh    // re-evaluate when roles change
                if (!trackManager)
                    return []

                var all = trackManager.roleTable()
                if (setupRoot.filter === "")
                    return all

                var out = []
                for (var i = 0; i < all.length; i++)
                    if (all[i].name.toLowerCase().indexOf(setupRoot.filter) >= 0
                        || all[i].path.toLowerCase().indexOf(setupRoot.filter) >= 0)
                        out.push(all[i])
                return out
            }

            delegate: Rectangle
            {
                id: funcRow
                width: funcList.width
                height: 46
                color: "#1F1F1F"
                radius: 3

                // held here so the role tiles below can reach them without
                // walking up through the layout
                property int rowRole: modelData.role
                property var rowId: modelData.id

                RowLayout
                {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 4

                    ColumnLayout
                    {
                        Layout.preferredWidth: 200
                        spacing: 0

                        Text
                        {
                            Layout.fillWidth: true
                            text: modelData.name
                            color: setupRoot.cText
                            font.pixelSize: 14
                            elide: Text.ElideRight
                        }
                        Text
                        {
                            Layout.fillWidth: true
                            visible: modelData.path.length > 0
                            text: modelData.path
                            color: "#6A6A6A"
                            font.pixelSize: 10
                            elide: Text.ElideRight
                        }
                    }

                    Repeater
                    {
                        model: trackManager ? trackManager.roleCount : 0

                        Tile
                        {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            label: setupRoot.roleShort(index)
                            active: funcRow.rowRole === index
                            activeColor: setupRoot.roleColor(index)
                            onTapped: trackManager.assignRole(funcRow.rowId, index)
                        }
                    }

                    Tile
                    {
                        Layout.preferredWidth: 60
                        Layout.fillHeight: true
                        label: "–"
                        active: funcRow.rowRole < 0
                        onTapped: trackManager.assignRole(funcRow.rowId, -1)
                    }
                }
            }
        }

        // ------------------------------------------------- advanced
        Rectangle
        {
            Layout.fillWidth: true
            Layout.preferredHeight: setupRoot.advancedOpen ? 230 : 0
            visible: setupRoot.advancedOpen
            color: "#1B1B1B"
            radius: 3

            Flickable
            {
                anchors.fill: parent
                anchors.margins: 8
                contentHeight: advCol.height
                clip: true

                Column
                {
                    id: advCol
                    width: parent.width
                    spacing: 6

                    Text
                    {
                        text: qsTr("Pin a role inside one section. Leave on AUTO "
                                   + "to let the engine choose.")
                        color: setupRoot.cDim
                        font.pixelSize: 12
                    }

                    Repeater
                    {
                        model: setupRoot.states

                        Column
                        {
                            id: secCol
                            width: advCol.width
                            spacing: 3

                            property string secName: modelData

                            Text
                            {
                                text: secCol.secName.toUpperCase()
                                color: setupRoot.cText
                                font.bold: true
                                font.pixelSize: 13
                            }

                            Repeater
                            {
                                model: trackManager ? trackManager.roleCount : 0

                                Row
                                {
                                    id: advRow
                                    spacing: 5
                                    property int roleIdx: index
                                    property string secName: secCol.secName

                                    Text
                                    {
                                        width: 90
                                        height: 30
                                        verticalAlignment: Text.AlignVCenter
                                        text: trackManager
                                              ? trackManager.roleName(advRow.roleIdx) : ""
                                        color: setupRoot.roleColor(advRow.roleIdx)
                                        font.pixelSize: 12
                                    }

                                    Tile
                                    {
                                        width: 60
                                        height: 30
                                        label: active ? qsTr("ON") : qsTr("OFF")
                                        active:
                                        {
                                            setupRoot.refresh
                                            return trackManager
                                                   ? trackManager.roleEnabled(
                                                       advRow.secName, advRow.roleIdx)
                                                   : true
                                        }
                                        activeColor: "#7ED07E"
                                        onTapped: trackManager.setRoleEnabled(
                                                      advRow.secName, advRow.roleIdx,
                                                      !active)
                                    }

                                    Tile
                                    {
                                        width: 200
                                        height: 30
                                        label:
                                        {
                                            setupRoot.refresh
                                            if (!trackManager)
                                                return qsTr("AUTO")
                                            var fid = trackManager.forcedRole(
                                                          advRow.secName, advRow.roleIdx)
                                            var list = trackManager.roleFunctions(
                                                           advRow.roleIdx)
                                            for (var i = 0; i < list.length; i++)
                                                if (list[i].id === fid)
                                                    return list[i].name
                                            return qsTr("AUTO")
                                        }
                                        active: label !== qsTr("AUTO")
                                        activeColor: setupRoot.roleColor(advRow.roleIdx)

                                        // step through this role's functions,
                                        // wrapping back around to AUTO
                                        onTapped:
                                        {
                                            var list = trackManager.roleFunctions(
                                                           advRow.roleIdx)
                                            if (list.length === 0)
                                                return

                                            var fid = trackManager.forcedRole(
                                                          advRow.secName, advRow.roleIdx)
                                            var at = -1
                                            for (var i = 0; i < list.length; i++)
                                                if (list[i].id === fid)
                                                    at = i

                                            if (at + 1 >= list.length)
                                                trackManager.setForcedRole(
                                                    advRow.secName, advRow.roleIdx, 0)
                                            else
                                                trackManager.setForcedRole(
                                                    advRow.secName, advRow.roleIdx,
                                                    list[at + 1].id)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // ------------------------------------------------- running
        Text
        {
            Layout.fillWidth: true
            text: qsTr("Running") + ": "
                  + (trackManager ? trackManager.roleReport() : "")
            color: setupRoot.cDim
            font.pixelSize: 13
            elide: Text.ElideRight
        }
    }
}
