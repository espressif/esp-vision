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
const ENTER_RAW_REPL_TIMEOUT_MS = 5000;
const REPL_SETTLE_MS = 200;
const INTERRUPT_CTRL_C_COUNT = 3;

function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

export class RawRepl {
    constructor(private readonly transport: SerialTransport) {
    }

    async enter(): Promise<void> {
        let lastError: unknown;
        for (let attempt = 0; attempt < ENTER_RAW_REPL_ATTEMPTS; attempt++) {
            try {
                // Always leave raw REPL first. If the previous run failed while already in raw
                // REPL, another Ctrl-A may not print the "raw REPL" banner and the host would
                // wait until timeout.
                await this.exitToFriendly();
                await this.interruptUserCode();
                await this.transport.flushInput();
                await this.transport.write(CTRL_A);
                await this.transport.readUntil("raw REPL", ENTER_RAW_REPL_TIMEOUT_MS);
                await this.transport.readUntil(">", ENTER_RAW_REPL_TIMEOUT_MS);
                return;
            } catch (error) {
                lastError = error;
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
        await this.transport.flushInput();
        await this.interruptUserCode();
        await this.exitToFriendly();
        this.transport.clearInput();
        await this.transport.write(CTRL_D);
        await delay(500);
    }

    async recoverAfterFailedEnter(): Promise<void> {
        await this.exitToFriendly().catch(() => undefined);
    }

    private async interruptUserCode(): Promise<void> {
        // Multiple Ctrl-C with settle delay covers main.py stuck in long C calls (sensor.reset / skip_frames).
        await this.transport.flushInput();
        for (let i = 0; i < INTERRUPT_CTRL_C_COUNT; i++) {
            await this.transport.write(CTRL_C);
            await delay(REPL_SETTLE_MS);
        }
        await this.transport.flushInput();
    }

    private async exitToFriendly(): Promise<void> {
        // Fallback when Ctrl-A didn't reach raw REPL: device may already be in raw REPL.
        await this.transport.write(CTRL_B);
        await delay(REPL_SETTLE_MS);
        await this.transport.flushInput();
    }
}
