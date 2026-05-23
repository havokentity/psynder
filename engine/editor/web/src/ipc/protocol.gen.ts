// SPDX-License-Identifier: MIT
// AUTO-GENERATED — do not edit. Source: engine/editor/ipc/protocol.psy
// Regenerate via CMake or `npm run gen:ipc` in engine/editor/web.

export const kProtocolVersion = 4;

export const channels = {
    log: "log",
    scene: "scene",
    stats: "stats",
    console: "console",
    selection: "selection",
    perf: "perf",
    schemas: "schemas",
} as const;

export interface Hello {
    protocol_version: number;
    engine_version: string;
    session_token: string;
}

export interface Welcome {
    accepted: boolean;
    server_ver: number;
    server_build: string;
    reason: string;
}

export interface Subscribe {
    channel: string;
}

export interface Unsubscribe {
    channel: string;
}

export interface LogLine {
    level: number;
    message: string;
}

export interface SceneDelta {
    frame_index: bigint;
    entity_id: number;
    op: number;
    payload: Uint8Array;
}

export interface StatsTick {
    frame_index: bigint;
    cpu_ms: number;
    gpu_ms: number;
    draw_calls: number;
    entities: number;
}

export interface ConsoleCmd {
    text: string;
    mode: string;
}

export interface ConsoleCompletionQuery {
    id: number;
    input: string;
    cursor: number;
}

export interface ConsoleCompletionReply {
    id: number;
    start: number;
    end: number;
    names: Array<string>;
    kinds: Array<number>;
    values: Array<string>;
    descriptions: Array<string>;
}

export interface SceneDeltaSlice {
    slice: string;
    payload: Uint8Array;
}

export interface ConsoleReply {
    ok: boolean;
    text: string;
}

export const opcodes = {
    HelloFrame: 1,
    WelcomeFrame: 2,
    SubscribeFrame: 3,
    UnsubscribeFrame: 4,
    LogFrame: 16,
    SceneFrame: 17,
    StatsFrame: 18,
    ConsoleFrame: 19,
    SceneDeltaFrame: 20,
    ConsoleReplyFrame: 21,
    ConsoleCompletionQueryFrame: 22,
    ConsoleCompletionReplyFrame: 23,
} as const;

export type FrameName = keyof typeof opcodes;
