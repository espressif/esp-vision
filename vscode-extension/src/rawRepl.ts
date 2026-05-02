/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import { SerialTransport } from "./serialTransport";

const CTRL_A = Buffer.from([0x01]);
const CTRL_B = Buffer.from([0x02]);
const CTRL_C = Buffer.from([0x03]);
const CTRL_D = Buffer.from([0x04]);
const ENTER_RAW_REPL_ATTEMPTS = 4;
const ENTER_RAW_REPL_TIMEOUT_MS = 2500;

function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

export class RawRepl {
    constructor(private readonly transport: SerialTransport) {
    }

    async enter(): Promise<void> {
        await this.interruptUserCode();

        let lastError: unknown;
        for (let attempt = 0; attempt < ENTER_RAW_REPL_ATTEMPTS; attempt++) {
            try {
                await this.transport.flushInput();
                await this.transport.write(CTRL_A);
                await this.transport.readUntil("raw REPL", ENTER_RAW_REPL_TIMEOUT_MS);
                await this.transport.readUntil(">", ENTER_RAW_REPL_TIMEOUT_MS);
                return;
            } catch (error) {
                lastError = error;
                if (await this.trySyncRawPrompt()) {
                    return;
                }
                await this.interruptUserCode();
            }
        }

        throw lastError instanceof Error ? lastError : new Error("Failed to enter raw REPL");
    }

    async exit(): Promise<void> {
        await this.transport.write(CTRL_B);
    }

    async runScript(source: string): Promise<void> {
        await this.enter();
        await this.executeScript(source);
    }

    async executeScript(source: string): Promise<void> {
        await this.transport.flushInput();
        await this.transport.write(source);
        if (!source.endsWith("\n")) {
            await this.transport.write("\n");
        }
        await this.transport.write(CTRL_D);
        await this.transport.readUntil("OK", 3000);
    }

    async stopScript(): Promise<void> {
        await this.interruptUserCode();
    }

    async softReset(): Promise<void> {
        await this.enter();
        this.transport.clearInput();
        await this.transport.write(CTRL_D);
        await delay(500);
    }

    private async interruptUserCode(): Promise<void> {
        await this.transport.flushInput();
        for (let i = 0; i < 4; i++) {
            await this.transport.write(CTRL_C);
            await delay(120);
        }
        await delay(300);
        await this.transport.flushInput();
    }

    private async trySyncRawPrompt(): Promise<boolean> {
        try {
            await this.transport.flushInput();
            await this.transport.write(CTRL_D);
            await this.transport.readUntil("OK", 1000);
            await this.transport.readUntil(">", 1000);
            return true;
        } catch {
            return false;
        }
    }
}
