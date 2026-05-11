/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import * as vscode from "vscode";
import { registerCommands } from "./commands";
import { EXAMPLE_SCHEME, ExampleCodeLensProvider, ExampleContentProvider, ExampleProvider } from "./exampleProvider";
import { FileManagerProvider } from "./fileManagerProvider";
import { LIBRARY_SCHEME, LibraryContentProvider, LibraryProvider } from "./libraryProvider";
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

    const exampleProvider = new ExampleProvider(context.extensionUri);
    const exampleContent = new ExampleContentProvider(exampleProvider);
    const libraryProvider = new LibraryProvider(context.extensionUri);
    const libraryContent = new LibraryContentProvider(libraryProvider);
    const fileManagerProvider = new FileManagerProvider();
    session = new EspVisionSession(context.extensionUri, output, buttons, exampleProvider);
    registerCommands(context, session);

    context.subscriptions.push(
        vscode.window.registerTreeDataProvider("espVisionExplorer", exampleProvider),
        vscode.window.registerTreeDataProvider("espVisionLibrary", libraryProvider),
        vscode.window.registerTreeDataProvider("espVisionFiles", fileManagerProvider),
        vscode.workspace.registerTextDocumentContentProvider(EXAMPLE_SCHEME, exampleContent),
        vscode.workspace.registerTextDocumentContentProvider(LIBRARY_SCHEME, libraryContent),
        vscode.languages.registerCodeLensProvider(
            { scheme: EXAMPLE_SCHEME, language: "python" },
            new ExampleCodeLensProvider(),
        ),
        vscode.commands.registerCommand("espVision.refreshExamples", () => exampleProvider.refresh()),
        vscode.commands.registerCommand("espVision.openLibrary", (moduleName: string) => libraryProvider.openModule(moduleName)),
        vscode.commands.registerCommand("espVision.refreshLibrary", () => libraryProvider.refresh()),
        vscode.commands.registerCommand("espVision.refreshFiles", () => fileManagerProvider.refresh()),
        vscode.commands.registerCommand("espVision.mountFlash", () => fileManagerProvider.mountFlash()),
        vscode.commands.registerCommand("espVision.openFlashFile", (node) => fileManagerProvider.openFile(node)),
        vscode.commands.registerCommand("espVision.revealFlashRoot", () => fileManagerProvider.revealRoot()),
        vscode.commands.registerCommand("espVision.selectFlashRoot", () => fileManagerProvider.selectRoot()),
        vscode.commands.registerCommand("espVision.clearFlashRoot", () => fileManagerProvider.clearSelectedRoot()),
        vscode.commands.registerCommand("espVision.newFlashFile", (node) => fileManagerProvider.newFile(node)),
        vscode.commands.registerCommand("espVision.newFlashFolder", (node) => fileManagerProvider.newFolder(node)),
        vscode.commands.registerCommand("espVision.deleteFlashEntry", (node) => fileManagerProvider.deleteEntry(node)),
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
