/*
  Q Light Controller Plus
  QLCPlusFader.qml

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
import QtQuick.Controls.Basic

import "."

Slider
{
    id: slider
    width: 32
    height: 100
    orientation: Qt.Vertical
    from: 0
    to: 255
    stepSize: 1.0
    wheelEnabled: true

    property Gradient handleGradient: defaultGradient
    property Gradient handleGradientHover: defaultGradientHover
    property color trackColor: defaultTrackColor

    property color defaultTrackColor: UISettings.vcBarFill
    property Gradient defaultGradient:
        Gradient
        {
            GradientStop { position: 0; color: "#ccc" }
            GradientStop { position: 0.45; color: "#555" }
            GradientStop { position: 0.50; color: "#000" }
            GradientStop { position: 0.55; color: "#555" }
            GradientStop { position: 1.0; color: "#888" }
        }

    property Gradient defaultGradientHover:
        Gradient
        {
            GradientStop { position: 0; color: "#eee" }
            GradientStop { position: 0.45; color: "#999" }
            GradientStop { position: 0.50; color: "red" }
            GradientStop { position: 0.55; color: "#999" }
            GradientStop { position: 1.0; color: "#ccc" }
        }

    // A bar that fills from the bottom, with a lit top edge - the Track
    // page's language. The old groove was a thin blue line with grey painted
    // over the unused part, which reads backwards on a dark desk.
    background:
        Rectangle
        {
            y: slider.leftPadding
            x: slider.topPadding + slider.availableWidth / 2 - width / 2
            implicitHeight: slider.height
            width: slider.availableWidth
            height: slider.availableHeight
            radius: UISettings.vcRadius - 1
            color: UISettings.vcBarBg
            border.width: 1
            border.color: UISettings.vcTileBorder

            Rectangle
            {
                x: 1
                width: parent.width - 2
                y: 1 + slider.visualPosition * (parent.height - 2)
                height: parent.height - 2 - slider.visualPosition * (parent.height - 2)
                radius: parent.radius
                color: trackColor

                // the lit top edge: where the level is, seen from across a room
                Rectangle
                {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 2
                    color: UISettings.vcBarEdge
                    visible: parent.height > 3
                }
            }
        }

    // No grip: the Track page draws a line where the level is and lets
    // the filled body do the rest. Dragging anywhere on the fader still works.
    handle:
        Rectangle
        {
            y: slider.leftPadding + slider.visualPosition * (slider.availableHeight - height)
            x: slider.topPadding
            implicitWidth: slider.availableWidth
            implicitHeight: Math.max(3, slider.height * 0.012)
            color: slider.pressed ? "#FFFFFF" : UISettings.vcBarEdge
            radius: 1
        }
}
