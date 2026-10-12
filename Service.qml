// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ManateeLazyCat

pragma ComponentBehavior: Bound
import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons as Commons
import "renderer/Geometry.js" as Position

// Only settings and IPC run inside Omarchy. The worker owns every window.
Item {
    id: root
    property var shell: null
    readonly property string pluginId: "io.github.manateelazycat.desktop-top"
    property var settings: ({})
    property var workerState: ({screens: [], gesturing: false})
    property var peer: null
    property bool unloading: false
    readonly property string workerPath: {
        var url = String(Qt.resolvedUrl("renderer/Worker.qml"));
        // Quickshell rewrites imports inside its config root to qs:@ URLs.
        if (url.indexOf("qs:@/qs/") === 0) return Quickshell.shellDir + "/" + url.slice(8);
        return decodeURIComponent(url.replace(/^file:\/\//, ""));
    }
    readonly property string modulePath: {
        var url = String(Qt.resolvedUrl("qml"));
        if (url.indexOf("qs:@/qs/") === 0) return Quickshell.shellDir + "/" + url.slice(8);
        return decodeURIComponent(url.replace(/^file:\/\//, ""));
    }
    readonly property string socketPath: Quickshell.env("XDG_RUNTIME_DIR")
        + "/desktop-top-" + Date.now() + "-" + Math.floor(Math.random() * 1000000) + ".sock"
    readonly property var palette: ({foreground: String(Commons.Color.foreground), background: String(Commons.Color.background),
        accent: String(Commons.Color.accent), fontFamily: Commons.Style.fontFamily})

    function configureWorker() {
        if (peer && peer.connected) peer.write(JSON.stringify({settings: settings, palette: palette}) + "\n");
    }
    function setMinimized(screenName, minimized) {
        if (!Quickshell.screens.some(function(s) { return s.name === screenName; })) return "Unknown screen.";
        if (!peer || !peer.connected) return "Desktop Top is not connected.";
        peer.write(JSON.stringify({type: "minimize", screen: screenName, minimized: minimized}) + "\n");
        return "ok";
    }
    function loadSettings(raw) {
        try {
            var entries = JSON.parse(raw).plugins || [];
            var next = entries.find(function(item) { return item.id === root.pluginId; }) || ({});
            if (JSON.stringify(next) !== JSON.stringify(settings)) settings = next;
        } catch (error) { console.warn("Desktop Top: invalid shell settings:", error); }
    }
    function save(settings) {
        if (shell && typeof shell.updateEntryInline === "function") {
            shell.updateEntryInline(pluginId, settings);
            root.settings = settings;
        }
    }
    function receive(data, socket) {
        try {
            var packet = JSON.parse(data);
            if (socket === peer && packet.type === "save") save(packet.settings);
            else if (socket === peer && packet.type === "state") workerState = packet.state;
        } catch (error) { console.warn("Desktop Top: invalid worker message:", error); }
    }
    onSettingsChanged: Qt.callLater(root.configureWorker)
    onPaletteChanged: Qt.callLater(root.configureWorker)
    FileView {
        path: (Quickshell.env("XDG_CONFIG_HOME") || (Quickshell.env("HOME") + "/.config")) + "/omarchy/shell.json"
        watchChanges: true
        printErrors: false
        onLoaded: root.loadSettings(text())
        onFileChanged: reload()
    }
    // SocketServer requires Quickshell's root reload hook, which third-party
    // Omarchy services created with createObject(null) never receive. The
    // standalone worker owns that server; this dynamic service is the client.
    Socket {
        id: connection
        path: root.socketPath
        parser: SplitParser { onRead: data => root.receive(data, connection) }
        onConnectedChanged: {
            root.peer = connected ? connection : null;
            if (connected) {
                retryConnection.stop();
                root.configureWorker();
            }
        }
        onError: {
            if (!root.unloading) retryConnection.restart();
        }
    }
    Timer {
        id: retryConnection
        interval: 250
        onTriggered: {
            if (worker.running && !root.unloading) {
                connection.connected = false;
                connection.connected = true;
            }
        }
    }
    Process {
        id: worker
        command: ["quickshell", "-p", root.workerPath, "--no-color"]
        environment: ({DESKTOP_TOP_SOCKET: root.socketPath, QT_QUICK_BACKEND: "software", QSG_RENDER_LOOP: "basic",
            QS_DISABLE_FILE_WATCHER: "1", QML_IMPORT_PATH: root.modulePath})
        // Also connect when qml.debug logging is disabled and the optional
        // ready marker is suppressed. Retries stop as soon as connected.
        onStarted: retryConnection.restart()
        stdout: SplitParser {
            onRead: data => {
                if (data.indexOf("DESKTOP_TOP_WORKER_READY") !== -1) connection.connected = true;
                else console.log("Desktop Top worker:", data);
            }
        }
        stderr: SplitParser { onRead: data => console.warn("Desktop Top worker:", data) }
        onExited: {
            retryConnection.stop();
            connection.connected = false;
            root.peer = null;
            root.workerState = {screens: [], gesturing: false};
            if (!root.unloading) restartTimer.restart();
        }
    }
    Timer { id: restartTimer; interval: 3000; onTriggered: worker.running = true }
    IpcHandler {
        target: "desktop-top"
        function status(): string {
            return JSON.stringify(Object.assign({}, root.workerState, {
                version: "0.1.4", renderer: "isolated-software", workerPid: worker.processId,
                connected: !!root.peer, positionX: Position.unit(root.settings.positionX, 0.64),
                positionY: Position.unit(root.settings.positionY, 0.52),
                accent: root.palette.accent, foreground: root.palette.foreground, background: root.palette.background
            }));
        }
        function setPosition(x: string, y: string): string {
            var px = Position.number(x, NaN), py = Position.number(y, NaN);
            if (!isFinite(px) || !isFinite(py) || px < 0 || px > 1 || py < 0 || py > 1)
                return "Position must be two numbers between 0 and 1.";
            if (root.workerState.gesturing) return "Desktop Top is being resized or moved.";
            root.save(Object.assign({}, root.settings, {positionX: px, positionY: py}));
            return "ok";
        }
        function resetPosition(): string { return setPosition("0.64", "0.52"); }
        function setSize(width: string, height: string): string {
            var w = Position.number(width, NaN), h = Position.number(height, NaN);
            if (!isFinite(w) || !isFinite(h) || w < 1 || h < 1)
                return "Size must be two positive numbers (logical pixels).";
            if (root.workerState.gesturing) return "Desktop Top is being resized or moved.";
            root.save(Object.assign({}, root.settings, {width: Math.round(w), height: Math.round(h)}));
            return "ok";
        }
        function resetSize(): string { return setSize("1000", "650"); }
        function minimize(screen: string): string { return root.setMinimized(screen, true); }
        function restore(screen: string): string { return root.setMinimized(screen, false); }
    }
    Component.onCompleted: worker.running = true
    Component.onDestruction: {
        root.unloading = true;
        restartTimer.stop();
        retryConnection.stop();
        connection.connected = false;
        worker.running = false;
    }
}
