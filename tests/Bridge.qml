// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ManateeLazyCat

import QtQuick
import Quickshell
import Quickshell.Io
import qs.Commons as Commons

ShellRoot {
    id: test
    property int writes: 0
    property var service: null
    property var host: QtObject {
        function updateEntryInline(id, settings) { test.writes++; return true; }
    }
    Timer {
        interval: 150; running: true
        onTriggered: {
            var component = Qt.createComponent(Qt.resolvedUrl("Plugin/Service.qml"), Component.PreferSynchronous);
            test.service = component.createObject(null);
            test.service.shell = test.host;
        }
    }
    IpcHandler {
        target: "bridge-test"
        function theme(): void { Commons.Color.accent = "#f7768e"; Commons.Color.foreground = "#eeeeee"; }
        function info(): string {
            return JSON.stringify({writes: test.writes, state: test.service ? test.service.workerState : null,
                palette: {foreground: String(Commons.Color.foreground), background: String(Commons.Color.background),
                    accent: String(Commons.Color.accent), fontFamily: Commons.Style.fontFamily}});
        }
        function unload(): void { test.service.destroy(); test.service = null; }
        function quit(): void { Qt.quit(); }
    }
}
