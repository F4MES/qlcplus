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
    // TRACK_GRIP_V2 (runde 315): a Layout reads this, not `width`
    implicitWidth: 32

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

    // TRACK_GRIP_V1 (runde 310): the Track page's fader, stood on its end.
    // A dark box with an edge, the level filled inside it 3 px in, ticks at
    // a quarter, a half and three quarters, and a raised grip with three
    // ridges where the level is (track_view_touch.qml, SliderGrip and
    // SliderTicks, runde 140: "hvordan gør vi så alle sliders faktisk viser
    // at det er sliders?"). The mode is still the colour of the fill.
    property real gripSize: Math.max(12, Math.min(18, slider.availableHeight * 0.12))

    background:
        Rectangle
        {
            y: slider.leftPadding
            x: slider.topPadding + slider.availableWidth / 2 - width / 2
            implicitHeight: slider.height
            width: slider.availableWidth
            height: slider.availableHeight
            radius: 4
            color: UISettings.vcBarBg
            border.width: 1
            border.color: UISettings.vcTileBorder

            // the fill reaches the middle of the grip, wherever the grip is
            Rectangle
            {
                x: 3
                width: parent.width - 6
                y: Math.min(parent.height - 3,
                            slider.visualPosition * (parent.height - slider.gripSize) + slider.gripSize / 2)
                height: Math.max(0, parent.height - 3 - y)
                radius: 3
                color: trackColor
            }

            // a quarter, a half, three quarters - short marks at both sides
            Repeater
            {
                model: [ 0.25, 0.5, 0.75 ]
                Item
                {
                    y: slider.gripSize / 2 + (1.0 - modelData) * (parent.height - slider.gripSize) - 1
                    width: parent.width
                    height: 2
                    Rectangle { x: 0; width: 6; height: 2; color: "#4A4A4A" }
                    Rectangle { x: parent.width - 6; width: 6; height: 2; color: "#4A4A4A" }
                }
            }
        }

    // the grip: raised, light, three ridges - brighter while it is held
    handle:
        Rectangle
        {
            y: slider.leftPadding + slider.visualPosition * (slider.availableHeight - height)
            x: slider.topPadding + 2
            implicitWidth: Math.max(0, slider.availableWidth - 4)   // TRACK_GRIP_V3: never wider than the track
            implicitHeight: slider.gripSize
            radius: 4
            color: slider.pressed ? "#FFFFFF" : Qt.lighter(slider.trackColor, 1.45)
            border.width: 1
            border.color: "#0E0E0E"

            Row
            {
                anchors.centerIn: parent
                spacing: 3
                Repeater
                {
                    model: 3
                    Rectangle { width: 2; height: Math.min(9, slider.gripSize - 6); radius: 1; color: "#1A1A1A"; opacity: 0.75 }
                }
            }
        }
}
