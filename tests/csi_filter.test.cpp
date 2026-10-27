// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ManateeLazyCat
//
// Standalone regression test for CsiFilter; no Qt required.
//
//   g++ -std=c++17 -Wall -Wextra -Werror -o /tmp/csi_filter.test tests/csi_filter.test.cpp native/CsiFilter.cpp
//   /tmp/csi_filter.test
//
// Add -DCSI_FILTER_WITH_VTERM and link libvterm to exercise the real parser;
// link an ASan/UBSan build of libvterm to detect writes before its callbacks.
//
// The oracle below re-derives libvterm 0.3.3's argument accumulation
// (src/parser.c lines 139-376) line by line: a raw over-long CSI must trip
// it, and the filter's output must never trip it.

#include "../native/CsiFilter.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#ifdef CSI_FILTER_WITH_VTERM
#include <vterm.h>
#endif

// Models libvterm 0.3.3 with utf8 mode on, as the plugin configures it:
// raw C1 bytes never reach the state machine, only ESC-hoisted ones do.
// Counts writes to args[index] with index > 15, which is the OOB.
struct LibvtermMirror {
    enum St { NORMAL, CSI_LEADER, CSI_ARGS, CSI_INTERMED, DCS_COMMAND, OSC_COMMAND, STRING };
    St st = NORMAL;
    bool in_esc = false;
    int intermedlen = 0;
    int argi = 0;
    long oobWrites = 0;
    long dispatches = 0;

    static bool is_intermed(unsigned char c) { return c >= 0x20 && c <= 0x2f; }
    static bool is_string_state(St st) { return st == OSC_COMMAND || st == STRING; }

    void feed(const char *bytes, size_t len) {
        for (size_t pos = 0; pos < len; pos++) {
            unsigned char c = static_cast<unsigned char>(bytes[pos]);
            bool c1_allowed = false; // !utf8 mode

            if (c == 0x00 || c == 0x7f) continue; // parser.c:143, state kept
            if (c == 0x18 || c == 0x1a) { in_esc = false; st = NORMAL; continue; } // 152
            if (c == 0x1b) { // 159: string states keep their state
                intermedlen = 0;
                if (!is_string_state(st)) st = NORMAL;
                in_esc = true;
                continue;
            }
            const bool bel_terminates = c == 0x07 && is_string_state(st); // 166
            if (c < 0x20 && !bel_terminates) continue; // 170: do_control, state kept
            if (bel_terminates) { st = NORMAL; continue; } // 299 via fallthrough

            if (in_esc) { // 185
                if (!intermedlen && c >= 0x40 && c < 0x60 && (!is_string_state(st) || c == 0x5c)) {
                    c = static_cast<unsigned char>(c + 0x40);
                    c1_allowed = true;
                    in_esc = false;
                } else {
                    st = NORMAL; // 197-200
                }
            }

            switch (st) {
            case CSI_LEADER: // 204
                if (c >= 0x3c && c <= 0x3f) break;
                argi = 0; // 215-217
                st = CSI_ARGS;
                [[fallthrough]];
            case CSI_ARGS: // 220
                if (c >= '0' && c <= '9') break; // 222
                // parser.c:229-235: ':' sets CSI_ARG_FLAG_MORE, is rewritten
                // to ';', and falls through to the same unguarded argi++
                // write, so both separators count identically.
                if (c == ':' || c == ';') { // 229-235: the unguarded write
                    argi++;
                    if (argi > 15) oobWrites++;
                    break;
                }
                argi++; // 240
                intermedlen = 0;
                st = CSI_INTERMED;
                [[fallthrough]];
            case CSI_INTERMED: // 243
                if (is_intermed(c)) { // 244, sequence continues
                    if (intermedlen < 15) intermedlen++;
                    break;
                }
                if (c >= 0x40 && c <= 0x7e) dispatches++; // 252: do_csi
                st = NORMAL; // 258: dispatch or invalid, both resume
                break;
            case DCS_COMMAND: // 283
                if (c >= 0x40 && c <= 0x7e) st = STRING;
                break;
            case OSC_COMMAND: // 261
                if (c == 0x07 || (c1_allowed && c == 0x9c)) { st = NORMAL; break; } // via 299
                if (c >= '0' && c <= '9') break;
                st = STRING; // 271-281
                [[fallthrough]];
            case STRING: // 294
                if (c == 0x07 || (c1_allowed && c == 0x9c)) st = NORMAL;
                break;
            case NORMAL: // 305
                if (in_esc) {
                    if (is_intermed(c)) { // 307
                        if (intermedlen < 15) intermedlen++;
                    } else if (c >= 0x30 && c < 0x7f) { // 311: do_escape
                        in_esc = false;
                    }
                    break; // other bytes leave in_esc armed (316)
                }
                if (c1_allowed && c >= 0x80 && c < 0xa0) { // 321
                    if (c == 0x9b) st = CSI_LEADER;
                    else if (c == 0x90) st = DCS_COMMAND;
                    else if (c == 0x98 || c == 0x9d || c == 0x9e || c == 0x9f) st = STRING;
                }
                break;
            }
        }
    }
};

static std::string filter(const std::string &input, const std::vector<size_t> &chunks = {}) {
    CsiFilter f;
    std::string out;
    if (chunks.empty()) {
        std::string buffer = input;
        const size_t n = f.push(buffer.data(), buffer.size());
        out.assign(buffer, 0, n);
    } else {
        size_t offset = 0;
        for (const size_t chunk : chunks) {
            const size_t take = std::min(chunk, input.size() - offset);
            std::string piece = input.substr(offset, take);
            const size_t n = f.push(piece.data(), piece.size());
            out.append(piece, 0, n);
            offset += take;
            if (offset >= input.size()) break;
        }
        if (offset < input.size()) {
            std::string piece = input.substr(offset);
            const size_t n = f.push(piece.data(), piece.size());
            out.append(piece, 0, n);
        }
    }
    return out;
}

static void expect(const std::string &got, const std::string &want, const char *what) {
    if (got != want) {
        std::fprintf(stderr, "FAIL %s:\n  got  %zu bytes\n  want %zu bytes\n", what, got.size(), want.size());
        std::exit(1);
    }
}

// Feeds the filtered bytes through the real libvterm parser. The callback
// reports how many CSI arguments reached it: the parser only writes past its
// fixed array when a separator arrives at argi == 15, after which the final
// byte reports argcount > 16. Build libvterm with ASan to catch the write
// itself rather than relying on this counter.
#ifdef CSI_FILTER_WITH_VTERM
struct CsiProbe {
    int maxArgCount = 0;
    int csiCount = 0;
};

static int probeCsi(const char *, const long *, int argcount, const char *, char, void *user) {
    CsiProbe *probe = static_cast<CsiProbe *>(user);
    if (argcount > probe->maxArgCount) probe->maxArgCount = argcount;
    ++probe->csiCount;
    return 1;
}

static void checkRealLibvterm(const std::string &filtered, const char *what) {
    VTerm *vt = vterm_new(25, 80);
    assert(vt);
    vterm_set_utf8(vt, 1);
    CsiProbe probe;
    VTermParserCallbacks callbacks = {};
    callbacks.csi = probeCsi;
    vterm_parser_set_callbacks(vt, &callbacks, &probe);
    vterm_input_write(vt, filtered.data(), filtered.size());
    vterm_free(vt);
    if (probe.maxArgCount > 16) {
        std::fprintf(stderr, "FAIL %s: libvterm saw %d CSI arguments\n", what, probe.maxArgCount);
        std::exit(1);
    }
}
#endif

static void checkSafe(const std::string &input, const char *what) {
    const std::string out = filter(input);
    LibvtermMirror m;
    m.feed(out.data(), out.size());
    if (m.oobWrites != 0) {
        std::fprintf(stderr, "FAIL %s: filtered output still overflows\n", what);
        std::exit(1);
    }
#ifdef CSI_FILTER_WITH_VTERM
    checkRealLibvterm(out, what);
#endif
}

int main() {
    // Sanity: the oracle must model the bug it guards against.
    {
        LibvtermMirror m;
        const std::string raw = "\x1b[" + std::string(20, ';') + "m";
        m.feed(raw.data(), raw.size());
        assert(m.oobWrites > 0);
    }

    // Benign sequences pass through byte-identically.
    const char *benign[] = {
        "\x1b[0;1;38;2;10;20;30;48;5;196m",   // truecolor + indexed, 10 params
        "\x1b[?25l\x1b[?1049h\x1b[2J\x1b[H",
        "\x1b[12;34H\x1b[38:2:255:0:0m",      // colon sub-parameters
        "\x1b]0;title with ; and [ chars\x07", // OSC payload: ';' is string content
        "\x1bP$q~\x1b\\",                      // DCS with terminator
        "\x1b(B\x1bM\x1b=",                    // two-character escapes
        "\x1b[1\t2;3m",                        // C0 executed inside CSI
        "\xe8\xbf\x9b\xe7\xa8\x8b \xf0\x9f\x90\xa2 libre\xc3\xa9", // UTF-8 text
        "\x1b[?;5h\x1b[?;h",                   // leader then separators
        "\x1b[!p\x1b[;m\x1b[0m",
        "\x1b]0;title\x1b[" "\x07",            // ESC '[' inside OSC is an escape final
        "\x1bPq;payload;with;semicolons\x1b\\",
        "\x1b]8;;https://example.test/a;b;c\x07", // OSC 8 hyperlink
    };
    for (const char *sequence : benign) {
        const std::string input(sequence);
        expect(filter(input), input, "benign passthrough");
        LibvtermMirror m;
        m.feed(input.data(), input.size());
        assert(m.oobWrites == 0);
        checkSafe(input, "benign passthrough");
    }

    // Over-long CSIs collapse to 16 parameters and keep their final byte.
    for (int n = 0; n <= 64; ++n) {
        const std::string input = "\x1b[" + std::string(size_t(n), ';') + "m";
        const std::string out = filter(input);
        if (n <= 15) {
            expect(out, input, "16 params stay intact");
        } else {
            expect(out, "\x1b[" + std::string(15, ';') + "m", "17+ params truncate");
        }
        LibvtermMirror m;
        m.feed(out.data(), out.size());
        assert(m.oobWrites == 0);
        if (n >= 16) {
            LibvtermMirror raw;
            raw.feed(input.data(), input.size());
            assert(raw.oobWrites > 0);
        }
        checkSafe(input, "semicolon parameter cap");
    }

    // Regression for issue #10102 comment 5995705972: ':' increments the
    // same fixed index, so colon runs must clamp exactly like semicolons.
    for (int n = 0; n <= 64; ++n) {
        const std::string input = "\x1b[" + std::string(size_t(n), ':') + "m";
        const std::string out = filter(input);
        if (n <= 15) {
            expect(out, input, "16 colon params stay intact");
        } else {
            expect(out, "\x1b[" + std::string(15, ':') + "m", "17+ colon params truncate");
        }
        LibvtermMirror m;
        m.feed(out.data(), out.size());
        assert(m.oobWrites == 0);
        if (n >= 16) {
            LibvtermMirror raw;
            raw.feed(input.data(), input.size());
            assert(raw.oobWrites > 0);
        }
        checkSafe(input, "colon parameter cap");
    }

    // A ':' before any parameter byte still increments argi because
    // CSI_LEADER falls through to CSI_ARGS, so ':' + 15 ';' + digits
    // reached the out-of-bounds write before colons were counted.
    {
        const std::string input = "\x1b[:" + std::string(15, ';') + "1m";
        LibvtermMirror raw;
        raw.feed(input.data(), input.size());
        assert(raw.oobWrites > 0);
        const std::string out = filter(input);
        LibvtermMirror m;
        m.feed(out.data(), out.size());
        assert(m.oobWrites == 0);
        expect(out, "\x1b[:" + std::string(14, ';') + "1m", "leader colon counts toward the limit");
        checkSafe(input, "leader colon");
    }

    // Regression for issue #10336 comment 6333298078: libvterm keeps in_esc
    // across BEL (and other non-cancelling controls), so the filter must not
    // lose the pending ESC and miss the CSI opened by the following '['.
    {
        const std::string prefixes[] = {
            std::string("\x1b\x07[", 3),  // BEL
            std::string("\x1b\x00[", 3),  // NUL
            std::string("\x1b\x7f[", 3),  // DEL
            std::string("\x1b\x09[", 3),  // TAB
            std::string("\x1b\x0a[", 3),  // LF
            std::string("\x1b\x80[", 3),  // high byte
            std::string("\x1b\x9b[", 3),  // raw C1 is a text byte in UTF-8 mode
            std::string("\x1b\x1b\x07[", 4), // ESC re-arms, then BEL
        };
        for (const std::string &prefix : prefixes) {
            const std::string input = prefix + std::string(20, ';') + "m";
            LibvtermMirror raw;
            raw.feed(input.data(), input.size());
            assert(raw.oobWrites > 0);
            const std::string out = filter(input);
            expect(out, prefix + std::string(15, ';') + "m",
                   "pending ESC survives control/high byte");
            checkSafe(input, "pending ESC survives control/high byte");
        }
    }

    // ESC intermediates must suppress CSI hoisting: '[' completes the escape
    // instead of opening a sequence, so the following separators are text.
    {
        const char *prefixes[] = {
            "\x1b![",   // intermediate 0x21
            "\x1b(0[",  // two intermediates, then '[' as escape final
        };
        for (const char *prefix : prefixes) {
            const std::string input = std::string(prefix) + std::string(20, ';') + "m";
            expect(filter(input), input, "escape intermediate blocks CSI");
            LibvtermMirror raw;
            raw.feed(input.data(), input.size());
            assert(raw.oobWrites == 0);
            checkSafe(input, "escape intermediate blocks CSI");
        }
    }

    // CAN/SUB cancel a pending escape; the following '[' is plain text.
    {
        const char *prefixes[] = { "\x1b\x18[", "\x1b\x1a[" };
        for (const char *prefix : prefixes) {
            const std::string input = std::string(prefix) + std::string(20, ';') + "m";
            expect(filter(input), input, "CAN/SUB cancels pending escape");
            LibvtermMirror raw;
            raw.feed(input.data(), input.size());
            assert(raw.oobWrites == 0);
            checkSafe(input, "CAN/SUB cancels pending escape");
        }
    }

    // String states never expose a CSI argument array; payload separators and
    // an embedded ESC '[' must pass through untouched.
    {
        const std::string strings[] = {
            std::string("\x1b]0;") + std::string(40, ';') + "\x07",
            std::string("\x1b]0;title\x1b[") + std::string(40, ';') + "m\x07",
            std::string("\x1bPq") + std::string(40, ';') + "\x1b\\",
            "\x1bP1;2;3;4qpayload\x1b\\",
            "\x1bXpayload\x1b\\",
            "\x1b^payload\x1b\\",
            "\x1b_payload\x1b\\",
        };
        for (const std::string &input : strings) {
            expect(filter(input), input, "string payload passthrough");
            LibvtermMirror raw;
            raw.feed(input.data(), input.size());
            assert(raw.oobWrites == 0);
            checkSafe(input, "string payload passthrough");
        }
    }

    // Regression for issue #10336 comment 6402925863: a non-terminating C0
    // after ESC inside OSC must preserve both OSC and the parser's pending ESC.
    // The following ESC '[' is consequently still OSC content, while a later
    // fresh CSI is bounded normally. Exercise every split around the state
    // transition because PTY reads can divide this sequence at any byte.
    {
        const std::string input = std::string("\x1b]0;title\x1b\x01\x1b[") +
                                  std::string(20, ';') + "\x07" +
                                  "\x1b[" + std::string(20, ';') + "m";
        const std::string expected = std::string("\x1b]0;title\x1b\x01\x1b[") +
                                     std::string(20, ';') + "\x07" +
                                     "\x1b[" + std::string(15, ';') + "m";
        expect(filter(input), expected, "OSC and pending ESC survive C0");
        LibvtermMirror raw;
        raw.feed(input.data(), input.size());
        assert(raw.oobWrites > 0); // the final, unfiltered CSI is intentionally overlong
        checkSafe(input, "OSC and pending ESC survive C0");
        for (size_t split = 1; split < input.size(); ++split) {
            expect(filter(input, {split}), expected, "OSC C0 split boundary");
        }
    }

    // A long run of separators with no final byte stays bounded and a later
    // sequence starts fresh.
    {
        const std::string input = "\x1b[" + std::string(200, ';') + "\x1b[0;1;38;2;1;2;3m";
        const std::string out = filter(input);
        LibvtermMirror m;
        m.feed(out.data(), out.size());
        assert(m.oobWrites == 0);
        checkSafe(input, "unterminated over-long CSI");
    }

    // Chunk boundaries anywhere in the stream give the same result.
    {
        const std::string input = "\x1b\x07[" + std::string(40, ';') + "4m" + benign[0] +
                                  "\x1b[" + std::string(40, ':') + "m" +
                                  "\x1b]0;title\x1b[" + std::string(40, ';') + "\x07";
        const std::string whole = filter(input);
        for (size_t size = 1; size <= 7; ++size) {
            std::vector<size_t> chunks(input.size(), size);
            expect(filter(input, chunks), whole, "chunked filtering");
        }
        checkSafe(input, "chunked filtering");
    }

    // Random streams: the filtered output is an ordered subsequence of the
    // input, never trips the oracle, and leaves already-safe streams intact.
    std::mt19937_64 rng(0xC51EA4A8);
    const char alphabet[] = {
        '\x1b', '[', ']', 'P', 'X', '\\', ';', ':', '?', '0', '1', '9', 'm', 'h', '$',
        ' ', '!', '"', '\x07', '\x00', '\x7f', '\x18', '\x1a', '\t', '\x0a',
        '\x9b', '\x90', '\xc3', '\xa9', '\xe2', '\x80', '\x8b', '\x1b', '[', ';', ';',
    };
    for (int iteration = 0; iteration < 2000; ++iteration) {
        std::string input;
        const size_t length = 1 + rng() % 384;
        for (size_t i = 0; i < length; ++i) input.push_back(alphabet[rng() % sizeof(alphabet)]);
        const std::string out = filter(input);
        assert(out.size() <= input.size());
        size_t cursor = 0;
        for (const char c : out) { // ordered subsequence
            while (cursor < input.size() && input[cursor] != c) ++cursor;
            assert(cursor < input.size());
            ++cursor;
        }
        LibvtermMirror m;
        m.feed(out.data(), out.size());
        if (m.oobWrites != 0) {
            std::fprintf(stderr, "FAIL fuzz iteration %d: filtered output overflows\n", iteration);
            return 1;
        }
        LibvtermMirror raw;
        raw.feed(input.data(), input.size());
        if (raw.oobWrites == 0 && out != input) {
            std::fprintf(stderr, "FAIL fuzz iteration %d: safe stream was altered\n", iteration);
            return 1;
        }
    }

    std::printf("csi_filter: all checks passed\n");
    return 0;
}
