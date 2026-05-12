/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import { EventEmitter } from "events";
import * as path from "path";
import type { SerialPort as SerialPortType } from "serialport";

const MAX_RX_BUFFER_BYTES = 256 * 1024;
const WIN32_OPEN_RETRY_DELAYS_MS = [0, 300, 700, 1200];
const WIN32_CLOSE_SETTLE_MS = 300;
type SerialPortConstructor = typeof import("serialport").SerialPort;

interface ReadWaiter {
    marker: Buffer;
    resolve: (value: Buffer) => void;
    reject: (error: Error) => void;
    timer: NodeJS.Timeout;
}

export interface SerialPortInfo {
    path: string;
    label: string;
    manufacturer?: string;
    productId?: string;
    vendorId?: string;
}

export class SerialTransport extends EventEmitter {
    private port?: SerialPortType;
    private rxBuffer = Buffer.alloc(0);
    private waiters: ReadWaiter[] = [];

    static async listPorts(): Promise<SerialPortInfo[]> {
        const SerialPort = loadSerialPort();
        const ports = await SerialPort.list();
        return ports.map((port) => {
            const details = [port.manufacturer, port.serialNumber].filter(Boolean).join(" ");
            return {
                path: port.path,
                label: details ? `${port.path} - ${details}` : port.path,
                manufacturer: port.manufacturer,
                productId: port.productId,
                vendorId: port.vendorId,
            };
        });
    }

    get isOpen(): boolean {
        return Boolean(this.port?.isOpen);
    }

    async open(path: string, baudRate: number): Promise<void> {
        if (this.isOpen) {
            await this.close();
        }

        const delays = process.platform === "win32" ? WIN32_OPEN_RETRY_DELAYS_MS : [0];
        let lastError: unknown;
        for (let attempt = 0; attempt < delays.length; attempt++) {
            if (delays[attempt] > 0) {
                await delay(delays[attempt]);
            }

            try {
                await this.openOnce(path, baudRate);
                return;
            } catch (error) {
                lastError = error;
                await this.close().catch(() => undefined);
                if (!shouldRetryOpen(error) || attempt === delays.length - 1) {
                    break;
                }
            }
        }

        throw lastError instanceof Error ? lastError : new Error(String(lastError));
    }

    private async openOnce(path: string, baudRate: number): Promise<void> {
        const SerialPort = loadSerialPort();
        this.rxBuffer = Buffer.alloc(0);
        const port = new SerialPort({
            path,
            baudRate,
            autoOpen: false,
        });
        this.port = port;

        port.on("data", (data: Buffer) => {
            this.appendRx(data);
            this.processWaiters();
            this.emit("data", data);
        });

        port.on("error", (error) => this.emit("error", error));
        port.on("close", () => this.emit("close"));

        await new Promise<void>((resolve, reject) => {
            port.open((error) => error ? reject(error) : resolve());
        });
        await new Promise<void>((resolve, reject) => {
            port.set({ dtr: true, rts: false }, (error) => error ? reject(error) : resolve());
        });
    }

    async close(): Promise<void> {
        const port = this.port;
        this.port = undefined;
        this.rejectWaiters(new Error("Serial port closed"));

        if (!port || !port.isOpen) {
            return;
        }

        await new Promise<void>((resolve) => {
            port.set({ dtr: false, rts: false }, () => resolve());
        });
        await new Promise<void>((resolve, reject) => {
            port.close((error) => error ? reject(error) : resolve());
        });
        if (process.platform === "win32") {
            await delay(WIN32_CLOSE_SETTLE_MS);
        }
    }

    async write(data: Buffer | string): Promise<void> {
        const port = this.port;
        if (!port?.isOpen) {
            throw new Error("Serial port is not open");
        }

        const payload = Buffer.isBuffer(data) ? data : Buffer.from(data, "utf8");
        await new Promise<void>((resolve, reject) => {
            port.write(payload, (error) => {
                if (error) {
                    reject(error);
                    return;
                }
                port.drain((drainError) => drainError ? reject(drainError) : resolve());
            });
        });
    }

    clearInput(): void {
        this.rxBuffer = Buffer.alloc(0);
    }

    async flushInput(): Promise<void> {
        const port = this.port;
        this.clearInput();
        if (!port?.isOpen) {
            return;
        }

        await new Promise<void>((resolve, reject) => {
            port.flush((error) => error ? reject(error) : resolve());
        });
        this.clearInput();
    }

    takeInput(): Buffer {
        const data = this.rxBuffer;
        this.rxBuffer = Buffer.alloc(0);
        return data;
    }

    readUntil(marker: Buffer | string, timeoutMs: number): Promise<Buffer> {
        const markerBuffer = Buffer.isBuffer(marker) ? marker : Buffer.from(marker, "utf8");
        const immediate = this.tryConsumeUntil(markerBuffer);
        if (immediate) {
            return Promise.resolve(immediate);
        }

        return new Promise<Buffer>((resolve, reject) => {
            const waiter: ReadWaiter = {
                marker: markerBuffer,
                resolve,
                reject,
                timer: setTimeout(() => {
                    this.waiters = this.waiters.filter((item) => item !== waiter);
                    reject(new Error(`Timed out waiting for ${JSON.stringify(markerBuffer.toString("utf8"))}`));
                }, timeoutMs),
            };
            this.waiters.push(waiter);
        });
    }

    private appendRx(data: Buffer): void {
        const total = this.rxBuffer.length + data.length;
        if (total <= MAX_RX_BUFFER_BYTES) {
            this.rxBuffer = this.rxBuffer.length === 0 ? Buffer.from(data) : Buffer.concat([this.rxBuffer, data]);
            return;
        }
        if (data.length >= MAX_RX_BUFFER_BYTES) {
            this.rxBuffer = Buffer.from(data.subarray(data.length - MAX_RX_BUFFER_BYTES));
            return;
        }
        const keep = MAX_RX_BUFFER_BYTES - data.length;
        this.rxBuffer = Buffer.concat([this.rxBuffer.subarray(this.rxBuffer.length - keep), data]);
    }

    private processWaiters(): void {
        for (const waiter of [...this.waiters]) {
            const data = this.tryConsumeUntil(waiter.marker);
            if (!data) {
                continue;
            }
            clearTimeout(waiter.timer);
            this.waiters = this.waiters.filter((item) => item !== waiter);
            waiter.resolve(data);
        }
    }

    private tryConsumeUntil(marker: Buffer): Buffer | undefined {
        const index = this.rxBuffer.indexOf(marker);
        if (index < 0) {
            return undefined;
        }

        const end = index + marker.length;
        const data = this.rxBuffer.subarray(0, end);
        this.rxBuffer = this.rxBuffer.subarray(end);
        return data;
    }

    private rejectWaiters(error: Error): void {
        for (const waiter of this.waiters) {
            clearTimeout(waiter.timer);
            waiter.reject(error);
        }
        this.waiters = [];
    }
}

function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

function shouldRetryOpen(error: unknown): boolean {
    if (process.platform !== "win32") {
        return false;
    }
    const message = error instanceof Error ? error.message : String(error);
    return message.includes("SetCommState")
        || message.includes("Unknown error code 31")
        || message.includes("Access denied")
        || message.includes("cannot open");
}

function loadSerialPort(): SerialPortConstructor {
    try {
        const moduleName = "serialport";
        return require(moduleName).SerialPort as SerialPortConstructor;
    } catch (error) {
        try {
            return require(path.join(__dirname, "..", "runtime", "node_modules", "serialport")).SerialPort as SerialPortConstructor;
        } catch (runtimeError) {
            const message = runtimeError instanceof Error ? runtimeError.message : String(runtimeError);
            throw new Error(`serialport runtime dependency is missing or failed to load: ${message}`);
        }
    }
}
