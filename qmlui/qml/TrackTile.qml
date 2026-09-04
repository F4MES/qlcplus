/*
  Q Light Controller Plus
  TrackTile.qml

  A big touch button for the Track page. Plain Rectangle, no Controls theme,
  so it stays dark no matter what the platform style does.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt
*/

import QtQuick

Rectangle
{
    id: tileRoot

    property alias label: tileText.text
    property bool active: false
    property color activeColor: "#4A4A4A"
    signal tapped()

    radius: 3
    color: active ? activeColor : "#3A3A3A"
    border.width: 1
    border.color: active ? Qt.lighter(activeColor, 1.3) : "#555555"

    Text
    {
        id: tileText
        anchors.centerIn: parent
        color: tileRoot.active ? "#101010" : "#EEEEEE"
        font.bold: tileRoot.active
        font.pixelSize: 13
    }

    MouseArea
    {
        anchors.fill: parent
        onClicked: tileRoot.tapped()
    }
}
