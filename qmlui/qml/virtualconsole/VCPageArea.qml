/*
  Q Light Controller Plus
  VCPageArea.qml

  Copyright (c) Massimo Callegari

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0.txt

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

import QtQuick
import QtQuick.Controls

Rectangle
{
    anchors.fill: parent
    color: "transparent"

    property int page: -1

    // Auto-stretch the page to fill the available area when NOT editing
    // (operate/live/kiosk mode). In edit mode the normal behaviour is kept.
    property bool autoStretch: virtualConsole.editMode === false

    onPageChanged: virtualConsole.renderPage(vcPage, pageWrapper, page)

    Flickable
    {
        id: vcPage
        objectName: "vcPage" + page
        anchors.fill: parent
        boundsBehavior: Flickable.StopAtBounds
        // nothing to scroll when the page is stretched to fill the screen
        interactive: autoStretch === false

        // Declared before the page wrapper so the widgets stay on top and
        // still receive clicks; empty areas fall through to deselect.
        MouseArea
        {
            anchors.fill: parent
            onClicked: virtualConsole.resetWidgetSelection()
        }

        // The VC page is rendered into this wrapper. The Scale transform
        // stretches the whole page (all widgets) to fill the area in operate
        // mode, non-uniformly so it always fills 100% of width and height.
        Item
        {
            id: pageWrapper
            width: vcPage.contentWidth
            height: vcPage.contentHeight
            transform: Scale
            {
                origin.x: 0
                origin.y: 0
                xScale: (autoStretch && vcPage.contentWidth > 0) ? vcPage.width / vcPage.contentWidth : 1
                yScale: (autoStretch && vcPage.contentHeight > 0) ? vcPage.height / vcPage.contentHeight : 1
            }
        }

        ScrollBar.vertical: CustomScrollBar { }
        ScrollBar.horizontal : CustomScrollBar { orientation: Qt.Horizontal }
    }
}
