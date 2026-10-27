// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ManateeLazyCat

#include "CsiFilter.h"

namespace {

constexpr unsigned char kEsc = 0x1b;
constexpr unsigned char kCan = 0x18;
constexpr unsigned char kSub = 0x1a;

// CSI_ARGS_MAX from libvterm's vterm_internal.h. The parser writes
// args[++argi] on every ';' and on every ':' - a colon is rewritten to a
// semicolon and falls through to the same unguarded increment
// (parser.c:229-235) - so 16 parameters stay in bounds and the separator
// that would open a seventeenth is dropped instead.
constexpr int kMaxParams = 16;

bool isLeaderByte(unsigned char c) { return c >= 0x3c && c <= 0x3f; }
bool isIntermediate(unsigned char c) { return c >= 0x20 && c <= 0x2f; }
bool isCsiFinalByte(unsigned char c) { return c >= 0x40 && c <= 0x7e; }

// libvterm ignores NUL and DEL and executes other C0 controls inline without
// leaving the current parser state (parser.c:143-180).
bool isIgnoredControl(unsigned char c) {
    return c == 0x00 || c == 0x7f || c < 0x20;
}

} // namespace

void CsiFilter::reset() {
    m_state = State::Ground;
    m_separators = 0;
}

size_t CsiFilter::push(char *data, size_t length) {
    size_t out = 0;
    for (size_t in = 0; in < length; ++in) {
        const unsigned char c = static_cast<unsigned char>(data[in]);
        bool emit = true;
        switch (m_state) {
        case State::Ground:
            // Everything passes; an ESC may begin a sequence.
            if (c == kEsc) m_state = State::Escape;
            break;
        case State::Escape:
            // CAN/SUB cancel a pending escape; a bare ESC re-arms it
            // (parser.c:152-162).
            if (c == kEsc) break;
            if (c == kCan || c == kSub) { m_state = State::Ground; break; }
            if (isIgnoredControl(c)) break;
            if (isIntermediate(c)) { m_state = State::EscapeIntermediate; break; }
            if (c == '[') {
                // Hoisted to C1 CSI (parser.c:185-196).
                m_state = State::CsiLeader;
                m_separators = 0;
                break;
            }
            if (c >= 0x40 && c <= 0x5f) {
                // Remaining ESC letters in this range become C1 controls.
                if (c == 'P') m_state = State::DcsCommand;
                else if (c == ']') m_state = State::OscCommand;
                else if (c == 'X' || c == '^' || c == '_') m_state = State::String;
                else m_state = State::Ground;
                break;
            }
            if (c >= 0x30 && c <= 0x7e) {
                // 0x30-0x3f and 0x60-0x7e are two-character escapes.
                m_state = State::Ground;
            }
            // Bytes >= 0x80 leave in_esc armed (parser.c:316).
            break;
        case State::EscapeIntermediate:
            // Once ESC has intermediates, '[' completes the escape rather
            // than opening a CSI (parser.c:185-196).
            if (c == kEsc) { m_state = State::Escape; break; }
            if (c == kCan || c == kSub) { m_state = State::Ground; break; }
            if (isIgnoredControl(c)) break;
            if (isIntermediate(c)) break;
            if (c >= 0x30 && c <= 0x7e) m_state = State::Ground;
            // Bytes >= 0x80 keep in_esc armed.
            break;
        case State::CsiLeader:
            if (c == kEsc) { m_state = State::Escape; break; }
            if (c == kCan || c == kSub) { m_state = State::Ground; break; }
            if (isIgnoredControl(c)) break;
            if (isLeaderByte(c)) break;
            // The first non-leader byte ends the leader and is reprocessed
            // as a parameter byte (parser.c:204-217).
            m_state = State::CsiArgs;
            m_separators = 0;
            [[fallthrough]];
        case State::CsiArgs:
            if (c == kEsc) { m_state = State::Escape; break; }
            if (c == kCan || c == kSub) { m_state = State::Ground; break; }
            if (isIgnoredControl(c)) break;
            if (c == ';' || c == ':') {
                // libvterm rewrites ':' to ';' and falls through to the same
                // argi++ write (parser.c:229-235), so both count identically.
                // Dropping the separator keeps argi at the cap; later
                // separators are dropped the same way.
                if (m_separators >= kMaxParams - 1) emit = false;
                else ++m_separators;
                break;
            }
            if (c >= '0' && c <= '9') break;
            if (isLeaderByte(c)) { m_state = State::Ground; break; }
            if (isIntermediate(c)) { m_state = State::CsiIntermed; break; }
            if (isCsiFinalByte(c)) { m_state = State::Ground; break; }
            // Any other byte invalidates the sequence (parser.c:240-258).
            m_state = State::Ground;
            break;
        case State::CsiIntermed:
            if (c == kEsc) { m_state = State::Escape; break; }
            if (c == kCan || c == kSub) { m_state = State::Ground; break; }
            if (isIgnoredControl(c)) break;
            if (isIntermediate(c)) break;
            // A final byte dispatches, anything else is invalid; both resume
            // in the ground state (parser.c:243-258).
            m_state = State::Ground;
            break;
        case State::DcsCommand:
            // DCS_COMMAND is not a string state: ESC returns to NORMAL with
            // in_esc armed, while C0 controls are executed inline.
            if (c == kEsc) { m_state = State::Escape; break; }
            if (c == kCan || c == kSub) { m_state = State::Ground; break; }
            if (isIgnoredControl(c)) break;
            if (isCsiFinalByte(c)) m_state = State::String; // DCS payload
            // Every other byte stays in the bounded command array.
            break;
        case State::OscCommand:
            // OSC_COMMAND is a string state: ESC keeps it armed, BEL ends it.
            if (c == kEsc) { m_state = State::StringEscape; break; }
            if (c == kCan || c == kSub) { m_state = State::Ground; break; }
            if (isIgnoredControl(c)) {
                if (c == 0x07) m_state = State::Ground; // BEL acts as ST
                break;
            }
            if (c >= '0' && c <= '9') break;
            // ';' or any other byte starts the OSC payload (parser.c:261-281).
            m_state = State::String;
            break;
        case State::String:
            if (c == kEsc) { m_state = State::StringEscape; break; }
            if (c == kCan || c == kSub) { m_state = State::Ground; break; }
            if (isIgnoredControl(c)) {
                if (c == 0x07) m_state = State::Ground; // BEL acts as ST
                break;
            }
            // Payload bytes are unbounded strings inside libvterm.
            break;
        case State::StringEscape:
            if (c == kEsc) break; // re-arm, still inside the string
            if (c == kCan || c == kSub) { m_state = State::Ground; break; }
            if (c == 0x00 || c == 0x7f) break;
            if (c == '\\') { m_state = State::Ground; break; } // ST terminates
            if (c < 0x20) {
                // Non-terminating C0 controls execute inline while libvterm
                // retains both the string state and its pending in_esc flag
                // (parser.c:158-166). BEL terminates the string; because the
                // parser still processes the pending ESC first, it leaves the
                // parser in NORMAL with in_esc armed.
                if (c == 0x07) m_state = State::Escape;
                break;
            }
            if (isIntermediate(c)) { m_state = State::EscapeIntermediate; break; }
            if (c >= 0x30 && c <= 0x7e) {
                // The escape completes; '[' is an escape final here, not CSI.
                m_state = State::Ground;
                break;
            }
            // Bytes >= 0x80 leave ESC armed in NORMAL.
            m_state = State::Escape;
            break;
        }
        if (emit) data[out++] = data[in];
    }
    return out;
}
