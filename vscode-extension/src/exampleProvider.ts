/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";

export const EXAMPLE_SCHEME = "espvision";

interface CategoryDef {
    id: string;
    label: string;
    icon: string;
    resolveRoot: (extensionUri: vscode.Uri) => string | undefined;
}

const CATEGORIES: CategoryDef[] = [
    {
        id: "examples",
        label: "Examples",
        icon: "library",
        resolveRoot: (extensionUri) => resolveFirstExisting([
            path.join(extensionUri.fsPath, "examples"),
            path.join(extensionUri.fsPath, "..", "example"),
        ]),
    },
];

function resolveFirstExisting(candidates: string[]): string | undefined {
    for (const candidate of candidates) {
        if (fs.existsSync(candidate)) {
            return candidate;
        }
    }
    return undefined;
}

type ExplorerNode =
    | { kind: "category"; id: string; label: string; icon: string; rootPath: string }
    | { kind: "dir"; categoryId: string; fsPath: string; relativePath: string; label: string }
    | { kind: "file"; categoryId: string; fsPath: string; relativePath: string; label: string };

export class ExampleProvider implements vscode.TreeDataProvider<ExplorerNode> {
    private readonly categories: { def: CategoryDef; rootPath: string }[];
    private readonly _onDidChangeTreeData = new vscode.EventEmitter<ExplorerNode | undefined | void>();
    readonly onDidChangeTreeData = this._onDidChangeTreeData.event;

    constructor(private readonly extensionUri: vscode.Uri) {
        this.categories = CATEGORIES
            .map((def) => ({ def, rootPath: def.resolveRoot(extensionUri) }))
            .filter((c): c is { def: CategoryDef; rootPath: string } => Boolean(c.rootPath));
    }

    refresh(): void {
        this._onDidChangeTreeData.fire();
    }

    getTreeItem(node: ExplorerNode): vscode.TreeItem {
        if (node.kind === "category") {
            const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.Expanded);
            item.iconPath = new vscode.ThemeIcon(node.icon);
            item.contextValue = "espVisionCategory";
            return item;
        }
        const collapsible = node.kind === "dir"
            ? vscode.TreeItemCollapsibleState.Collapsed
            : vscode.TreeItemCollapsibleState.None;
        const item = new vscode.TreeItem(node.label, collapsible);
        if (node.kind === "dir") {
            item.iconPath = new vscode.ThemeIcon("folder");
        } else {
            item.iconPath = new vscode.ThemeIcon("file-code");
            item.command = {
                command: "espVision.openExample",
                title: "Open Example",
                arguments: [`${node.categoryId}/${node.relativePath}`],
            };
        }
        return item;
    }

    getCategoryAndPathFromUri(uri: vscode.Uri): string | undefined {
        if (uri.scheme !== EXAMPLE_SCHEME) {
            return undefined;
        }
        return uri.path.replace(/^\/+/, "");
    }

    getChildren(node?: ExplorerNode): ExplorerNode[] {
        if (!node) {
            if (this.categories.length === 1) {
                const category = this.categories[0];
                return this.listDir(category.rootPath, category.def.id, category.rootPath);
            }
            return this.categories.map((c) => ({
                kind: "category" as const,
                id: c.def.id,
                label: c.def.label,
                icon: c.def.icon,
                rootPath: c.rootPath,
            }));
        }
        if (node.kind === "category") {
            return this.listDir(node.rootPath, node.id, node.rootPath);
        }
        if (node.kind === "dir") {
            const root = this.categoryRoot(node.categoryId);
            if (!root) {
                return [];
            }
            return this.listDir(node.fsPath, node.categoryId, root);
        }
        return [];
    }

    resolveExamplePath(categoryAndPath: string): string | undefined {
        const slash = categoryAndPath.indexOf("/");
        if (slash < 0) {
            return undefined;
        }
        const categoryId = categoryAndPath.slice(0, slash);
        const relativePath = categoryAndPath.slice(slash + 1);
        const root = this.categoryRoot(categoryId);
        if (!root) {
            return undefined;
        }
        const candidate = path.join(root, relativePath);
        if (!candidate.startsWith(root)) {
            return undefined;
        }
        return fs.existsSync(candidate) ? candidate : undefined;
    }

    private categoryRoot(id: string): string | undefined {
        return this.categories.find((c) => c.def.id === id)?.rootPath;
    }

    readContent(categoryAndPath: string): string | undefined {
        const fsPath = this.resolveExamplePath(categoryAndPath);
        return fsPath ? fs.readFileSync(fsPath, "utf8") : undefined;
    }

    private listDir(dir: string, categoryId: string, root: string): ExplorerNode[] {
        if (!fs.existsSync(dir)) {
            return [];
        }
        const entries = fs.readdirSync(dir, { withFileTypes: true });
        const result: ExplorerNode[] = [];
        for (const entry of entries) {
            if (entry.name.startsWith(".")) {
                continue;
            }
            const fsPath = path.join(dir, entry.name);
            const relativePath = path.relative(root, fsPath).split(path.sep).join("/");
            if (entry.isDirectory()) {
                result.push({ kind: "dir", categoryId, fsPath, relativePath, label: entry.name });
            } else if (entry.isFile() && entry.name.endsWith(".py")) {
                result.push({ kind: "file", categoryId, fsPath, relativePath, label: entry.name });
            }
        }
        result.sort((a, b) => {
            if (a.kind !== b.kind) {
                return a.kind === "dir" ? -1 : 1;
            }
            return a.label.localeCompare(b.label);
        });
        return result;
    }
}

export class ExampleContentProvider implements vscode.TextDocumentContentProvider {
    constructor(private readonly examples: ExampleProvider) {
    }

    provideTextDocumentContent(uri: vscode.Uri): string {
        const categoryAndPath = uri.path.replace(/^\/+/, "");
        const content = this.examples.readContent(categoryAndPath);
        return content ?? `# Example not found: ${categoryAndPath}\n`;
    }
}

export class ExampleCodeLensProvider implements vscode.CodeLensProvider {
    provideCodeLenses(document: vscode.TextDocument): vscode.CodeLens[] {
        if (document.uri.scheme !== EXAMPLE_SCHEME) {
            return [];
        }
        const range = new vscode.Range(0, 0, 0, 0);
        return [
            new vscode.CodeLens(range, {
                command: "espVision.runCurrentFile",
                title: "$(play) Run on Device",
            }),
            new vscode.CodeLens(range, {
                command: "espVision.editExampleCopy",
                title: "$(edit) Edit a Copy",
            }),
        ];
    }
}
