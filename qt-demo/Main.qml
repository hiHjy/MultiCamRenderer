import QtQuick
import QtQuick.Controls

Window {
    width: 640
    height: 480
    visible: true
    // title: qsTr("Hello World")
    
    // Rectangle {
    //     width: 960
    //     height: 540
    //     color: "red"
    // }

    // Button {
    //     text:"我是按钮点我"
    //     x: 200
    //     y: 200
    //     anchors.centerIn: parent       
    // }
    MyItem {
        //text: "MyItem"
        width:640
        height:480
        x:0
        y:0
		
		Button {

			width: 30
			height: 50
			anchors.bottom: parent.bottom
			anchors.horizontalCenter: parent.horizontalCenter

			background: Rectangle {
				color: "red"
				radius: 4
				
			}
			opacity:0.2
			
			onClicked: {
				console.log("Button clicked!")
			}
		}
    }
}
