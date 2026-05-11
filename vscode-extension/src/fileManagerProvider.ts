/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import { execFile } from "child_process";
import * as vscode from "vscode";

const ESP_VISION_DISK_MARKER = ".esp_vision_disk";
const ESP_VISION_MSC_VENDOR = "ESPVIS";

interface MountEntry {
    source: string;
    target: string;
    fstype: string;
}

interface BlockDevice {
    path: string;
    name: string;
    vendor: string;
    model: string;
    isPartition: boolean;
}

type FileNode =
    | { kind: "message"; label: string; description?: string }
    | { kind: "root"; rootPath: string; label: string }
    | { kind: "dir"; rootPath: string; fsPath: string; relativePath: string; label: string }
    | { kind: "file"; rootPath: string; fsPath: string; relativePath: string; label: string };

export class FileManagerProvider implements vscode.TreeDataProvider<FileNode> {
    private readonly _onDidChangeTreeData = new vscode.EventEmitter<FileNode | undefined | void>();
    readonly onDidChangeTreeData = this._onDidChangeTreeData.event;
    private rootPath?: string;
    private mountAttempt?: Promise<void>;
    private lastMountError?: string;
    private lastMountDevice?: BlockDevice;

    constructor() {
        this.rootPath = this.resolveFlashRoot();
        if (!this.rootPath) {
            void this.tryAutoMountFlash();
        }
    }

    refresh(): void {
        this.rootPath = this.resolveFlashRoot();
        if (this.rootPath) {
            this.lastMountError = undefined;
        }
        if (!this.rootPath) {
            void this.tryAutoMountFlash();
        }
        this._onDidChangeTreeData.fire();
    }

    getTreeItem(node: FileNode): vscode.TreeItem {
        if (node.kind === "message") {
            const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.None);
            item.description = node.description;
            item.iconPath = new vscode.ThemeIcon("info");
            item.contextValue = "espVisionFlashMessage";
            return item;
        }

        if (node.kind === "root") {
            const item = new vscode.TreeItem(node.label, vscode.TreeItemCollapsibleState.Expanded);
            item.description = node.rootPath;
            item.iconPath = new vscode.ThemeIcon("device-camera");
            item.contextValue = "espVisionFlashRoot";
            return item;
        }

        const item = new vscode.TreeItem(
            node.label,
            node.kind === "dir" ? vscode.TreeItemCollapsibleState.Collapsed : vscode.TreeItemCollapsibleState.None,
        );
        item.resourceUri = vscode.Uri.file(node.fsPath);
        item.contextValue = node.kind === "dir" ? "espVisionFlashDir" : "espVisionFlashFile";

        if (node.kind === "dir") {
            item.iconPath = new vscode.ThemeIcon("folder");
        } else {
            item.iconPath = new vscode.ThemeIcon("file");
            item.command = {
                command: "espVision.openFlashFile",
                title: "Open Flash File",
                arguments: [node],
            };
        }
        return item;
    }

    getChildren(node?: FileNode): FileNode[] {
        if (!node) {
            const root = this.rootPath;
            if (!root) {
                const device = findEspVisionBlockDevices()[0];
                if (device) {
                    return [{
                        kind: "message",
                        label: `${device.path} is not mounted`,
                        description: this.flashNotMountedDescription(device),
                    }];
                }
                return [{
                    kind: "message",
                    label: "ESP-VISION flash disk not found",
                    description: "Connect USB MSC or select a mount path",
                }];
            }
            return [{
                kind: "root",
                rootPath: root,
                label: "Flash",
            }];
        }

        if (node.kind === "root") {
            return this.listDir(node.rootPath, node.rootPath);
        }
        if (node.kind === "dir") {
            return this.listDir(node.fsPath, node.rootPath);
        }
        return [];
    }

    async openFile(node: FileNode): Promise<void> {
        if (node.kind !== "file") {
            return;
        }
        await vscode.commands.executeCommand("vscode.open", vscode.Uri.file(node.fsPath), { preview: false });
    }

    async revealRoot(): Promise<void> {
        const root = this.rootPath ?? this.resolveFlashRoot();
        if (!root) {
            void vscode.window.showWarningMessage("ESP-VISION flash disk was not found.");
            return;
        }
        await vscode.commands.executeCommand("revealFileInOS", vscode.Uri.file(root));
    }

    async selectRoot(): Promise<void> {
        const picked = await vscode.window.showOpenDialog({
            canSelectFiles: false,
            canSelectFolders: true,
            canSelectMany: false,
            openLabel: "Use as ESP-VISION Flash",
            title: "Select ESP-VISION Flash Mount",
        });
        const selected = picked?.[0]?.fsPath;
        if (!selected) {
            return;
        }

        await vscode.workspace.getConfiguration("espVision")
            .update("flashMountPath", selected, vscode.ConfigurationTarget.Global);
        this.rootPath = selected;
        this.lastMountError = undefined;
        this._onDidChangeTreeData.fire();
    }

    async clearSelectedRoot(): Promise<void> {
        await vscode.workspace.getConfiguration("espVision")
            .update("flashMountPath", "", vscode.ConfigurationTarget.Global);
        this.refresh();
        void vscode.window.showInformationMessage("ESP-VISION selected flash disk was cleared.");
    }

    async mountFlash(): Promise<void> {
        await this.tryAutoMountFlash();
        this.rootPath = this.resolveFlashRoot();
        if (this.rootPath) {
            this.lastMountError = undefined;
            this._onDidChangeTreeData.fire();
            void vscode.window.showInformationMessage(`ESP-VISION flash disk mounted: ${this.rootPath}`);
            return;
        }

        const message = this.lastMountError
            ? `ESP-VISION flash mount failed: ${this.lastMountError}`
            : "ESP-VISION flash disk was not mounted.";
        this._onDidChangeTreeData.fire();
        const actions = shouldOfferTerminalMount(this.lastMountError)
            ? ["Run in Terminal", "Select Mount Path"]
            : ["Select Mount Path"];
        const action = await vscode.window.showWarningMessage(message, ...actions);
        if (action === "Run in Terminal") {
            this.mountFlashInTerminal();
        } else if (action === "Select Mount Path") {
            await this.selectRoot();
        }
    }

    async newFile(node?: FileNode): Promise<void> {
        const parent = this.resolveParentDir(node);
        if (!parent) {
            void vscode.window.showWarningMessage("ESP-VISION flash disk was not found.");
            return;
        }

        const name = await vscode.window.showInputBox({
            prompt: "New file name",
            value: "main.py",
            validateInput: validateRelativeName,
        });
        if (!name) {
            return;
        }

        const target = resolveChildPath(parent, name);
        if (!target || !this.isInsideRoot(target)) {
            void vscode.window.showErrorMessage("Invalid flash file path.");
            return;
        }
        if (fs.existsSync(target)) {
            void vscode.window.showErrorMessage(`File already exists: ${name}`);
            return;
        }

        fs.writeFileSync(target, "", "utf8");
        this._onDidChangeTreeData.fire();
        await vscode.commands.executeCommand("vscode.open", vscode.Uri.file(target), { preview: false });
    }

    async newFolder(node?: FileNode): Promise<void> {
        const parent = this.resolveParentDir(node);
        if (!parent) {
            void vscode.window.showWarningMessage("ESP-VISION flash disk was not found.");
            return;
        }

        const name = await vscode.window.showInputBox({
            prompt: "New folder name",
            value: "scripts",
            validateInput: validateRelativeName,
        });
        if (!name) {
            return;
        }

        const target = resolveChildPath(parent, name);
        if (!target || !this.isInsideRoot(target)) {
            void vscode.window.showErrorMessage("Invalid flash folder path.");
            return;
        }
        if (fs.existsSync(target)) {
            void vscode.window.showErrorMessage(`Folder already exists: ${name}`);
            return;
        }

        fs.mkdirSync(target, { recursive: false });
        this._onDidChangeTreeData.fire();
    }

    async deleteEntry(node?: FileNode): Promise<void> {
        if (!node || (node.kind !== "file" && node.kind !== "dir")) {
            return;
        }
        if (!this.isInsideRoot(node.fsPath)) {
            void vscode.window.showErrorMessage("Refusing to delete a path outside the ESP-VISION flash disk.");
            return;
        }

        const answer = await vscode.window.showWarningMessage(
            `Delete ${node.relativePath} from ESP-VISION flash?`,
            { modal: true },
            "Delete",
        );
        if (answer !== "Delete") {
            return;
        }

        fs.rmSync(node.fsPath, { recursive: node.kind === "dir", force: false });
        this._onDidChangeTreeData.fire();
    }

    private resolveParentDir(node?: FileNode): string | undefined {
        if (!node) {
            return this.rootPath;
        }
        if (node.kind === "root" || node.kind === "dir") {
            return node.kind === "root" ? node.rootPath : node.fsPath;
        }
        if (node.kind === "file") {
            return path.dirname(node.fsPath);
        }
        return this.rootPath;
    }

    private isInsideRoot(candidate: string): boolean {
        const root = this.rootPath;
        if (!root) {
            return false;
        }
        const resolvedRoot = path.resolve(root);
        const resolvedCandidate = path.resolve(candidate);
        return resolvedCandidate === resolvedRoot || resolvedCandidate.startsWith(`${resolvedRoot}${path.sep}`);
    }

    private resolveFlashRoot(): string | undefined {
        const configured = vscode.workspace.getConfiguration("espVision").get<string>("flashMountPath", "").trim();
        if (configured && isDirectory(configured)) {
            return configured;
        }
        return findEspVisionFlashMounts()[0];
    }

    private async tryAutoMountFlash(): Promise<void> {
        if (process.platform !== "linux") {
            return;
        }
        if (this.mountAttempt) {
            return this.mountAttempt;
        }

        this.mountAttempt = this.mountEspVisionBlockDevice();
        try {
            await this.mountAttempt;
        } finally {
            this.mountAttempt = undefined;
        }
    }

    private async mountEspVisionBlockDevice(): Promise<void> {
        this.lastMountError = undefined;
        this.lastMountDevice = undefined;
        let attempted = false;
        for (const device of findEspVisionBlockDevices()) {
            this.lastMountDevice = device;
            if (!fs.existsSync(device.path)) {
                this.lastMountError = `${device.path} is not accessible from the VSCode extension host.`;
                continue;
            }

            attempted = true;
            try {
                await execFileText("udisksctl", ["mount", "-b", device.path]);
            } catch (error) {
                this.lastMountError = error instanceof Error ? error.message : String(error);
                continue;
            }

            const root = this.resolveFlashRoot();
            if (root) {
                this.rootPath = root;
                this.lastMountError = undefined;
                this._onDidChangeTreeData.fire();
                return;
            }
        }

        if (!attempted && !this.lastMountError) {
            this.lastMountError = "No accessible ESPVIS block device was found.";
        }
    }

    private mountFlashInTerminal(): void {
        const device = this.lastMountDevice ?? findEspVisionBlockDevices()[0];
        if (!device) {
            void vscode.window.showWarningMessage("No ESP-VISION flash block device was found.");
            return;
        }

        const terminal = vscode.window.createTerminal("ESP-VISION Flash");
        terminal.show();
        terminal.sendText(`udisksctl mount -b ${shellQuote(device.path)}`);
        void vscode.window.showInformationMessage("Run the mount command in the terminal, then refresh the ESP-VISION Files view.");
    }

    private listDir(dir: string, root: string): FileNode[] {
        if (!isDirectory(dir)) {
            return [];
        }

        const result: FileNode[] = [];
        for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
            if (entry.name === "." || entry.name === "..") {
                continue;
            }

            const fsPath = path.join(dir, entry.name);
            const relativePath = path.relative(root, fsPath).split(path.sep).join("/");
            if (entry.isDirectory()) {
                result.push({ kind: "dir", rootPath: root, fsPath, relativePath, label: entry.name });
            } else if (entry.isFile()) {
                result.push({ kind: "file", rootPath: root, fsPath, relativePath, label: entry.name });
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

    private flashNotMountedDescription(device: BlockDevice): string {
        if (!fs.existsSync(device.path)) {
            return `${device.path} is not accessible`;
        }
        if (this.lastMountError) {
            return `Mount failed: ${this.lastMountError}`;
        }
        return "Use Mount Flash Disk or select a mount path";
    }
}

function findEspVisionBlockDevices(): BlockDevice[] {
    const root = "/sys/class/block";
    if (!isDirectory(root)) {
        return [];
    }

    const devices: BlockDevice[] = [];
    for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
        if (!entry.isSymbolicLink() && !entry.isDirectory()) {
            continue;
        }
        if (entry.name.startsWith("loop")) {
            continue;
        }

        const values = readBlockDeviceValues(entry.name, ["vendor", "model", "product"]);
        if (!values.some((value) => value.includes(ESP_VISION_MSC_VENDOR))) {
            continue;
        }

        const vendor = values.find((value) => value.includes(ESP_VISION_MSC_VENDOR)) ?? "";
        const model = values.find((value) => value.includes("Flash")) ?? "";
        devices.push({
            path: `/dev/${entry.name}`,
            name: entry.name,
            vendor,
            model,
            isPartition: fs.existsSync(path.join(root, entry.name, "partition")),
        });
    }

    return devices.sort((a, b) => {
        if (a.isPartition !== b.isPartition) {
            return a.isPartition ? -1 : 1;
        }
        return a.name.localeCompare(b.name);
    });
}

function findEspVisionFlashMounts(): string[] {
    const mounts = parseProcMounts();
    const candidates = new Map<string, number>();

    for (const mount of mounts) {
        if (!isDirectory(mount.target)) {
            continue;
        }

        let score = 0;
        if (fs.existsSync(path.join(mount.target, ESP_VISION_DISK_MARKER))) {
            score += 100;
        }
        if (deviceMatchesEspVision(mount.source)) {
            score += 80;
        }
        if (looksLikeEspVisionDisk(mount.target)) {
            score += 20;
        }
        if (score > 0) {
            candidates.set(mount.target, score);
        }
    }

    for (const root of fallbackMountRoots()) {
        if (!isDirectory(root)) {
            continue;
        }
        const score = fs.existsSync(path.join(root, ESP_VISION_DISK_MARKER))
            ? 100
            : looksLikeEspVisionDisk(root) ? 20 : 0;
        if (score > 0) {
            candidates.set(root, Math.max(candidates.get(root) ?? 0, score));
        }
    }

    return Array.from(candidates.entries())
        .sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]))
        .map(([target]) => target);
}

function parseProcMounts(): MountEntry[] {
    try {
        const content = fs.readFileSync("/proc/mounts", "utf8");
        return content.split(/\r?\n/)
            .map((line) => line.trim())
            .filter(Boolean)
            .map((line) => {
                const parts = line.split(/\s+/);
                return {
                    source: decodeMountField(parts[0] ?? ""),
                    target: decodeMountField(parts[1] ?? ""),
                    fstype: parts[2] ?? "",
                };
            });
    } catch {
        return [];
    }
}

function decodeMountField(value: string): string {
    return value
        .replace(/\\040/g, " ")
        .replace(/\\011/g, "\t")
        .replace(/\\012/g, "\n")
        .replace(/\\134/g, "\\");
}

function deviceMatchesEspVision(source: string): boolean {
    const deviceName = resolveBlockDeviceName(source);
    if (!deviceName) {
        return false;
    }

    const values = readBlockDeviceValues(deviceName, ["vendor", "model", "product"]);
    return values.some((value) => value.includes(ESP_VISION_MSC_VENDOR));
}

function resolveBlockDeviceName(source: string): string | undefined {
    if (!source.startsWith("/dev/")) {
        return undefined;
    }

    try {
        const realSource = fs.realpathSync(source);
        return path.basename(realSource);
    } catch {
        return path.basename(source);
    }
}

function readBlockDeviceValues(deviceName: string, names: string[]): string[] {
    const start = `/sys/class/block/${deviceName}`;
    let current: string;
    try {
        current = fs.realpathSync(start);
    } catch {
        return [];
    }

    const values: string[] = [];
    for (let depth = 0; depth < 8; depth++) {
        for (const name of names) {
            for (const candidate of [
                path.join(current, name),
                path.join(current, "device", name),
            ]) {
                const value = readSmallTextFile(candidate);
                if (value) {
                    values.push(value);
                }
            }
        }
        const parent = path.dirname(current);
        if (parent === current) {
            break;
        }
        current = parent;
    }
    return values;
}

function readSmallTextFile(filePath: string): string | undefined {
    try {
        return fs.readFileSync(filePath, "utf8").trim();
    } catch {
        return undefined;
    }
}

function fallbackMountRoots(): string[] {
    const username = os.userInfo().username;
    const roots = [
        path.join("/media", username),
        path.join("/run/media", username),
        "/Volumes",
    ];
    const result: string[] = [];
    for (const root of roots) {
        if (!isDirectory(root)) {
            continue;
        }
        for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
            if (entry.isDirectory()) {
                result.push(path.join(root, entry.name));
            }
        }
    }
    return result;
}

function looksLikeEspVisionDisk(root: string): boolean {
    return fs.existsSync(path.join(root, "main.py"))
        && fs.existsSync(path.join(root, "README.txt"));
}

function validateRelativeName(value: string): string | undefined {
    if (!value.trim()) {
        return "Name is required.";
    }
    if (path.isAbsolute(value) || value.split(/[\\/]+/).some((part) => part === "..")) {
        return "Use a relative name inside the flash disk.";
    }
    return undefined;
}

function resolveChildPath(parent: string, name: string): string | undefined {
    const trimmed = name.trim();
    if (!trimmed || path.isAbsolute(trimmed)) {
        return undefined;
    }
    return path.resolve(parent, trimmed);
}

function shouldOfferTerminalMount(error: string | undefined): boolean {
    if (process.platform !== "linux") {
        return false;
    }
    if (!error) {
        return true;
    }

    const normalized = error.toLowerCase();
    return normalized.includes("authentication")
        || normalized.includes("authentyication")
        || normalized.includes("polkit")
        || normalized.includes("controlling terminal")
        || normalized.includes("udisks daemon")
        || normalized.includes("not authorized")
        || normalized.includes("permission");
}

function shellQuote(value: string): string {
    return `'${value.replace(/'/g, "'\\''")}'`;
}

function isDirectory(fsPath: string): boolean {
    try {
        return fs.statSync(fsPath).isDirectory();
    } catch {
        return false;
    }
}

function execFileText(file: string, args: string[]): Promise<{ stdout: string; stderr: string }> {
    return new Promise((resolve, reject) => {
        execFile(file, args, { encoding: "utf8", timeout: 15000 }, (error, stdout, stderr) => {
            if (error) {
                const detail = stderr.trim() || error.message;
                reject(new Error(detail));
                return;
            }
            resolve({ stdout, stderr });
        });
    });
}
