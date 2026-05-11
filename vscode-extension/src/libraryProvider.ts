/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";

export const LIBRARY_SCHEME = "espvision-library";

interface LibraryNode {
    moduleName: string;
    fsPath: string;
}

interface ParsedStub {
    constants: string[];
    aliases: string[];
    functions: string[];
    classes: ParsedClass[];
}

interface ParsedClass {
    name: string;
    members: string[];
}

interface CollectedSignature {
    signature: string;
    consumed: number;
}

export class LibraryProvider implements vscode.TreeDataProvider<LibraryNode> {
    private readonly _onDidChangeTreeData = new vscode.EventEmitter<LibraryNode | undefined | void>();
    readonly onDidChangeTreeData = this._onDidChangeTreeData.event;

    constructor(private readonly extensionUri: vscode.Uri) {
    }

    refresh(): void {
        this._onDidChangeTreeData.fire();
    }

    getTreeItem(node: LibraryNode): vscode.TreeItem {
        const item = new vscode.TreeItem(node.moduleName, vscode.TreeItemCollapsibleState.None);
        item.iconPath = new vscode.ThemeIcon("symbol-module");
        item.command = {
            command: "espVision.openLibrary",
            title: "Open Library Reference",
            arguments: [node.moduleName],
        };
        return item;
    }

    getChildren(): LibraryNode[] {
        const root = this.resolveStubRoot();
        if (!root) {
            return [];
        }

        return fs.readdirSync(root, { withFileTypes: true })
            .filter((entry) => entry.isFile() && entry.name.endsWith(".pyi"))
            .map((entry) => ({
                moduleName: path.basename(entry.name, ".pyi"),
                fsPath: path.join(root, entry.name),
            }))
            .sort((a, b) => a.moduleName.localeCompare(b.moduleName));
    }

    async openModule(moduleName: string): Promise<vscode.TextEditor> {
        const uri = vscode.Uri.parse(`${LIBRARY_SCHEME}:/${moduleName}`);
        const doc = await vscode.workspace.openTextDocument(uri);
        await vscode.languages.setTextDocumentLanguage(doc, "markdown");
        return vscode.window.showTextDocument(doc, {
            preview: false,
        });
    }

    readModuleMarkdown(moduleName: string): string {
        const stubPath = this.resolveModulePath(moduleName);
        if (!stubPath) {
            return `# ${moduleName}\n\nLibrary stub not found.\n`;
        }

        const source = fs.readFileSync(stubPath, "utf8");
        return renderStubMarkdown(moduleName, source);
    }

    private resolveModulePath(moduleName: string): string | undefined {
        if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(moduleName)) {
            return undefined;
        }
        const root = this.resolveStubRoot();
        if (!root) {
            return undefined;
        }
        const candidate = path.join(root, `${moduleName}.pyi`);
        return fs.existsSync(candidate) ? candidate : undefined;
    }

    private resolveStubRoot(): string | undefined {
        return resolveFirstExisting([
            path.join(this.extensionUri.fsPath, "stubs"),
            path.join(this.extensionUri.fsPath, "..", "stubs"),
        ]);
    }
}

export class LibraryContentProvider implements vscode.TextDocumentContentProvider {
    constructor(private readonly library: LibraryProvider) {
    }

    provideTextDocumentContent(uri: vscode.Uri): string {
        const moduleName = uri.path.replace(/^\/+/, "");
        return this.library.readModuleMarkdown(moduleName);
    }
}

function resolveFirstExisting(candidates: string[]): string | undefined {
    for (const candidate of candidates) {
        if (fs.existsSync(candidate)) {
            return candidate;
        }
    }
    return undefined;
}

function renderStubMarkdown(moduleName: string, source: string): string {
    const parsed = parseStub(source);
    const lines = [
        `# ${moduleName}`,
        "",
        "ESP-VISION MicroPython library reference generated from the bundled type stubs.",
        "",
        "```python",
        `import ${moduleName}`,
        "```",
        "",
    ];

    appendSection(lines, "Constants", parsed.constants);
    appendSection(lines, "Type Aliases", parsed.aliases);
    appendSection(lines, "Functions", parsed.functions);

    for (const cls of parsed.classes) {
        lines.push(`## class ${cls.name}`, "");
        if (cls.members.length === 0) {
            lines.push("_No public members in stub._", "");
            continue;
        }
        lines.push("```python");
        for (const member of cls.members) {
            lines.push(member);
        }
        lines.push("```", "");
    }

    return `${lines.join("\n").trimEnd()}\n`;
}

function appendSection(lines: string[], title: string, values: string[]): void {
    if (values.length === 0) {
        return;
    }
    lines.push(`## ${title}`, "", "```python");
    lines.push(...values);
    lines.push("```", "");
}

function parseStub(source: string): ParsedStub {
    const constants: string[] = [];
    const aliases: string[] = [];
    const functions: string[] = [];
    const classes: ParsedClass[] = [];
    const lines = source.split(/\r?\n/);

    let currentClass: ParsedClass | undefined;
    let pendingDocs: string[] = [];
    for (let i = 0; i < lines.length; i++) {
        const rawLine = lines[i];
        const line = rawLine.trim();
        if (line.startsWith("#:")) {
            pendingDocs.push(line.slice(2).trim());
            continue;
        }
        if (line.length === 0 || line.startsWith("#") || line.startsWith("from ") || line.startsWith("import ")) {
            continue;
        }

        const classMatch = /^class\s+([A-Za-z_][A-Za-z0-9_]*)/.exec(line);
        if (classMatch && !rawLine.startsWith(" ")) {
            currentClass = {
                name: classMatch[1],
                members: formatDocs(pendingDocs),
            };
            pendingDocs = [];
            classes.push(currentClass);
            continue;
        }

        if (rawLine.startsWith("    ")) {
            if (currentClass && (line.startsWith("def ") || line.startsWith("@overload"))) {
                const { signature, consumed } = collectSignature(lines, i);
                currentClass.members.push(...formatDocs(pendingDocs));
                currentClass.members.push(signature.replace(/^def /, `${currentClass.name}.`));
                pendingDocs = [];
                i += consumed - 1;
            } else if (currentClass && /^[A-Za-z_][A-Za-z0-9_]*\s*:/.test(line)) {
                currentClass.members.push(...formatDocs(pendingDocs));
                currentClass.members.push(`${currentClass.name}.${line}`);
                pendingDocs = [];
            }
            continue;
        }

        currentClass = undefined;
        if (line.startsWith("def ") || line.startsWith("@overload")) {
            const { signature, consumed } = collectSignature(lines, i);
            functions.push(...formatDocs(pendingDocs));
            functions.push(signature);
            pendingDocs = [];
            i += consumed - 1;
        } else if (/^[A-Z][A-Z0-9_]*\s*:/.test(line)) {
            constants.push(...formatDocs(pendingDocs));
            constants.push(line);
            pendingDocs = [];
        } else if (/^[A-Za-z_][A-Za-z0-9_]*\s*=/.test(line)) {
            aliases.push(...formatDocs(pendingDocs));
            aliases.push(line);
            pendingDocs = [];
        }
    }

    return { constants, aliases, functions, classes };
}

function formatDocs(docs: string[]): string[] {
    return docs.map((doc) => `# ${doc}`);
}

function collectSignature(lines: string[], start: number): CollectedSignature {
    const collected: string[] = [];
    let consumed = 0;
    for (let i = start; i < lines.length; i++) {
        consumed++;
        const line = lines[i].trim();
        if (line.startsWith("@overload")) {
            continue;
        }
        collected.push(line);
        if (line.endsWith("...")) {
            break;
        }
    }
    return {
        signature: collected.join(" ").replace(/\s+/g, " "),
        consumed: Math.max(consumed, 1),
    };
}
