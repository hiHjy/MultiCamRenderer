import QtQuick
import QtQuick.Controls

Window {
    width: 1280
    height: 720
    visible: true
    color: "black"

    property real tileGap: 16
    property real tileWidth: Math.min(600, (width - tileGap * 4) / 3)
    property real tileHeight: tileWidth * 3 / 4

    Row {
        anchors.centerIn: parent
        spacing: tileGap

        Repeater {
            model: 3

            delegate: Item {
                width: tileWidth
                height: tileHeight
                clip: true

                MyItem {
                    anchors.fill: parent
                }

                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    border.width: 2
                    border.color: index === 0 ? '#fd3ff892' : index === 1 ? "#4da3ff" : "#ffcc33"
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 10
                    width: 96
                    height: 34
                    color: "#99000000"
                    radius: 4

                    Text {
                        anchors.centerIn: parent
                        color: "white"
                        text: "view " + (index + 1)
                        font.pixelSize: 16
                    }
                }

                Button {
                    width: 54
                    height: 34
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 10
                    text: "OK"
                    opacity: 0.75

                    onClicked: {
                        console.log("Button clicked on view", index + 1)
                    }
                }
            }
        }
    }
}
