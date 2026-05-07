/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

const PREVIEW_MAX_PENDING_CHARS = 256 * 1024;

export interface PreviewFrameHeader {
    width: number;
    height: number;
    size: number;
    format: string;
    encoding: string;
    raw: string;
}

export interface PreviewTextParser {
    phase: "text" | "body";
    pending: string;
    header?: PreviewFrameHeader;
    dropUntilFirstFrame?: boolean;
    seenFrame?: boolean;
}

export type PreviewFrameHandler = (header: PreviewFrameHeader, base64: string) => void;

export function createPreviewTextParser(options: { dropUntilFirstFrame?: boolean } = {}): PreviewTextParser {
    return {
        phase: "text",
        pending: "",
        dropUntilFirstFrame: options.dropUntilFirstFrame,
        seenFrame: false,
    };
}

export function processPreviewText(parser: PreviewTextParser, value: string, onFrame: PreviewFrameHandler): string {
    parser.pending += value;
    let outputText = "";

    for (;;) {
        if (parser.phase === "text") {
            const frameStart = parser.pending.indexOf("<EVFRAME");
            if (frameStart < 0) {
                const keep = "<EVFRAME".length - 1;
                if (parser.pending.length > keep) {
                    if (!parser.dropUntilFirstFrame || parser.seenFrame) {
                        outputText += parser.pending.slice(0, parser.pending.length - keep);
                    }
                    parser.pending = parser.pending.slice(parser.pending.length - keep);
                }
                return outputText;
            }

            if (!parser.dropUntilFirstFrame || parser.seenFrame) {
                outputText += parser.pending.slice(0, frameStart);
            }
            parser.pending = parser.pending.slice(frameStart);

            const headerEnd = parser.pending.indexOf(">");
            if (headerEnd < 0) {
                return outputText;
            }

            const headerText = parser.pending.slice(0, headerEnd + 1);
            const header = parsePreviewFrameHeader(headerText);
            parser.pending = parser.pending.slice(headerEnd + 1);
            if (!header) {
                outputText += headerText;
                continue;
            }

            parser.phase = "body";
            parser.header = header;
            continue;
        }

        const frameEnd = parser.pending.indexOf("</EVFRAME>");
        const nextFrameStart = parser.pending.indexOf("<EVFRAME", 1);
        if (nextFrameStart >= 0 && (frameEnd < 0 || nextFrameStart < frameEnd)) {
            parser.pending = parser.pending.slice(nextFrameStart);
            parser.phase = "text";
            parser.header = undefined;
            continue;
        }

        if (frameEnd < 0) {
            if (parser.pending.length > PREVIEW_MAX_PENDING_CHARS) {
                parser.pending = "";
                parser.phase = "text";
                parser.header = undefined;
            }
            return outputText;
        }

        const base64 = parser.pending.slice(0, frameEnd).replace(/\s+/g, "");
        const header = parser.header;
        parser.pending = parser.pending.slice(frameEnd + "</EVFRAME>".length);
        parser.pending = parser.pending.replace(/^\r?\n/, "");
        parser.phase = "text";
        parser.header = undefined;

        if (header && isValidPreviewJpeg(header, base64)) {
            parser.seenFrame = true;
            onFrame(header, base64);
        }
    }
}

export function flushPreviewText(parser: PreviewTextParser): string {
    const outputText = parser.phase === "body" && parser.header
        ? parser.header.raw + parser.pending
        : parser.pending;
    parser.phase = "text";
    parser.pending = "";
    parser.header = undefined;
    return outputText;
}

function parsePreviewFrameHeader(headerText: string): PreviewFrameHeader | undefined {
    const attributes = new Map<string, string>();
    const pattern = /([A-Za-z_][A-Za-z0-9_]*)=("[^"]*"|[^\s>]+)/g;
    let match: RegExpExecArray | null;
    while ((match = pattern.exec(headerText)) !== null) {
        const value = match[2].startsWith("\"") ? match[2].slice(1, -1) : match[2];
        attributes.set(match[1], value);
    }

    const width = Number(attributes.get("width"));
    const height = Number(attributes.get("height"));
    const size = Number(attributes.get("size"));
    const format = attributes.get("format") ?? "";
    const encoding = attributes.get("encoding") ?? "";

    if (!Number.isFinite(width) || !Number.isFinite(height) || !Number.isFinite(size)) {
        return undefined;
    }
    if (width <= 0 || height <= 0 || size <= 0) {
        return undefined;
    }
    if (format !== "jpg" || encoding !== "base64") {
        return undefined;
    }

    return {
        width,
        height,
        size,
        format,
        encoding,
        raw: headerText,
    };
}

function isValidPreviewJpeg(header: PreviewFrameHeader, base64: string): boolean {
    if (base64.length === 0) {
        return false;
    }
    if (!/^[A-Za-z0-9+/]*={0,2}$/.test(base64) || (base64.length % 4) !== 0) {
        return false;
    }

    let jpeg: Buffer;
    try {
        jpeg = Buffer.from(base64, "base64");
    } catch {
        return false;
    }

    if (jpeg.length !== header.size || jpeg.length < 4) {
        return false;
    }
    return jpeg[0] === 0xff &&
        jpeg[1] === 0xd8 &&
        jpeg[jpeg.length - 2] === 0xff &&
        jpeg[jpeg.length - 1] === 0xd9;
}
