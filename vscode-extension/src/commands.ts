/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import * as vscode from "vscode";
import { EspVisionSession } from "./session";

export function registerCommands(context: vscode.ExtensionContext, session: EspVisionSession): void {
    context.subscriptions.push(
        vscode.commands.registerCommand("espVision.connect", () => session.connect()),
        vscode.commands.registerCommand("espVision.disconnect", () => session.disconnect()),
        vscode.commands.registerCommand("espVision.runCurrentFile", () => session.runCurrentFile()),
        vscode.commands.registerCommand("espVision.stopScript", () => session.stopScript()),
        vscode.commands.registerCommand("espVision.softReset", () => session.softReset()),
        vscode.commands.registerCommand("espVision.showPreview", () => session.showPreview()),
        vscode.commands.registerCommand("espVision.showTools", () => session.showTools()),
    );
}
