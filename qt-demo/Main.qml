pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Window {
	id: root
	width: 1280
	height: 800
	minimumWidth: 800
	minimumHeight: 500
	visible: true
	title: "MultiCam Renderer"
	color: "#1b1f25"
	property var demoCameraPaths: ["/dev/video12"]
	property int nextDemoCameraIndex: 0

	function addDemoCameraToNextTile() {
		if (nextDemoCameraIndex >= demoCameraPaths.length)
			return;

		var tile = tileRepeater.itemAt(nextDemoCameraIndex);
		if (!tile)
			return;

		var id = displayController.addLocalCam(demoCameraPaths[nextDemoCameraIndex]);
		if (id < 0)
			return;

		tile.videoItem.cameraId = id;
		displayController.startLocalCam(id);
		nextDemoCameraIndex += 1;
	}

	Component.onCompleted: {
		addDemoCameraToNextTile();
		addDemoCameraToNextTile();
	}

	// 1. 顶部工具栏
	Rectangle {
		id: topToolbar
		anchors.top: parent.top
		anchors.left: parent.left
		anchors.right: parent.right
		height: 52
		color: "#2b3038"
		border.color: "#454c57"
		border.width: 1

		Label {
			anchors.centerIn: parent
			text: "顶部工具栏"
			color: "#d8dde5"
		}
	}

	// 2. 左侧设备 / 列表区域
	Rectangle {
		id: leftSidebar
		anchors.top: topToolbar.bottom
		anchors.bottom: bottomToolbar.top
		anchors.left: parent.left
		width: 220
		color: "#252a31"
		border.color: "#454c57"
		border.width: 1

		Label {
			anchors.centerIn: parent
			text: "左侧区域"
			color: "#bfc6d0"
		}
	}

	// 3. 下部控制栏
	Rectangle {
		id: bottomToolbar
		anchors.left: parent.left
		anchors.right: parent.right
		anchors.bottom: parent.bottom
		height: 56
		color: "#2b3038"
		border.color: "#454c57"
		border.width: 1

		Row {
			anchors.centerIn: parent
			spacing: 8

			Button {
				text: "添加"
				onClicked: root.addDemoCameraToNextTile()
			}

			Button {
				text: "2 路"
				onClicked: videoArea.layoutMode = 2
			}
			Button {
				text: "4 宫格"
				onClicked: videoArea.layoutMode = 4
			}
			Button {
				text: "6 路"
				onClicked: videoArea.layoutMode = 6
			}
		}
	}
	DisplayController {
		id: displayController
	}

	// 4. 中部视频显示区域
	Rectangle {
		id: videoArea
		anchors.top: topToolbar.bottom
		anchors.bottom: bottomToolbar.top
		anchors.left: leftSidebar.right
		anchors.right: parent.right
		color: "#000000"
		border.color: "#454c57"
		border.width: 1

		property int layoutMode: 6  // 当前支持：2、4、6 路
		property int spacing: 4
		property var draggedTile: null
		property var dropTarget: null

		// 返回 slot（格子编号）在当前布局中的位置和大小。
		// 六路时 slot 0 占左上 2×2 格；其余五路各占一格。
		function geometryForSlot(slot) {
			var p = spacing;
			if (layoutMode === 2) {
				var halfWidth = (width - p * 3) / 2;
				return {
					x: p + slot * (halfWidth + p),
					y: p,
					width: halfWidth,
					height: height - p * 2
				};
			}
			if (layoutMode === 4) {
				var w2 = (width - p * 3) / 2;
				var h2 = (height - p * 3) / 2;
				return {
					x: p + (slot % 2) * (w2 + p),
					y: p + Math.floor(slot / 2) * (h2 + p),
					width: w2,
					height: h2
				};
			}

			var cellWidth = (width - p * 4) / 3;
			var cellHeight = (height - p * 4) / 3;
			if (slot === 0)
				return {
					x: p,
					y: p,
					width: cellWidth * 2 + p,
					height: cellHeight * 2 + p
				};
			if (slot === 1)
				return {
					x: p + 2 * (cellWidth + p),
					y: p,
					width: cellWidth,
					height: cellHeight
				};
			if (slot === 2)
				return {
					x: p + 2 * (cellWidth + p),
					y: p + cellHeight + p,
					width: cellWidth,
					height: cellHeight
				};
			return {
				x: p + (slot - 3) * (cellWidth + p),
				y: p + 2 * (cellHeight + p),
				width: cellWidth,
				height: cellHeight
			};
		}

		function tileAt(x, y, exceptTile) {
			for (var i = 0; i < tileRepeater.count; ++i) {
				var tile = tileRepeater.itemAt(i);
				if (tile !== exceptTile && tile.visible && x >= tile.x && x <= tile.x + tile.width && y >= tile.y && y <= tile.y + tile.height)
					return tile;
			}
			return null;
		}

		function swapTiles(first, second) {
			if (!first || !second || first === second)
				return;
			var oldSlot = first.slot;
			first.slot = second.slot;
			second.slot = oldSlot;
		}

		Repeater {
			id: tileRepeater
			model: 6

			delegate: Rectangle {
				id: tile
				required property int index
				property int cameraNumber: index + 1
				property int slot: index
				property alias videoItem: videoItem
				property real videoAspectRatio: 16 / 9
				property var targetGeometry: videoArea.geometryForSlot(slot)
				property bool isDropTarget: videoArea.dropTarget === tile

				visible: slot < videoArea.layoutMode
				x: targetGeometry.x
				y: targetGeometry.y
				width: targetGeometry.width
				height: targetGeometry.height
				color: videoArea.draggedTile === tile ? "#1f2933" : "#000000"
				border.width: isDropTarget ? 3 : 1
				border.color: isDropTarget ? "#35baf6" : "#59626f"
				z: videoArea.draggedTile === tile ? 1 : 0

				Behavior on x {
					NumberAnimation {
						duration: 260
						easing.type: Easing.InOutQuad
					}
				}
				Behavior on y {
					NumberAnimation {
						duration: 260
						easing.type: Easing.InOutQuad
					}
				}
				Behavior on width {
					NumberAnimation {
						duration: 260
						easing.type: Easing.InOutQuad
					}
				}
				Behavior on height {
					NumberAnimation {
						duration: 260
						easing.type: Easing.InOutQuad
					}
				}

				// tile 是布局分配到的格子；videoViewport 才是实际视频显示范围。
				// 无论格子是宽是窄，videoViewport 都保持 16:9 并居中，空余处为黑边。
				Rectangle {
					id: videoViewport
					anchors.centerIn: parent
					width: Math.min(tile.width, tile.height * tile.videoAspectRatio)
					height: width / tile.videoAspectRatio
					color: "black"
					clip: true

					MyItem {
						id: videoItem
						anchors.fill: parent
					}
				}

				MouseArea {
					anchors.fill: parent
					acceptedButtons: Qt.LeftButton
					cursorShape: pressed ? Qt.ClosedHandCursor : Qt.ArrowCursor
					onPressed: videoArea.draggedTile = tile
					onPositionChanged: function (mouse) {
						if (!pressed)
							return;
						var point = tile.mapToItem(videoArea, mouse.x, mouse.y);
						videoArea.dropTarget = videoArea.tileAt(point.x, point.y, tile);
					}
					onReleased: function (mouse) {
						var point = tile.mapToItem(videoArea, mouse.x, mouse.y);
						videoArea.swapTiles(tile, videoArea.tileAt(point.x, point.y, tile));
						videoArea.draggedTile = null;
						videoArea.dropTarget = null;
					}
					onCanceled: {
						videoArea.draggedTile = null;
						videoArea.dropTarget = null;
					}
				}
			}
		}
	}
}
