// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ManateeLazyCat

#pragma once
#include <cstddef>

// Bounds CSI parameter counts before bytes reach libvterm.
//
// libvterm up to and including 0.3.3 increments its CSI argument index on
// every ';' and ':' - parser.c:229-235 rewrites a colon to a semicolon and
// falls through to the same argi++ - without checking CSI_ARGS_MAX
// (src/parser.c:233-235), so a sequence with more than 16 parameters writes
// past the long args[16] that ends the parser union and lands on the
// callback pointers stored behind it (src/vterm_internal.h:210-233). btop
// renders process argv without escaping control characters in tree view
// (src/btop_draw.cpp:2098-2101), so another local user can inject such a
// sequence through /proc/<pid>/cmdline and corrupt this process.
//
// This filter mirrors libvterm's ESC and CSI state machine (parser.c lines
// 143-259) in UTF-8 mode, as configured by Terminal. In particular, pending
// ESC state survives NUL, DEL, other C0 controls and bytes >= 0x80, exactly
// as libvterm keeps its in_esc flag, and escape intermediates are tracked so
// that a following '[' completes the escape instead of opening a CSI.
// Getting this wrong would let an injected sequence enter CSI in libvterm
// while the filter is in the ground state, leaving its parameter count
// unbounded.
//
// The only bytes the filter ever drops are the parameter separators that
// would open the seventeenth parameter. Everything else - text, UTF-8,
// control bytes, DCS/OSC payloads - is forwarded unchanged, so normal
// terminal output is never altered.
//
// Bytes are emitted or dropped immediately, so output never exceeds input
// and push() may filter in place. State persists across calls because
// sequences straddle PTY read boundaries.
class CsiFilter {
public:
    // Filters |data| in place and returns the number of leading bytes that
    // are safe to hand to vterm_input_write().
    size_t push(char *data, size_t length);

    // Returns to the ground state, e.g. when the child is restarted and the
    // previous stream ended mid-sequence.
    void reset();

private:
    enum class State {
        Ground,             // parser NORMAL, no pending ESC
        Escape,             // pending ESC, no intermediates yet
        EscapeIntermediate, // pending ESC with intermediate bytes 0x20-0x2f
        CsiLeader,          // after ESC '[': leader bytes 0x3c-0x3f
        CsiArgs,            // CSI parameter bytes
        CsiIntermed,        // CSI intermediate bytes 0x20-0x2f
        DcsCommand,         // after ESC 'P': DCS command bytes
        OscCommand,         // after ESC ']': OSC command number
        String,             // OSC/DCS/APC/PM/SOS payload
        StringEscape,       // pending ESC inside a string state
    };
    State m_state = State::Ground;
    int m_separators = 0; // ';' and ':' separators seen in the current CSI
};
