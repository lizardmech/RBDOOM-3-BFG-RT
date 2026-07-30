#!/usr/bin/env node

// Small, dependency-free reader for the instrumentation events in Optick .opt
// captures. It intentionally ignores sampling, tags, and platform trace data.

"use strict";

const fs = require("fs");
const zlib = require("zlib");

const RESPONSE_HEADER_BYTES = 12;
const OPTICK_FILE_HEADER_BYTES = 8;
const RESPONSE_BOARD = 0;
const RESPONSE_EVENT_FRAME = 1;

function fail(message) {
    process.stderr.write(`optick_capture_summary: ${message}\n`);
    process.exit(1);
}

function readU32(buffer, offset) {
    return buffer.readUInt32LE(offset);
}

function readI32(buffer, offset) {
    return buffer.readInt32LE(offset);
}

function readI64(buffer, offset) {
    return buffer.readBigInt64LE(offset);
}

function readString(buffer, cursor) {
    if (cursor.offset + 4 > buffer.length) {
        throw new Error("truncated string length");
    }
    const length = readU32(buffer, cursor.offset);
    cursor.offset += 4;
    if (length > 1 << 20 || cursor.offset + length > buffer.length) {
        throw new Error("invalid string length");
    }
    const value = buffer.toString("utf8", cursor.offset, cursor.offset + length);
    cursor.offset += length;
    return value;
}

function readResponses(fileBuffer) {
    if (fileBuffer.length < OPTICK_FILE_HEADER_BYTES + 2) {
        fail("capture is too small");
    }

    let stream;
    try {
        const compressed = fileBuffer.subarray(OPTICK_FILE_HEADER_BYTES);
        stream = compressed[0] === 0x1f && compressed[1] === 0x8b
            ? zlib.gunzipSync(compressed)
            : zlib.inflateSync(compressed);
    } catch (error) {
        fail(`cannot decompress capture: ${error.message}`);
    }

    const responses = [];
    let offset = 0;
    while (offset + RESPONSE_HEADER_BYTES <= stream.length) {
        const version = readU32(stream, offset);
        const size = readU32(stream, offset + 4);
        const type = stream.readUInt16LE(offset + 8);
        const application = stream.readUInt16LE(offset + 10);
        offset += RESPONSE_HEADER_BYTES;
        if (offset + size > stream.length) {
            fail(`truncated response type ${type} at byte ${offset}`);
        }
        responses.push({
            version,
            type,
            application,
            payload: stream.subarray(offset, offset + size),
        });
        offset += size;
    }
    return responses;
}

function isPlausibleText(value) {
    if (value.length === 0) {
        return true;
    }
    for (let i = 0; i < value.length; ++i) {
        const code = value.charCodeAt(i);
        if (code === 9 || code === 10 || code === 13) {
            continue;
        }
        if (code < 32 || code === 127 || code === 0xfffd) {
            return false;
        }
    }
    return true;
}

function tryReadDescriptionVector(payload, start) {
    if (start + 4 > payload.length) {
        return null;
    }
    const count = readU32(payload, start);
    if (count < 16 || count > 100000) {
        return null;
    }

    const cursor = { offset: start + 4 };
    const descriptions = [];
    try {
        for (let i = 0; i < count; ++i) {
            const name = readString(payload, cursor);
            const file = readString(payload, cursor);
            if (!isPlausibleText(name) || !isPlausibleText(file)) {
                return null;
            }
            if (cursor.offset + 17 > payload.length) {
                return null;
            }
            const line = readU32(payload, cursor.offset);
            const filter = readU32(payload, cursor.offset + 4);
            const color = readU32(payload, cursor.offset + 8);
            const budget = payload.readFloatLE(cursor.offset + 12);
            const flags = payload[cursor.offset + 16];
            cursor.offset += 17;
            descriptions.push({ name, file, line, filter, color, budget, flags });
        }
    } catch {
        return null;
    }

    const named = descriptions.filter((entry) => entry.name.length > 0).length;
    const sourceBacked = descriptions.filter((entry) =>
        entry.file.includes(".cpp") || entry.file.includes(".h")).length;
    if (named < Math.min(8, count) || sourceBacked === 0) {
        return null;
    }
    return { descriptions, end: cursor.offset };
}

function parseBoard(payload) {
    if (payload.length < 40) {
        throw new Error("board response is too small");
    }
    const frequency = readI64(payload, 4);
    if (frequency <= 0n) {
        throw new Error("invalid timer frequency");
    }

    let best = null;
    for (let offset = 40; offset + 4 <= payload.length; ++offset) {
        const candidate = tryReadDescriptionVector(payload, offset);
        if (!candidate) {
            continue;
        }
        const score = candidate.descriptions.filter((entry) =>
            entry.name.startsWith("PT ") ||
            entry.name === "MainThread" ||
            entry.name.startsWith("PathTrace")).length;
        if (!best || score > best.score) {
            best = { ...candidate, score };
        }
    }
    if (!best || best.score === 0) {
        throw new Error("could not locate the event-description board");
    }
    return { frequency, descriptions: best.descriptions };
}

function readEventVector(payload, cursor) {
    if (cursor.offset + 4 > payload.length) {
        throw new Error("truncated event-vector length");
    }
    const count = readU32(payload, cursor.offset);
    cursor.offset += 4;
    if (count > 10000000 || cursor.offset + count * 20 > payload.length) {
        throw new Error("invalid event-vector length");
    }
    const events = new Array(count);
    for (let i = 0; i < count; ++i) {
        events[i] = {
            start: readI64(payload, cursor.offset),
            finish: readI64(payload, cursor.offset + 8),
            description: readU32(payload, cursor.offset + 16),
        };
        cursor.offset += 20;
    }
    return events;
}

function parseEventFrame(payload) {
    if (payload.length < 44) {
        throw new Error("event frame is too small");
    }
    const cursor = { offset: 0 };
    const board = readU32(payload, cursor.offset);
    const thread = readI32(payload, cursor.offset + 4);
    const fiber = readI32(payload, cursor.offset + 8);
    const start = readI64(payload, cursor.offset + 12);
    const finish = readI64(payload, cursor.offset + 20);
    const frameType = readI32(payload, cursor.offset + 28);
    cursor.offset = 32;
    readEventVector(payload, cursor); // Colored-category duplicates.
    const events = readEventVector(payload, cursor);
    return { board, thread, fiber, start, finish, frameType, events };
}

function addStat(stats, key, inclusive, exclusive) {
    let stat = stats.get(key);
    if (!stat) {
        stat = {
            calls: 0,
            inclusive: 0n,
            exclusive: 0n,
            maximum: 0n,
        };
        stats.set(key, stat);
    }
    stat.calls += 1;
    stat.inclusive += inclusive;
    stat.exclusive += exclusive;
    if (inclusive > stat.maximum) {
        stat.maximum = inclusive;
    }
}

function aggregateFrame(frame, stats) {
    const events = frame.events
        .filter((event) => event.finish >= event.start)
        .sort((lhs, rhs) => {
            if (lhs.start !== rhs.start) {
                return lhs.start < rhs.start ? -1 : 1;
            }
            if (lhs.finish !== rhs.finish) {
                return lhs.finish > rhs.finish ? -1 : 1;
            }
            return 0;
        });

    const stack = [];
    for (const event of events) {
        while (stack.length > 0 && event.start >= stack[stack.length - 1].finish) {
            const completed = stack.pop();
            addStat(
                stats,
                completed.description,
                completed.finish - completed.start,
                completed.finish - completed.start - completed.childTicks);
        }
        if (stack.length > 0 && event.finish <= stack[stack.length - 1].finish) {
            stack[stack.length - 1].childTicks += event.finish - event.start;
        }
        stack.push({ ...event, childTicks: 0n });
    }
    while (stack.length > 0) {
        const completed = stack.pop();
        addStat(
            stats,
            completed.description,
            completed.finish - completed.start,
            completed.finish - completed.start - completed.childTicks);
    }
}

function ticksToMilliseconds(ticks, frequency) {
    return Number(ticks) * 1000.0 / Number(frequency);
}

function main() {
    const capturePath = process.argv[2];
    if (!capturePath) {
        fail("usage: node tools/optick_capture_summary.js <capture.opt> [name-filter]");
    }
    const filterText = (process.argv[3] || "").toLowerCase();
    const responses = readResponses(fs.readFileSync(capturePath));
    const boardResponse = responses.find((response) => response.type === RESPONSE_BOARD);
    if (!boardResponse) {
        fail("capture has no description board");
    }

    let board;
    try {
        board = parseBoard(boardResponse.payload);
    } catch (error) {
        fail(error.message);
    }

    const stats = new Map();
    let eventFrames = 0;
    for (const response of responses) {
        if (response.type !== RESPONSE_EVENT_FRAME) {
            continue;
        }
        try {
            aggregateFrame(parseEventFrame(response.payload), stats);
            ++eventFrames;
        } catch (error) {
            fail(`cannot parse event frame ${eventFrames}: ${error.message}`);
        }
    }

    const rows = [];
    for (const [descriptionIndex, stat] of stats) {
        const description = board.descriptions[descriptionIndex];
        if (!description) {
            continue;
        }
        if (filterText && !description.name.toLowerCase().includes(filterText)) {
            continue;
        }
        rows.push({
            name: description.name,
            calls: stat.calls,
            inclusiveMs: ticksToMilliseconds(stat.inclusive, board.frequency),
            exclusiveMs: ticksToMilliseconds(stat.exclusive, board.frequency),
            maximumMs: ticksToMilliseconds(stat.maximum, board.frequency),
        });
    }
    rows.sort((lhs, rhs) => rhs.exclusiveMs - lhs.exclusiveMs);

    process.stdout.write(
        `capture=${capturePath}\nevent_frames=${eventFrames} frequency=${board.frequency}\n`);
    process.stdout.write(
        "exclusive_ms inclusive_ms max_ms calls event\n");
    for (const row of rows.slice(0, 100)) {
        process.stdout.write(
            `${row.exclusiveMs.toFixed(3).padStart(12)} ` +
            `${row.inclusiveMs.toFixed(3).padStart(12)} ` +
            `${row.maximumMs.toFixed(3).padStart(8)} ` +
            `${String(row.calls).padStart(7)} ${row.name}\n`);
    }
}

main();
