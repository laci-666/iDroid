import Foundation

/// Network diagnostic for the LocalDevVPN tunnel used by StikJIT.
///
/// StikJIT reaches the developer services via RSD/lockdown at `10.7.0.1`.
/// "VPN connected" in the UI is not enough: the route must actually exit via a
/// `utun*` interface with a source address in `10.7.0.0/16`, and `lockdownd`
/// (the classic port 62078) must accept TCP. This probe does exactly
/// the two checks, synchronously and with no protocol traffic:
/// 1) UDP `connect()` to `10.7.0.1` + `getsockname()` = the real source address,
///    apoi `getifaddrs()` ii gaseste interfata;
/// 2) non-blocking TCP connect, 400 ms, to `10.7.0.1:62078`.
/// Se apeleaza de pe un fir de fundal (Capabilitati / preflight JIT).
struct DNLocalDevVPNProbe {
    let sourceAddress: String?
    let interfaceName: String?
    let lockdowndReachable: Bool

    var exitsThroughVPN: Bool {
        guard let name = interfaceName else { return false }
        return name.hasPrefix("utun") || name.hasPrefix("ipsec") || name.hasPrefix("ppp")
    }

    var sourceIsLocalDevSubnet: Bool {
        sourceAddress?.hasPrefix("10.7.") == true
    }

    /// The practical condition in which StikJIT can talk to the device.
    var localDevVPNReady: Bool {
        exitsThroughVPN && sourceIsLocalDevSubnet && lockdowndReachable
    }

    var summary: String {
        if localDevVPNReady {
            return "LocalDevVPN active (\(interfaceName ?? "?"), \(sourceAddress ?? "?"), lockdownd responds)"
        }
        if sourceAddress == nil {
            return "no route to 10.7.0.1 — connect LocalDevVPN"
        }
        if !exitsThroughVPN {
            return "traffic to 10.7.0.1 exits via \(interfaceName ?? "?"), not the VPN — reconnect LocalDevVPN"
        }
        if !sourceIsLocalDevSubnet {
            return "VPN active, but the source is \(sourceAddress ?? "?") (not 10.7.x) — check the LocalDevVPN profile"
        }
        return "LocalDevVPN looks active, but lockdownd (10.7.0.1:62078) is not responding yet"
    }

    static func probe() -> DNLocalDevVPNProbe {
        let route = routeTowardLocalDevHost()
        return DNLocalDevVPNProbe(
            sourceAddress: route.address,
            interfaceName: route.interface,
            lockdowndReachable: tcpConnects(host: "10.7.0.1", port: 62078, timeoutMs: 400)
        )
    }

    // MARK: - Rutare

    private static func routeTowardLocalDevHost() -> (address: String?, interface: String?) {
        let fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else { return (nil, nil) }
        defer { close(fd) }

        var destination = sockaddr_in()
        destination.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        destination.sin_family = sa_family_t(AF_INET)
        destination.sin_port = in_port_t(62078).bigEndian
        destination.sin_addr = in_addr(s_addr: inet_addr("10.7.0.1"))

        let connected = withUnsafePointer(to: &destination) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddrPointer in
                connect(fd, sockaddrPointer, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard connected == 0 else { return (nil, nil) }

        var local = sockaddr_in()
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        let named = withUnsafeMutablePointer(to: &local) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddrPointer in
                getsockname(fd, sockaddrPointer, &length)
            }
        }
        guard named == 0, local.sin_addr.s_addr != 0 else { return (nil, nil) }

        let address = String(cString: inet_ntoa(local.sin_addr))
        return (address, interfaceName(forIPv4: local.sin_addr.s_addr))
    }

    private static func interfaceName(forIPv4 address: in_addr_t) -> String? {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let list else { return nil }
        defer { freeifaddrs(list) }

        var cursor: UnsafeMutablePointer<ifaddrs>? = list
        while let current = cursor {
            defer { cursor = current.pointee.ifa_next }
            guard let socketAddress = current.pointee.ifa_addr,
                  socketAddress.pointee.sa_family == sa_family_t(AF_INET) else { continue }
            let ipv4 = socketAddress.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee }
            if ipv4.sin_addr.s_addr == address, let name = current.pointee.ifa_name {
                return String(cString: name)
            }
        }
        return nil
    }

    // MARK: - TCP

    private static func tcpConnects(host: String, port: UInt16, timeoutMs: Int32) -> Bool {
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { return false }
        defer { close(fd) }

        let flags = fcntl(fd, F_GETFL, 0)
        guard flags >= 0, fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 else { return false }

        var destination = sockaddr_in()
        destination.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        destination.sin_family = sa_family_t(AF_INET)
        destination.sin_port = port.bigEndian
        destination.sin_addr = in_addr(s_addr: inet_addr(host))

        let result = withUnsafePointer(to: &destination) { pointer in
            pointer.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddrPointer in
                connect(fd, sockaddrPointer, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        if result == 0 { return true }
        guard errno == EINPROGRESS else { return false }

        var pollDescriptor = pollfd(fd: fd, events: Int16(POLLOUT), revents: 0)
        guard poll(&pollDescriptor, 1, timeoutMs) > 0, pollDescriptor.revents & Int16(POLLOUT) != 0 else {
            return false
        }
        var socketError: Int32 = 0
        var errorLength = socklen_t(MemoryLayout<Int32>.size)
        let queried = getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &errorLength)
        return queried == 0 && socketError == 0
    }
}
