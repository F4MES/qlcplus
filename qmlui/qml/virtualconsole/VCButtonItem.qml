/*
  Q Light Controller Plus
  VCButtonItem.qml

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
import QtQuick.Layouts

import org.qlcplus.classes 1.0

VCWidgetItem
{
    id: buttonRoot
    property VCButton buttonObj: null

    property int btnState: buttonObj ? buttonObj.state : VCButton.Inactive
    property int btnAction: buttonObj ? buttonObj.actionType : VCButton.Toggle
    // green when a look is on, red when the button overrides or forces
    // LTP. Guarded: this used to throw on every load while buttonObj was null
    property string activeColor: buttonObj && (buttonObj.flashOverrides || buttonObj.flashForceLTP)
                                 ? "#FF6B6B" : UISettings.vcTileActive

    // A flat tile, like the Track page: no gradient, a thin edge, and the
    // widget's own colour kept as the fill so the operator's choices stand.
    radius: UISettings.vcRadius
    border.width: 1
    border.color: UISettings.vcTileBorder

    function checkActionType()
    {
        if (btnAction === VCButton.Flash)
            buttonIcon.source = "qrc:/flash.svg"
        else if (btnAction === VCButton.StopAll)
            buttonIcon.source = "qrc:/stopall.svg"
        else if (btnAction === VCButton.Blackout)
            buttonIcon.source = "qrc:/blackout.svg"
        else
            buttonIcon.source = ""
    }

    onBtnStateChanged:
    {
        if (btnState === VCButton.Inactive)
        {
            // activate the blink effect here
            blink.start()
        }
    }

    onButtonObjChanged:
    {
        setCommonProperties(buttonObj)
        setBgImageMargins(4)
        checkActionType()
    }
    onBtnActionChanged: checkActionType()

    Rectangle
    {
        id: activeBorder
        x: 1
        y: 1
        width: parent.width - 2
        height: parent.height - 2
        color: "transparent"
        // on = a bright edge all the way round, off = a quiet one. Nothing
        // else moves, so a wall of buttons reads at a glance
        border.width: btnState === VCButton.Inactive
                      ? 1 : screenPixelDensity * UISettings.scalingFactor * 1.1
        border.color: btnState === VCButton.Active ? activeColor
                      : btnState === VCButton.Monitoring ? UISettings.vcTileMonitoring
                      : UISettings.vcTileIdleEdge
        radius: UISettings.vcRadius - 1

        Rectangle
        {
            id: bodyBg
            x: 3
            y: 3
            width: parent.width - 6
            height: parent.height - 6
            radius: 2
            color: "transparent"
            clip: true

            ColorAnimation on color
            {
                id: blink
                from: "black"
                to: "transparent"
                duration: 250
                running: false
            }

            Text
            {
                x: 2
                z: 2
                width: parent.width - 4
                height: parent.height
                // one font, built in one place: QML refuses a file that sets
                // both 'font:' and 'font.bold:' on the same item
                font: UISettings.vcFont(buttonObj ? buttonObj.font : null, true)
                text: buttonObj ? buttonObj.caption : ""
                verticalAlignment: Text.AlignVCenter
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                lineHeight: 0.9
                elide: Text.ElideRight
                color: buttonObj ? buttonObj.foregroundColor : UISettings.vcTileText
            }

            Image
            {
                id: buttonIcon
                visible: buttonObj ? (buttonObj.actionType != VCButton.Toggle) : false
                x: parent.width - 23
                y: 3
                z: 1
                width: 20
                height: 20
                source: ""
                sourceSize: Qt.size(width, height)
            }
        }
    }

    // a finger on the tile is seen at once, whatever the function does next
    property bool tileHeld: false

    Rectangle
    {
        anchors.fill: parent
        radius: UISettings.vcRadius
        color: "white"
        opacity: buttonRoot.tileHeld ? 0.16 : 0
        visible: opacity > 0
        z: 5
    }

    MouseArea
    {
        id: btnMouse
        anchors.fill: parent
        onPressedChanged: buttonRoot.tileHeld = pressed
        enabled: buttonObj && !buttonObj.isDisabled
        onClicked:
        {
            if (virtualConsole.editMode)
                return;

            if (buttonObj.actionType === VCButton.Toggle || buttonObj.actionType === VCButton.Blackout || buttonObj.actionType === VCButton.Freeze || buttonObj.actionType === VCButton.Kill)
            {
                buttonObj.requestStateChange(btnState === VCButton.Active ? false : true)
            }
            else if (buttonObj.actionType !== VCButton.Flash)
            {
                buttonObj.requestStateChange(true)
                blink.start()
            }
        }
        onPressed:
        {
            if (virtualConsole.editMode)
                return;

            if (buttonObj.actionType === VCButton.Flash)
                buttonObj.requestStateChange(true)
        }
        onReleased:
        {
            if (virtualConsole.editMode)
                return;

            if (buttonObj.actionType === VCButton.Flash)
                buttonObj.requestStateChange(false)
        }
    }

    MultiPointTouchArea
    {
        anchors.fill: parent
        enabled: buttonObj && !buttonObj.isDisabled
        mouseEnabled: false
        maximumTouchPoints: 1

        onPressed:
        {
            buttonRoot.tileHeld = true          // the tile lights under the finger
            if (virtualConsole.editMode)
                return;

            if (buttonObj.actionType === VCButton.Flash)
                buttonObj.requestStateChange(true)
        }
        onReleased:
        {
            buttonRoot.tileHeld = false
            if (virtualConsole.editMode)
                return;

            if (buttonObj.actionType === VCButton.Flash)
                buttonObj.requestStateChange(false)
            else if (buttonObj.actionType === VCButton.Toggle || buttonObj.actionType === VCButton.Blackout || buttonObj.actionType === VCButton.Freeze || buttonObj.actionType === VCButton.Kill)
                buttonObj.requestStateChange(btnState === VCButton.Active ? false : true)
            else
            {
                buttonObj.requestStateChange(true)
                blink.start()
            }
        }
    }

    DropArea
    {
        id: dropArea
        anchors.fill: parent
        z: 2 // this area must be above the VCWidget resize controls
        keys: [ "function" ]

        onDropped:
        {
            // attach function here
            if (drag.source.hasOwnProperty("fromFunctionManager"))
                buttonObj.functionID = drag.source.itemsList[0]
        }

        states: [
            State
            {
                when: dropArea.containsDrag
                PropertyChanges
                {
                    target: buttonRoot
                    color: UISettings.activeDropArea
                }
            }
        ]
    }
}
