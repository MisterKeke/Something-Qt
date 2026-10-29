import QtQuick

// Small, font-independent line icons for the console chrome.
Canvas {
    id: root
    property string symbol: "chevron"
    property color ink: "#00cfff"
    implicitWidth: 24
    implicitHeight: 24
    Accessible.ignored: true
    onSymbolChanged: requestPaint()
    onInkChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.scale(width / 24, height / 24)
        ctx.strokeStyle = ink
        ctx.fillStyle = ink
        ctx.lineWidth = 1.9
        ctx.lineCap = "round"
        ctx.lineJoin = "round"
        function line(points) {
            ctx.beginPath()
            ctx.moveTo(points[0], points[1])
            for (let i = 2; i < points.length; i += 2)
                ctx.lineTo(points[i], points[i + 1])
            ctx.stroke()
        }
        if (symbol === "terminal") {
            line([4, 6, 10, 12, 4, 18])
            line([13, 19, 21, 19])
        } else if (symbol === "play") {
            ctx.beginPath()
            ctx.moveTo(7, 4)
            ctx.lineTo(20, 12)
            ctx.lineTo(7, 20)
            ctx.closePath()
            ctx.fill()
        } else if (symbol === "info") {
            ctx.beginPath()
            ctx.arc(12, 12, 9, 0, Math.PI * 2)
            ctx.stroke()
            line([12, 11, 12, 17])
            ctx.fillRect(11, 6, 2, 2)
        } else if (symbol === "book") {
            line([12, 6, 8, 4, 3, 4, 3, 19, 8, 19, 12, 21, 16, 19, 21, 19, 21, 4, 16, 4, 12, 6, 12, 21])
        } else if (symbol === "trash") {
            line([4, 6, 20, 6])
            line([9, 6, 9, 3, 15, 3, 15, 6])
            line([6, 6, 7, 21, 17, 21, 18, 6])
            line([10, 10, 10, 17])
            line([14, 10, 14, 17])
        } else if (symbol === "up" || symbol === "down") {
            const sign = symbol === "up" ? 1 : -1
            line([12, 12 + 7 * sign, 12, 12 - 7 * sign])
            line([7, 12 - 2 * sign, 12, 12 - 7 * sign, 17, 12 - 2 * sign])
        } else if (symbol === "back") {
            line([14, 5, 7, 12, 14, 19])
            line([7, 12, 21, 12])
        } else {
            line([9, 5, 16, 12, 9, 19])
        }
    }
}
