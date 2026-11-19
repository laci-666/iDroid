import SwiftUI

@main
struct DroidNativeApp: App {
    @StateObject private var controller = VMController()
    @StateObject private var setup = GuestSetupManager()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(controller)
                .environmentObject(setup)
        }
        .onChange(of: scenePhase) { _, phase in
            // iOS nu ne lasa sa rulam VM-ul in background. La trecerea in
            // .background we save a snapshot; see VMController.handleBackground().
            if phase == .background { controller.handleBackground() }
            if phase == .active { controller.handleForeground() }
        }
    }
}
