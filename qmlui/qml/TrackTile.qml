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

    // TRACK_APP_STYLE_R557: the TRACK app's tiles as the Track page's - flat, round, bold capitals
    readonly property bool appStyle: typeof trackAppCtl !== "undefined" && !!trackAppCtl && trackAppCtl.active === true
    readonly property color idle: appStyle ? "#2A2A33" : "#3A3A3A"
    readonly property bool lightFill: 0.299 * activeColor.r + 0.587 * activeColor.g + 0.114 * activeColor.b > 0.5

    radius: appStyle ? 10 : 3
    color: tileMouse.pressed ? Qt.lighter(active ? activeColor : idle, 1.35)
                             : (active ? activeColor : idle)
    border.width: appStyle ? 0 : 1
    border.color: active ? Qt.lighter(activeColor, 1.3) : "#555555"
    opacity: enabled ? 1.0 : (appStyle ? 0.35 : 1.0)

    Text
    {
        id: tileText
        anchors.centerIn: parent
        // a long label shrinks to fit (down to 9 px) before it is cut
        width: tileRoot.appStyle ? Math.max(0, tileRoot.width - 12) : implicitWidth
        horizontalAlignment: Text.AlignHCenter
        fontSizeMode: tileRoot.appStyle ? Text.HorizontalFit : Text.FixedSize
        minimumPixelSize: 9
        elide: tileRoot.appStyle ? Text.ElideRight : Text.ElideNone
        color: tileRoot.active ? (tileRoot.appStyle && !tileRoot.lightFill ? "#FFFFFF" : "#101010")
                               : (tileRoot.appStyle ? "#D6D6DC" : "#EEEEEE")
        font.bold: tileRoot.active || tileRoot.appStyle
        font.pixelSize: tileRoot.appStyle ? 12 : 13
        font.letterSpacing: tileRoot.appStyle ? 0.7 : 0
        font.capitalization: tileRoot.appStyle ? Font.AllUppercase : Font.MixedCase
    }

    MouseArea
    {
        id: tileMouse
        anchors.fill: parent
        onClicked: tileRoot.tapped()
    }
}
