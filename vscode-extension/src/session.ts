/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import * as fs from "fs";
import * as path from "path";
import { StringDecoder } from "string_decoder";
import * as vscode from "vscode";
import {
    PreviewFrameHeader,
    PreviewTextParser,
    createPreviewTextParser,
    flushPreviewText,
    processPreviewText,
} from "./previewParser";
import { RawRepl } from "./rawRepl";
import { SerialPortInfo, SerialTransport } from "./serialTransport";

const PREVIEW_FPS_WINDOW_MS = 2000;
const SERIAL_RECONNECT_DELAY_MS = 1200;
const SERIAL_RECONNECT_MAX_ATTEMPTS = 20;
const SERIAL_OPEN_SETTLE_MS = 300;

function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

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

interface PreviewWebviewFrame {
    seq: number;
    width: number;
    height: number;
    size: number;
    fpsText: string;
    receivedAtText: string;
    base64: string;
}

export interface StatusButtons {
    status: vscode.StatusBarItem;
    run: vscode.StatusBarItem;
    stop: vscode.StatusBarItem;
    reset: vscode.StatusBarItem;
    preview: vscode.StatusBarItem;
    tools: vscode.StatusBarItem;
}

export class EspVisionSession {
    private transport?: SerialTransport;
    private rawRepl?: RawRepl;
    private currentPort = "";
    private lastSerialPort = "";
    private lastBaudRate = 0;
    private reconnectTimer?: NodeJS.Timeout;
    private reconnectAttempts = 0;
    private manualDisconnect = false;
    private suppressSerialOutput = false;
    private rawOutputParser?: RawOutputParser;
    private serialPreviewParser = createPreviewTextParser({ dropUntilFirstFrame: true });
    private serialDecoder = new StringDecoder("utf8");
    private lastRunnableUri?: vscode.Uri;
    private runCompletion?: Promise<void>;
    private resolveRunCompletion?: () => void;
    private runOperation: Promise<void> = Promise.resolve();
    private previewPanel?: vscode.WebviewPanel;
    private previewPanelReady = false;
    private thresholdPanel?: vscode.WebviewPanel;
    private thresholdPanelReady = false;
    private lastPreviewFrame?: PreviewFrame;
    private previewFrameSeq = 0;
    private previewFrameTimes: number[] = [];

    constructor(
        private readonly extensionUri: vscode.Uri,
        private readonly output: vscode.OutputChannel,
        private readonly buttons: StatusButtons,
    ) {
        this.updateStatus();
    }

    setLastRunnableUri(uri: vscode.Uri): void {
        this.lastRunnableUri = uri;
    }

    async connect(): Promise<void> {
        const config = vscode.workspace.getConfiguration("espVision");
        const configuredPort = config.get<string>("serialPort", "");
        const baudRate = config.get<number>("baudRate", 115200);
        const port = configuredPort || await pickSerialPort();

        if (!port) {
            return;
        }

        if (this.transport?.isOpen) {
            await this.disconnect();
        }

        this.clearReconnectTimer();
        this.manualDisconnect = false;
        await this.openSerialPort(port, baudRate);
    }

    async disconnect(options: { quiet?: boolean } = {}): Promise<void> {
        const currentTransport = this.transport;
        if (!currentTransport) {
            return;
        }

        this.manualDisconnect = true;
        this.clearReconnectTimer();
        this.transport = undefined;
        this.rawRepl = undefined;
        this.currentPort = "";
        this.finishRawRun();
        this.suppressSerialOutput = false;
        this.resetSerialPreviewParser();

        if (options.quiet) {
            currentTransport.removeAllListeners();
        }

        await currentTransport.close();
        this.updateStatus(false);
    }

    async runCurrentFile(): Promise<void> {
        this.runOperation = this.runOperation
            .catch(() => undefined)
            .then(() => this.runCurrentFileInner());
        await this.runOperation;
    }

    async stopScript(): Promise<void> {
        const repl = this.requireRawRepl();
        if (!repl) {
            return;
        }
        await this.stopRunningScript(repl, true);
    }

    async softReset(): Promise<void> {
        const repl = this.requireRawRepl();
        if (!repl) {
            return;
        }

        this.appendOutputLine("\n[soft reset]");
        this.finishRawRun();
        this.suppressSerialOutput = true;
        try {
            await repl.softReset();
            this.transport?.clearInput();
        } finally {
            this.suppressSerialOutput = false;
        }
    }

    async showTools(): Promise<void> {
        const items = [
            { label: "$(symbol-color) Threshold Editor (LAB)", id: "threshold" as const },
        ];
        const selected = await vscode.window.showQuickPick(items, {
            placeHolder: "ESP-VISION Tools",
        });
        if (selected?.id === "threshold") {
            this.openThresholdEditor();
        }
    }

    showPreview(): void {
        if (this.transport?.isOpen && !this.rawOutputParser) {
            this.lastPreviewFrame = undefined;
            this.resetPreviewFps();
            this.resetSerialPreviewParser();
        }

        if (this.previewPanel) {
            this.previewPanel.reveal(vscode.ViewColumn.Beside);
            this.updatePreviewPanel();
            return;
        }

        const mediaRoot = vscode.Uri.joinPath(this.extensionUri, "media");
        const panel = vscode.window.createWebviewPanel(
            "espVisionPreview",
            "ESP-VISION Preview",
            vscode.ViewColumn.Beside,
            {
                enableScripts: true,
                retainContextWhenHidden: true,
                localResourceRoots: [mediaRoot],
            },
        );
        this.previewPanel = panel;
        panel.onDidDispose(() => {
            this.previewPanel = undefined;
            this.previewPanelReady = false;
        });
        panel.webview.onDidReceiveMessage((message: { type?: string } | undefined) => {
            if (message?.type === "ready") {
                this.previewPanelReady = true;
                this.postPreviewState();
            }
        });
        panel.webview.html = this.renderWebviewHtml(panel.webview, "preview");
    }

    private openThresholdEditor(): void {
        if (this.thresholdPanel) {
            this.thresholdPanel.reveal(vscode.ViewColumn.Beside);
            this.postThresholdState();
            return;
        }

        const mediaRoot = vscode.Uri.joinPath(this.extensionUri, "media");
        const panel = vscode.window.createWebviewPanel(
            "espVisionThreshold",
            "ESP-VISION Threshold Editor",
            vscode.ViewColumn.Beside,
            {
                enableScripts: true,
                retainContextWhenHidden: true,
                localResourceRoots: [mediaRoot],
            },
        );
        this.thresholdPanel = panel;
        panel.onDidDispose(() => {
            this.thresholdPanel = undefined;
            this.thresholdPanelReady = false;
        });
        panel.webview.onDidReceiveMessage((message: { type?: string } | undefined) => {
            if (message?.type === "ready") {
                this.thresholdPanelReady = true;
                this.postThresholdState();
            }
        });
        panel.webview.html = this.renderWebviewHtml(panel.webview, "threshold");
    }

    async dispose(): Promise<void> {
        await this.disconnect({ quiet: true });
        this.previewPanel?.dispose();
        this.thresholdPanel?.dispose();
    }

    private async openSerialPort(port: string, baudRate: number): Promise<void> {
        const nextTransport = new SerialTransport();
        nextTransport.on("data", (data: Buffer) => this.handleSerialData(data));
        nextTransport.on("error", (error: Error) => {
            this.appendOutputLine(`\n[serial error] ${error.message}`);
        });
        nextTransport.on("close", () => {
            this.handleSerialClosed(nextTransport);
        });

        try {
            await nextTransport.open(port, baudRate);
        } catch (error) {
            nextTransport.removeAllListeners();
            await nextTransport.close().catch(() => undefined);
            throw error;
        }

        // Let USB CDC settle and skip boot-time noise before resetting parser.
        await delay(SERIAL_OPEN_SETTLE_MS);
        await nextTransport.flushInput();

        this.transport = nextTransport;
        this.rawRepl = new RawRepl(nextTransport);
        this.currentPort = port;
        this.lastSerialPort = port;
        this.lastBaudRate = baudRate;
        this.reconnectAttempts = 0;
        this.lastPreviewFrame = undefined;
        this.resetSerialPreviewParser();
        this.appendOutputLine(`[connected] ${port} @ ${baudRate}`);
        this.updateStatus(true);
    }

    private handleSerialClosed(closedTransport: SerialTransport): void {
        if (this.transport !== closedTransport) {
            return;
        }

        this.transport = undefined;
        this.rawRepl = undefined;
        this.currentPort = "";
        this.finishRawRun();
        this.suppressSerialOutput = false;
        this.lastPreviewFrame = undefined;
        this.resetSerialPreviewParser();
        this.updatePreviewPanel();
        this.postThresholdState();
        this.appendOutputLine("\n[serial closed]");
        this.updateStatus(false);

        if (!this.manualDisconnect && this.lastSerialPort && this.lastBaudRate > 0) {
            this.scheduleReconnect();
        }
    }

    private clearReconnectTimer(): void {
        if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = undefined;
        }
    }

    private scheduleReconnect(): void {
        if (this.reconnectTimer || this.reconnectAttempts >= SERIAL_RECONNECT_MAX_ATTEMPTS) {
            return;
        }

        this.reconnectTimer = setTimeout(() => {
            this.reconnectTimer = undefined;
            void this.reconnectSerial();
        }, SERIAL_RECONNECT_DELAY_MS);
    }

    private async reconnectSerial(): Promise<void> {
        if (this.manualDisconnect || this.transport?.isOpen || !this.lastSerialPort || this.lastBaudRate <= 0) {
            return;
        }

        this.reconnectAttempts += 1;
        try {
            await this.openSerialPort(this.lastSerialPort, this.lastBaudRate);
            this.appendOutputLine(`[reconnected] ${this.lastSerialPort} @ ${this.lastBaudRate}`);
        } catch (error) {
            const message = error instanceof Error ? error.message : String(error);
            this.appendOutputLine(`[reconnect ${this.reconnectAttempts}/${SERIAL_RECONNECT_MAX_ATTEMPTS}] ${message}`);
            this.scheduleReconnect();
        }
    }

    private async runCurrentFileInner(): Promise<void> {
        const repl = this.requireRawRepl();
        if (!repl) {
            return;
        }

        const document = await this.getRunnableDocument();
        if (!document) {
            return;
        }

        if (document.isDirty) {
            await document.save();
        }

        if (this.rawOutputParser) {
            this.appendOutputLine("\n[restart]");
            await this.stopRunningScript(repl);
        }

        this.showOutput(true);
        this.appendOutputLine(`\n[run] ${document.fileName}`);
        try {
            this.suppressSerialOutput = true;
            await repl.enter();
            this.suppressSerialOutput = false;
            this.startRawRun();
            await repl.executeScript(document.getText());
        } catch (error) {
            this.finishRawRun();
            const message = error instanceof Error ? error.message : String(error);
            this.appendOutputLine(`[error] ${message}`);
            vscode.window.showErrorMessage(`ESP-VISION run failed: ${message}`);
        } finally {
            this.suppressSerialOutput = false;
        }
    }

    private requireRawRepl(): RawRepl | undefined {
        if (!this.rawRepl || !this.transport?.isOpen) {
            vscode.window.showWarningMessage("ESP-VISION is not connected.");
            return undefined;
        }
        return this.rawRepl;
    }

    private async getRunnableDocument(): Promise<vscode.TextDocument | undefined> {
        const editor = vscode.window.activeTextEditor;
        if (editor && isRunnableDocument(editor.document)) {
            this.lastRunnableUri = editor.document.uri;
            return editor.document;
        }

        if (this.lastRunnableUri) {
            try {
                return await vscode.workspace.openTextDocument(this.lastRunnableUri);
            } catch {
                this.lastRunnableUri = undefined;
            }
        }

        vscode.window.showWarningMessage("Open a Python file before running ESP-VISION.");
        return undefined;
    }

    private startRawRun(): void {
        this.rawOutputParser = {
            phase: "header",
            decoder: new StringDecoder("utf8"),
            stderrBytes: 0,
            stopRequested: false,
            preview: createPreviewTextParser(),
        };
        this.runCompletion = new Promise<void>((resolve) => {
            this.resolveRunCompletion = resolve;
        });
    }

    private finishRawRun(): void {
        this.rawOutputParser = undefined;
        this.resetPreviewFps();
        const resolve = this.resolveRunCompletion;
        this.runCompletion = undefined;
        this.resolveRunCompletion = undefined;
        resolve?.();
    }

    private async stopRunningScript(repl: RawRepl, showMessage = false): Promise<void> {
        if (showMessage) {
            this.appendOutputLine("\n[stop]");
        }
        if (this.rawOutputParser) {
            this.rawOutputParser.stopRequested = true;
        }

        await repl.stopScript();

        if (this.runCompletion) {
            try {
                await withTimeout(this.runCompletion, 3000);
            } catch {
                this.finishRawRun();
                this.transport?.clearInput();
                this.appendOutputLine("[stopped]");
            }
        }
    }

    private updateStatus(connected = Boolean(this.transport?.isOpen)): void {
        try {
            this.buttons.status.text = connected ? `$(plug) ${this.currentPort}` : "$(plug) ESP-VISION";
            this.buttons.status.tooltip = connected ? "ESP-VISION board connected" : "Connect to ESP-VISION board";
            this.buttons.status.command = connected ? "espVision.disconnect" : "espVision.connect";
            this.buttons.status.show();
            this.buttons.run.show();
            this.buttons.stop.show();
            this.buttons.reset.show();
            this.buttons.preview.show();
            this.buttons.tools.show();
        } catch {
            // status bar item disposed
        }
    }

    private appendOutput(value: string): void {
        try {
            this.output.append(value);
        } catch {
            // output channel disposed
        }
    }

    private appendOutputLine(value: string): void {
        try {
            this.output.appendLine(value);
        } catch {
            // output channel disposed
        }
    }

    private showOutput(preserveFocus: boolean): void {
        try {
            this.output.show(preserveFocus);
        } catch {
            // output channel disposed
        }
    }

    private resetSerialPreviewParser(): void {
        this.serialPreviewParser = createPreviewTextParser({ dropUntilFirstFrame: true });
        this.serialDecoder = new StringDecoder("utf8");
    }

    private handleSerialData(data: Buffer): void {
        if (this.rawOutputParser) {
            this.handleRawOutputData(data);
            return;
        }

        if (this.suppressSerialOutput) {
            return;
        }

        const text = this.serialDecoder.write(data);
        const onFrame = (h: PreviewFrameHeader, b: string) => this.handlePreviewFrame(h, b);
        this.appendOutput(normalizeLineEndings(processPreviewText(this.serialPreviewParser, text, onFrame)));
    }

    private handleRawOutputData(data: Buffer): void {
        let remaining = data;

        while (remaining.length > 0 && this.rawOutputParser) {
            const parser = this.rawOutputParser;
            if (parser.phase === "header") {
                const okIndex = remaining.indexOf("OK");
                if (okIndex < 0) {
                    return;
                }
                remaining = remaining.subarray(okIndex + 2);
                parser.phase = "stdout";
                continue;
            }

            if (this.rawOutputParser.phase === "prompt") {
                const promptIndex = remaining.indexOf(">");
                if (promptIndex < 0) {
                    return;
                }
                this.appendOutputLine(statusTextForRawParser(parser));
                this.finishRawRun();
                remaining = remaining.subarray(promptIndex + 1);
                continue;
            }

            const delimiterIndex = remaining.indexOf(0x04);
            if (delimiterIndex < 0) {
                this.appendRawStreamData(parser, remaining);
                return;
            }

            const phaseBeforeDelimiter = parser.phase;
            this.appendRawStreamData(parser, remaining.subarray(0, delimiterIndex));
            this.appendRawText(parser, parser.decoder.end());
            if (phaseBeforeDelimiter === "stdout") {
                this.appendOutput(normalizeLineEndings(flushPreviewText(parser.preview)));
            }
            remaining = remaining.subarray(delimiterIndex + 1);
            parser.decoder = new StringDecoder("utf8");
            parser.phase = parser.phase === "stdout" ? "stderr" : "prompt";
        }

        if (remaining.length > 0) {
            this.handleSerialData(remaining);
        }
    }

    private appendRawStreamData(parser: RawOutputParser, data: Buffer): void {
        if (data.length === 0) {
            return;
        }
        if (parser.phase === "stderr") {
            parser.stderrBytes += data.length;
            this.appendOutput(normalizeLineEndings(parser.decoder.write(data)));
            return;
        }
        this.appendRawText(parser, parser.decoder.write(data));
    }

    private appendRawText(parser: RawOutputParser, value: string): void {
        if (value.length === 0) {
            return;
        }
        if (parser.phase !== "stdout") {
            this.appendOutput(normalizeLineEndings(value));
            return;
        }
        const onFrame = (h: PreviewFrameHeader, b: string) => this.handlePreviewFrame(h, b);
        this.appendOutput(normalizeLineEndings(processPreviewText(parser.preview, value, onFrame)));
    }

    private handlePreviewFrame(header: PreviewFrameHeader, base64: string): void {
        const receivedAtMs = Date.now();
        const fps = this.recordPreviewFrame(receivedAtMs);
        this.lastPreviewFrame = {
            seq: ++this.previewFrameSeq,
            width: header.width,
            height: header.height,
            size: header.size,
            fps,
            format: header.format,
            encoding: header.encoding,
            base64,
            receivedAt: new Date(receivedAtMs),
        };
        this.buttons.preview.text = `$(preview) ${header.width}x${header.height} ${formatFps(fps)}`;
        this.buttons.preview.tooltip = `ESP-VISION Preview: ${header.width}x${header.height}, ${formatFps(fps)}`;
        this.updatePreviewPanel();
        this.postThresholdState();
    }

    private recordPreviewFrame(receivedAtMs: number): number {
        this.previewFrameTimes.push(receivedAtMs);

        const minTime = receivedAtMs - PREVIEW_FPS_WINDOW_MS;
        while (this.previewFrameTimes.length > 0 && this.previewFrameTimes[0] < minTime) {
            this.previewFrameTimes.shift();
        }

        if (this.previewFrameTimes.length < 2) {
            return 0;
        }

        const elapsedMs = this.previewFrameTimes[this.previewFrameTimes.length - 1] - this.previewFrameTimes[0];
        if (elapsedMs <= 0) {
            return 0;
        }

        return ((this.previewFrameTimes.length - 1) * 1000) / elapsedMs;
    }

    private resetPreviewFps(): void {
        this.previewFrameTimes = [];
    }

    private updatePreviewPanel(): void {
        if (!this.previewPanel || !this.previewPanelReady) {
            return;
        }
        this.postPreviewState();
    }

    private postPreviewState(): void {
        if (!this.previewPanel) {
            return;
        }
        const message = this.lastPreviewFrame
            ? { type: "frame", frame: toPreviewWebviewFrame(this.lastPreviewFrame) }
            : { type: "clear" };
        void this.previewPanel.webview.postMessage(message);
    }

    private postThresholdState(): void {
        if (!this.thresholdPanel || !this.thresholdPanelReady) {
            return;
        }
        const message = this.lastPreviewFrame
            ? { type: "frame", frame: toPreviewWebviewFrame(this.lastPreviewFrame) }
            : { type: "clear" };
        void this.thresholdPanel.webview.postMessage(message);
    }

    private renderWebviewHtml(webview: vscode.Webview, name: string): string {
        const htmlPath = path.join(this.extensionUri.fsPath, "media", `${name}.html`);
        const cssUri = webview.asWebviewUri(vscode.Uri.joinPath(this.extensionUri, "media", `${name}.css`));
        const jsUri = webview.asWebviewUri(vscode.Uri.joinPath(this.extensionUri, "media", `${name}.js`));
        const nonce = createNonce();
        const replacements: Record<string, string> = {
            cspSource: webview.cspSource,
            nonce,
            cssUri: cssUri.toString(),
            jsUri: jsUri.toString(),
        };
        const html = fs.readFileSync(htmlPath, "utf8");
        return html.replace(/\$\{(cspSource|nonce|cssUri|jsUri)\}/g, (_, key: string) => replacements[key] ?? "");
    }
}

export function isRunnableDocument(document: vscode.TextDocument): boolean {
    return document.uri.scheme === "file" &&
        (document.languageId === "python" || document.fileName.endsWith(".py"));
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

function createNonce(): string {
    const chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    let value = "";
    for (let i = 0; i < 32; i++) {
        value += chars.charAt(Math.floor(Math.random() * chars.length));
    }
    return value;
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

function toPreviewWebviewFrame(frame: PreviewFrame): PreviewWebviewFrame {
    return {
        seq: frame.seq,
        width: frame.width,
        height: frame.height,
        size: frame.size,
        fpsText: formatFps(frame.fps),
        receivedAtText: formatTime(frame.receivedAt),
        base64: frame.base64,
    };
}
