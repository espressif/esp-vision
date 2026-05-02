/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import { StringDecoder } from "string_decoder";
import * as vscode from "vscode";
import { RawRepl } from "./rawRepl";
import { SerialPortInfo, SerialTransport } from "./serialTransport";

let transport: SerialTransport | undefined;
let rawRepl: RawRepl | undefined;
let output: vscode.OutputChannel | undefined;
let status: vscode.StatusBarItem | undefined;
let runButton: vscode.StatusBarItem | undefined;
let stopButton: vscode.StatusBarItem | undefined;
let resetButton: vscode.StatusBarItem | undefined;
let previewButton: vscode.StatusBarItem | undefined;
let previewPanel: vscode.WebviewPanel | undefined;
let lastPreviewFrame: PreviewFrame | undefined;
let previewFrameSeq = 0;
let previewFrameTimes: number[] = [];
let currentPort = "";
let suppressSerialOutput = false;
let rawOutputParser: RawOutputParser | undefined;
let serialPreviewParser: PreviewTextParser = createPreviewTextParser({ dropUntilFirstFrame: true });
let serialDecoder = new StringDecoder("utf8");
let lastRunnableUri: vscode.Uri | undefined;
let runCompletion: Promise<void> | undefined;
let resolveRunCompletion: (() => void) | undefined;
let runOperation: Promise<void> = Promise.resolve();

const PREVIEW_FPS_WINDOW_MS = 2000;

interface RawOutputParser {
    phase: "header" | "stdout" | "stderr" | "prompt";
    decoder: StringDecoder;
    stderrBytes: number;
    stopRequested: boolean;
    preview: PreviewTextParser;
}

interface PreviewFrame {
    seq: number;
    width: number;
    height: number;
    size: number;
    fps: number;
    format: string;
    encoding: string;
    base64: string;
    receivedAt: Date;
}

interface PreviewFrameHeader {
    width: number;
    height: number;
    size: number;
    format: string;
    encoding: string;
    raw: string;
}

interface PreviewTextParser {
    phase: "text" | "body";
    pending: string;
    header?: PreviewFrameHeader;
    dropUntilFirstFrame?: boolean;
    seenFrame?: boolean;
}

export function activate(context: vscode.ExtensionContext): void {
    const nextOutput = vscode.window.createOutputChannel("ESP-VISION");
    const nextStatus = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
    const nextRunButton = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 99);
    const nextStopButton = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 98);
    const nextResetButton = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 97);
    const nextPreviewButton = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 96);
    output = nextOutput;
    status = nextStatus;
    runButton = nextRunButton;
    stopButton = nextStopButton;
    resetButton = nextResetButton;
    previewButton = nextPreviewButton;
    nextRunButton.text = "$(play)";
    nextRunButton.tooltip = "ESP-VISION: Run Current File";
    nextRunButton.command = "espVision.runCurrentFile";
    nextStopButton.text = "$(debug-stop)";
    nextStopButton.tooltip = "ESP-VISION: Stop Script";
    nextStopButton.command = "espVision.stopScript";
    nextResetButton.text = "$(debug-restart)";
    nextResetButton.tooltip = "ESP-VISION: Soft Reset";
    nextResetButton.command = "espVision.softReset";
    nextPreviewButton.text = "$(preview)";
    nextPreviewButton.tooltip = "ESP-VISION: Show Preview";
    nextPreviewButton.command = "espVision.showPreview";
    context.subscriptions.push(nextOutput, nextStatus, nextRunButton, nextStopButton, nextResetButton, nextPreviewButton);
    updateStatus();

    context.subscriptions.push(
        vscode.commands.registerCommand("espVision.connect", connect),
        vscode.commands.registerCommand("espVision.disconnect", disconnect),
        vscode.commands.registerCommand("espVision.runCurrentFile", runCurrentFile),
        vscode.commands.registerCommand("espVision.stopScript", stopScript),
        vscode.commands.registerCommand("espVision.softReset", softReset),
        vscode.commands.registerCommand("espVision.showPreview", showPreview),
        vscode.window.onDidChangeActiveTextEditor((editor) => {
            if (editor && isRunnableDocument(editor.document)) {
                lastRunnableUri = editor.document.uri;
            }
        }),
    );

    const activeEditor = vscode.window.activeTextEditor;
    if (activeEditor && isRunnableDocument(activeEditor.document)) {
        lastRunnableUri = activeEditor.document.uri;
    }
}

export async function deactivate(): Promise<void> {
    await disconnect({ quiet: true });
    output = undefined;
    status = undefined;
    runButton = undefined;
    stopButton = undefined;
    resetButton = undefined;
    previewButton = undefined;
    previewPanel = undefined;
}

async function connect(): Promise<void> {
    const config = vscode.workspace.getConfiguration("espVision");
    const configuredPort = config.get<string>("serialPort", "");
    const baudRate = config.get<number>("baudRate", 115200);
    const port = configuredPort || await pickSerialPort();

    if (!port) {
        return;
    }

    if (transport?.isOpen) {
        await disconnect();
    }

    const nextTransport = new SerialTransport();
    nextTransport.on("data", handleSerialData);
    nextTransport.on("error", (error: Error) => {
        appendOutputLine(`\n[serial error] ${error.message}`);
        vscode.window.showErrorMessage(`ESP-VISION serial error: ${error.message}`);
    });
    nextTransport.on("close", () => {
        if (transport === nextTransport) {
            transport = undefined;
            rawRepl = undefined;
            currentPort = "";
        }
        appendOutputLine("\n[serial closed]");
        updateStatus(false);
    });

    await nextTransport.open(port, baudRate);
    transport = nextTransport;
    rawRepl = new RawRepl(nextTransport);
    currentPort = port;
    resetSerialPreviewParser();
    showOutput(true);
    appendOutputLine(`[connected] ${port} @ ${baudRate}`);
    updateStatus(true);
}

async function disconnect(options: { quiet?: boolean } = {}): Promise<void> {
    const currentTransport = transport;
    if (!currentTransport) {
        return;
    }

    transport = undefined;
    rawRepl = undefined;
    currentPort = "";
    finishRawRun();
    suppressSerialOutput = false;
    resetSerialPreviewParser();

    if (options.quiet) {
        currentTransport.removeAllListeners();
    }

    await currentTransport.close();
    updateStatus(false);
}

async function runCurrentFile(): Promise<void> {
    runOperation = runOperation
        .catch(() => undefined)
        .then(runCurrentFileInner);
    await runOperation;
}

async function runCurrentFileInner(): Promise<void> {
    const repl = requireRawRepl();
    if (!repl) {
        return;
    }

    const document = await getRunnableDocument();
    if (!document) {
        return;
    }

    if (document.isDirty) {
        await document.save();
    }

    if (rawOutputParser) {
        appendOutputLine("\n[restart]");
        await stopRunningScript(repl);
    }

    showOutput(true);
    appendOutputLine(`\n[run] ${document.fileName}`);
    try {
        suppressSerialOutput = true;
        await repl.enter();
        suppressSerialOutput = false;
        startRawRun();
        await repl.executeScript(document.getText());
    } catch (error) {
        finishRawRun();
        const message = error instanceof Error ? error.message : String(error);
        appendOutputLine(`[error] ${message}`);
        vscode.window.showErrorMessage(`ESP-VISION run failed: ${message}`);
    } finally {
        suppressSerialOutput = false;
    }
}

async function stopScript(): Promise<void> {
    const repl = requireRawRepl();
    if (!repl) {
        return;
    }

    await stopRunningScript(repl, true);
}

async function softReset(): Promise<void> {
    const repl = requireRawRepl();
    if (!repl) {
        return;
    }

    appendOutputLine("\n[soft reset]");
    finishRawRun();
    suppressSerialOutput = true;
    try {
        await repl.softReset();
        transport?.clearInput();
    } finally {
        suppressSerialOutput = false;
    }
}

async function showPreview(): Promise<void> {
    if (previewPanel) {
        previewPanel.reveal(vscode.ViewColumn.Beside);
        updatePreviewPanel();
        return;
    }

    previewPanel = vscode.window.createWebviewPanel(
        "espVisionPreview",
        "ESP-VISION Preview",
        vscode.ViewColumn.Beside,
        {
            enableScripts: false,
            retainContextWhenHidden: true,
        },
    );
    previewPanel.onDidDispose(() => {
        previewPanel = undefined;
    });
    updatePreviewPanel();
}

async function pickSerialPort(): Promise<string | undefined> {
    const ports = await SerialTransport.listPorts();
    if (ports.length === 0) {
        vscode.window.showWarningMessage("No serial ports found.");
        return undefined;
    }

    const selected = await vscode.window.showQuickPick(
        ports.map((port: SerialPortInfo) => ({
            label: port.path,
            description: port.label === port.path ? undefined : port.label,
            port,
        })),
        { placeHolder: "Select ESP-VISION serial port" },
    );

    return selected?.port.path;
}

function requireRawRepl(): RawRepl | undefined {
    if (!rawRepl || !transport?.isOpen) {
        vscode.window.showWarningMessage("ESP-VISION is not connected.");
        return undefined;
    }
    return rawRepl;
}

async function getRunnableDocument(): Promise<vscode.TextDocument | undefined> {
    const editor = vscode.window.activeTextEditor;
    if (editor && isRunnableDocument(editor.document)) {
        lastRunnableUri = editor.document.uri;
        return editor.document;
    }

    if (lastRunnableUri) {
        try {
            return await vscode.workspace.openTextDocument(lastRunnableUri);
        } catch {
            lastRunnableUri = undefined;
        }
    }

    vscode.window.showWarningMessage("Open a Python file before running ESP-VISION.");
    return undefined;
}

function isRunnableDocument(document: vscode.TextDocument): boolean {
    return document.uri.scheme === "file" &&
        (document.languageId === "python" || document.fileName.endsWith(".py"));
}

function startRawRun(): void {
    rawOutputParser = {
        phase: "header",
        decoder: new StringDecoder("utf8"),
        stderrBytes: 0,
        stopRequested: false,
        preview: createPreviewTextParser(),
    };
    runCompletion = new Promise<void>((resolve) => {
        resolveRunCompletion = resolve;
    });
}

function finishRawRun(): void {
    rawOutputParser = undefined;
    resetPreviewFps();
    const resolve = resolveRunCompletion;
    runCompletion = undefined;
    resolveRunCompletion = undefined;
    resolve?.();
}

async function stopRunningScript(repl: RawRepl, showMessage = false): Promise<void> {
    if (showMessage) {
        appendOutputLine("\n[stop]");
    }
    if (rawOutputParser) {
        rawOutputParser.stopRequested = true;
    }

    await repl.stopScript();

    if (runCompletion) {
        try {
            await withTimeout(runCompletion, 3000);
        } catch {
            finishRawRun();
            transport?.clearInput();
            appendOutputLine("[stopped]");
        }
    }
}

async function withTimeout<T>(promise: Promise<T>, timeoutMs: number): Promise<T> {
    let timer: NodeJS.Timeout | undefined;
    try {
        return await Promise.race([
            promise,
            new Promise<T>((_, reject) => {
                timer = setTimeout(() => reject(new Error("timeout")), timeoutMs);
            }),
        ]);
    } finally {
        if (timer) {
            clearTimeout(timer);
        }
    }
}

function updateStatus(connected = Boolean(transport?.isOpen)): void {
    if (!status) {
        return;
    }
    try {
        status.text = connected ? `$(plug) ${currentPort}` : "$(plug) ESP-VISION";
        status.tooltip = connected ? "ESP-VISION board connected" : "Connect to ESP-VISION board";
        status.command = connected ? "espVision.disconnect" : "espVision.connect";
        status.show();
        runButton?.show();
        stopButton?.show();
        resetButton?.show();
        previewButton?.show();
    } catch {
        status = undefined;
    }
}

function appendOutput(value: string): void {
    try {
        output?.append(value);
    } catch {
        output = undefined;
    }
}

function createPreviewTextParser(options: { dropUntilFirstFrame?: boolean } = {}): PreviewTextParser {
    return {
        phase: "text",
        pending: "",
        dropUntilFirstFrame: options.dropUntilFirstFrame,
        seenFrame: false,
    };
}

function resetSerialPreviewParser(): void {
    serialPreviewParser = createPreviewTextParser({ dropUntilFirstFrame: true });
    serialDecoder = new StringDecoder("utf8");
}

function handleSerialData(data: Buffer): void {
    if (rawOutputParser) {
        handleRawOutputData(data);
        return;
    }

    if (suppressSerialOutput) {
        return;
    }

    const text = serialDecoder.write(data);
    appendOutput(normalizeLineEndings(processPreviewText(serialPreviewParser, text)));
}

function handleRawOutputData(data: Buffer): void {
    let remaining = data;

    while (remaining.length > 0 && rawOutputParser) {
        const parser = rawOutputParser;
        if (parser.phase === "header") {
            const okIndex = remaining.indexOf("OK");
            if (okIndex < 0) {
                return;
            }
            remaining = remaining.subarray(okIndex + 2);
            parser.phase = "stdout";
            continue;
        }

        if (rawOutputParser.phase === "prompt") {
            const promptIndex = remaining.indexOf(">");
            if (promptIndex < 0) {
                return;
            }
            appendOutputLine(statusTextForRawParser(parser));
            finishRawRun();
            remaining = remaining.subarray(promptIndex + 1);
            continue;
        }

        const delimiterIndex = remaining.indexOf(0x04);
        if (delimiterIndex < 0) {
            appendRawStreamData(parser, remaining);
            return;
        }

        const phaseBeforeDelimiter = parser.phase;
        appendRawStreamData(parser, remaining.subarray(0, delimiterIndex));
        appendRawText(parser, parser.decoder.end());
        if (phaseBeforeDelimiter === "stdout") {
            appendOutput(normalizeLineEndings(flushPreviewText(parser.preview)));
        }
        remaining = remaining.subarray(delimiterIndex + 1);
        parser.decoder = new StringDecoder("utf8");
        parser.phase = parser.phase === "stdout" ? "stderr" : "prompt";
    }

    if (remaining.length > 0) {
        handleSerialData(remaining);
    }
}

function appendRawStreamData(parser: RawOutputParser, data: Buffer): void {
    if (data.length === 0) {
        return;
    }
    if (parser.phase === "stderr") {
        parser.stderrBytes += data.length;
        appendOutput(normalizeLineEndings(parser.decoder.write(data)));
        return;
    }
    appendRawText(parser, parser.decoder.write(data));
}

function appendRawText(parser: RawOutputParser, value: string): void {
    if (value.length === 0) {
        return;
    }
    if (parser.phase !== "stdout") {
        appendOutput(normalizeLineEndings(value));
        return;
    }

    appendOutput(normalizeLineEndings(processPreviewText(parser.preview, value)));
}

function processPreviewText(parser: PreviewTextParser, value: string): string {
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
        if (frameEnd < 0) {
            return outputText;
        }

        const base64 = parser.pending.slice(0, frameEnd).replace(/\s+/g, "");
        const header = parser.header;
        parser.pending = parser.pending.slice(frameEnd + "</EVFRAME>".length);
        parser.pending = parser.pending.replace(/^\r?\n/, "");
        parser.phase = "text";
        parser.header = undefined;

        if (header) {
            parser.seenFrame = true;
            handlePreviewFrame(header, base64);
        }
    }
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

function handlePreviewFrame(header: PreviewFrameHeader, base64: string): void {
    if (base64.length === 0) {
        return;
    }

    const receivedAtMs = Date.now();
    const fps = recordPreviewFrame(receivedAtMs);
    lastPreviewFrame = {
        seq: ++previewFrameSeq,
        width: header.width,
        height: header.height,
        size: header.size,
        fps,
        format: header.format,
        encoding: header.encoding,
        base64,
        receivedAt: new Date(receivedAtMs),
    };
    if (previewButton) {
        previewButton.text = `$(preview) ${header.width}x${header.height} ${formatFps(fps)}`;
        previewButton.tooltip = `ESP-VISION Preview: ${header.width}x${header.height}, ${formatFps(fps)}`;
    }
    updatePreviewPanel();
}

function recordPreviewFrame(receivedAtMs: number): number {
    previewFrameTimes.push(receivedAtMs);

    const minTime = receivedAtMs - PREVIEW_FPS_WINDOW_MS;
    while (previewFrameTimes.length > 0 && previewFrameTimes[0] < minTime) {
        previewFrameTimes.shift();
    }

    if (previewFrameTimes.length < 2) {
        return 0;
    }

    const elapsedMs = previewFrameTimes[previewFrameTimes.length - 1] - previewFrameTimes[0];
    if (elapsedMs <= 0) {
        return 0;
    }

    return ((previewFrameTimes.length - 1) * 1000) / elapsedMs;
}

function resetPreviewFps(): void {
    previewFrameTimes = [];
}

function flushPreviewText(parser: PreviewTextParser): string {
    const outputText = parser.phase === "body" && parser.header
        ? parser.header.raw + parser.pending
        : parser.pending;
    parser.phase = "text";
    parser.pending = "";
    parser.header = undefined;
    return outputText;
}

function updatePreviewPanel(): void {
    if (!previewPanel) {
        return;
    }
    previewPanel.webview.html = renderPreviewHtml(lastPreviewFrame);
}

function renderPreviewHtml(frame: PreviewFrame | undefined): string {
    const body = frame
        ? `<main>
            <div class="frame">
                <img src="data:image/jpeg;base64,${frame.base64}" alt="ESP-VISION preview frame" />
            </div>
            <footer>${frame.width}x${frame.height} JPG · ${frame.size} bytes · ${formatFps(frame.fps)} · #${frame.seq} · ${escapeHtml(formatTime(frame.receivedAt))}</footer>
        </main>`
        : `<main class="empty"><div>No preview frame</div></main>`;

    return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
html, body {
    height: 100%;
    margin: 0;
    background: var(--vscode-editor-background);
    color: var(--vscode-editor-foreground);
    font-family: var(--vscode-font-family);
}
main {
    box-sizing: border-box;
    display: grid;
    grid-template-rows: minmax(0, 1fr) auto;
    height: 100%;
    padding: 12px;
    gap: 8px;
}
.frame {
    min-height: 0;
    display: grid;
    place-items: center;
    background: var(--vscode-sideBar-background);
    border: 1px solid var(--vscode-panel-border);
}
img {
    display: block;
    max-width: 100%;
    max-height: 100%;
    image-rendering: pixelated;
}
footer {
    color: var(--vscode-descriptionForeground);
    font-size: 12px;
    line-height: 18px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
}
.empty {
    place-items: center;
    color: var(--vscode-descriptionForeground);
}
</style>
</head>
<body>${body}</body>
</html>`;
}

function formatTime(value: Date): string {
    return value.toLocaleTimeString();
}

function formatFps(value: number): string {
    if (!Number.isFinite(value) || value <= 0) {
        return "-- FPS";
    }
    return `${value.toFixed(1)} FPS`;
}

function escapeHtml(value: string): string {
    return value
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;");
}

function statusTextForRawParser(parser: RawOutputParser): string {
    if (parser.stopRequested) {
        return "[stopped]";
    }
    if (parser.stderrBytes > 0) {
        return "[failed]";
    }
    return "[done]";
}

function normalizeLineEndings(value: string): string {
    return value.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
}

function appendOutputLine(value: string): void {
    try {
        output?.appendLine(value);
    } catch {
        output = undefined;
    }
}

function showOutput(preserveFocus: boolean): void {
    try {
        output?.show(preserveFocus);
    } catch {
        output = undefined;
    }
}
