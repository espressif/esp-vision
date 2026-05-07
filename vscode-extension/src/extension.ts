/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import * as vscode from "vscode";
import { registerCommands } from "./commands";
import { EspVisionSession, StatusButtons, isRunnableDocument } from "./session";

let session: EspVisionSession | undefined;

export function activate(context: vscode.ExtensionContext): void {
    const output = vscode.window.createOutputChannel("ESP-VISION");
    const buttons = createStatusButtons();

    context.subscriptions.push(
        output,
        buttons.status,
        buttons.run,
        buttons.stop,
        buttons.reset,
        buttons.preview,
        buttons.tools,
    );

    session = new EspVisionSession(context.extensionUri, output, buttons);
    registerCommands(context, session);

    context.subscriptions.push(
        vscode.window.onDidChangeActiveTextEditor((editor) => {
            if (editor && isRunnableDocument(editor.document)) {
                session?.setLastRunnableUri(editor.document.uri);
            }
        }),
    );

    const activeEditor = vscode.window.activeTextEditor;
    if (activeEditor && isRunnableDocument(activeEditor.document)) {
        session.setLastRunnableUri(activeEditor.document.uri);
    }
}

export async function deactivate(): Promise<void> {
    await session?.dispose();
    session = undefined;
}

function createStatusButtons(): StatusButtons {
    const status = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
    const run = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 99);
    const stop = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 98);
    const reset = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 97);
    const preview = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 96);
    const tools = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 95);
    run.text = "$(play)";
    run.tooltip = "ESP-VISION: Run Current File";
    run.command = "espVision.runCurrentFile";
    stop.text = "$(debug-stop)";
    stop.tooltip = "ESP-VISION: Stop Script";
    stop.command = "espVision.stopScript";
    reset.text = "$(debug-restart)";
    reset.tooltip = "ESP-VISION: Soft Reset";
    reset.command = "espVision.softReset";
    preview.text = "$(preview)";
    preview.tooltip = "ESP-VISION: Show Preview";
    preview.command = "espVision.showPreview";
    tools.text = "$(tools)";
    tools.tooltip = "ESP-VISION: Tools";
    tools.command = "espVision.showTools";
    return { status, run, stop, reset, preview, tools };
}
