pragma Singleton
import QtQuick

QtObject {
    readonly property int windowWidth: 1100
    readonly property int windowHeight: 720
    readonly property int pageMargin: 20
    readonly property int itemSpacing: 12
    readonly property int trackHeight: 76
    readonly property int trackHeaderWidth: 150
    readonly property int clipInset: 4
    readonly property int clipMinimumWidth: 18
    readonly property int borderWidth: 1
    readonly property color windowBackground: "#0a0a0a"
    readonly property color surfaceBackground: "#161616"
    readonly property color alternateTrackBackground: "#1e1e1e"
    readonly property color border: "#29ffffff"
    readonly property color primaryText: "#ffffff"
    readonly property color secondaryText: "#ccffffff"
    readonly property color clipBackground: "#1d5878"
    readonly property color clipBorder: "#000000"
    readonly property color errorText: "#e54f4f"
    readonly property color warningText: "#ff9500"
}
