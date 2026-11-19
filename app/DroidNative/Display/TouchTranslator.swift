import UIKit

/// Linux input event (the multitouch subset Android needs).
struct LinuxInputEvent: Equatable {
    let type: UInt16   // EV_ABS=0x03, EV_KEY=0x01, EV_SYN=0x00
    let code: UInt16
    let value: Int32
    /// Timp monotonic (ns), comun tuturor evenimentelor din aceeasi miscare —
    /// without it, the guest cannot correctly correlate multitouch events
    /// intr-un singur raport (lectie S5LBox, docs/05 L7).
    let timeNs: UInt64
}

enum InputCode {
    static let absMtSlot: UInt16 = 0x2f        // ABS_MT_SLOT
    static let absMtTrackingID: UInt16 = 0x39  // ABS_MT_TRACKING_ID
    static let absMtPositionX: UInt16 = 0x35   // ABS_MT_POSITION_X
    static let absMtPositionY: UInt16 = 0x36   // ABS_MT_POSITION_Y
    static let absMtPressure: UInt16 = 0x3a    // ABS_MT_PRESSURE
    static let btnTouch: UInt16 = 0x14a        // BTN_TOUCH
    static let synReport: UInt16 = 0x00        // SYN_REPORT
    static let evSyn: UInt16 = 0x00
    static let evKey: UInt16 = 0x01
    static let evAbs: UInt16 = 0x03
}

/// Traduce UITouch -> protocolul multitouch Linux (tip B, cu sloturi),
/// consumed by the guest virtio-tablet device.
///
/// Geometry: the guest renders at scanout resolution (e.g. 1080x2400), the iOS
/// view displays aspectFit on the 2868x1320 screen. Coordinates are
/// normalizeaza mai intai in spatiul view-ului, corectand benzile letterbox,
/// then scaled to the guest's absolute axes.
final class TouchTranslator {
    var guestSize: CGSize = CGSize(width: 1080, height: 2400)
    var onEvents: ([LinuxInputEvent]) -> Void = { _ in }

    private var slots: [ObjectIdentifier: Int] = [:]   // UITouch -> slot 0..9
    private var nextTrackingID: Int32 = 1
    private var trackingIDs: [ObjectIdentifier: Int32] = [:]
    private var lastTimeNs: UInt64 = 0

    /// Acum monoton, cu clamp la ultimul timp emis (niciodata inapoi).
    private func monotonicNowNs() -> UInt64 {
        let now = DispatchTime.now().uptimeNanoseconds
        lastTimeNs = max(lastTimeNs, now)
        return lastTimeNs
    }

    func handle(touches: Set<UITouch>, phaseFor touch: UITouch, in view: UIView) {
        let batchTime = monotonicNowNs()
        var events: [LinuxInputEvent] = []
        func emit(_ type: UInt16, _ code: UInt16, _ value: Int32) {
            events.append(LinuxInputEvent(type: type, code: code, value: value, timeNs: batchTime))
        }

        for t in touches {
            let id = ObjectIdentifier(t)
            switch t.phase {
            case .began:
                let slot = (0..<10).first { s in !slots.values.contains(s) } ?? 0
                slots[id] = slot
                let tid = nextTrackingID; nextTrackingID += 1
                trackingIDs[id] = tid
                emit(InputCode.evAbs, InputCode.absMtSlot, Int32(slot))
                emit(InputCode.evAbs, InputCode.absMtTrackingID, tid)
                appendPosition(of: t, in: view, emit: emit)
                if slots.count == 1 { emit(InputCode.evKey, InputCode.btnTouch, 1) }
            case .moved, .stationary:
                guard let slot = slots[id] else { continue }
                emit(InputCode.evAbs, InputCode.absMtSlot, Int32(slot))
                appendPosition(of: t, in: view, emit: emit)
            case .ended, .cancelled:
                guard let slot = slots[id] else { continue }
                emit(InputCode.evAbs, InputCode.absMtSlot, Int32(slot))
                emit(InputCode.evAbs, InputCode.absMtTrackingID, -1)  // ridicare deget
                slots[id] = nil; trackingIDs[id] = nil
                if slots.isEmpty { emit(InputCode.evKey, InputCode.btnTouch, 0) }
            default:
                continue
            }
        }
        guard !events.isEmpty else { return }
        emit(InputCode.evSyn, InputCode.synReport, 0)
        onEvents(events)
    }

    private func appendPosition(of touch: UITouch, in view: UIView,
                                emit: (UInt16, UInt16, Int32) -> Void) {
        let p = touch.preciseLocation(in: view)
        let fitted = Self.aspectFitRect(content: guestSize, into: view.bounds.size)
        // Normalizare 0...1 in zona efectiv desenata, cu clamp la margini.
        let nx = min(1, max(0, (p.x - fitted.minX) / fitted.width))
        let ny = min(1, max(0, (p.y - fitted.minY) / fitted.height))
        emit(InputCode.evAbs, InputCode.absMtPositionX, Int32(nx * Double(guestSize.width - 1)))
        emit(InputCode.evAbs, InputCode.absMtPositionY, Int32(ny * Double(guestSize.height - 1)))
        // iOS offers no force on models without 3D Touch; Android accepts a constant.
        emit(InputCode.evAbs, InputCode.absMtPressure, 50)
    }

    static func aspectFitRect(content: CGSize, into container: CGSize) -> CGRect {
        guard content.width > 0, content.height > 0, container.width > 0, container.height > 0 else {
            return CGRect(origin: .zero, size: container)
        }
        let scale = min(container.width / content.width, container.height / content.height)
        let size = CGSize(width: content.width * scale, height: content.height * scale)
        return CGRect(x: (container.width - size.width) / 2,
                      y: (container.height - size.height) / 2,
                      width: size.width, height: size.height)
    }
}
