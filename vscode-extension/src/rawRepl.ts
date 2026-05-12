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
const ENTER_RAW_REPL_BUDGET_MS = 15000;
const RAW_REPL_PROBE_TIMEOUT_MS = 3000;
const INTERRUPT_READY_TIMEOUT_MS = 8000;
const REPL_SETTLE_MS = 250;
const INTERRUPT_CTRL_C_COUNT = 3;
const ENTER_INTERRUPT_SEQUENCE = Buffer.from([0x03, 0x03, 0x02]);
const CLEANUP_USER_GLOBALS = `
def __esp_vision_cleanup():
    import gc
    keep = ("__name__", "__doc__", "__package__", "__loader__", "__spec__", "__builtins__", "__esp_vision_cleanup")
    scope = globals()
    for key in tuple(scope):
        if key not in keep:
            value = scope.get(key, None)
            deinit = getattr(value, "deinit", None)
            if deinit:
                try:
                    deinit()
                except Exception:
                    pass
            scope[key] = None
    gc.collect()
    for key in tuple(scope):
        if key not in keep:
            try:
                del scope[key]
            except KeyError:
                pass
    gc.collect()
__esp_vision_cleanup()
del __esp_vision_cleanup
`;

function delay(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
}

export class RawRepl {
    constructor(private readonly transport: SerialTransport) {
    }

    async enter(): Promise<void> {
        const deadline = Date.now() + ENTER_RAW_REPL_BUDGET_MS;
        let lastError: unknown;
        while (Date.now() < deadline) {
            try {
                await this.transport.flushInput();
                await this.transport.write(ENTER_INTERRUPT_SEQUENCE);
                await delay(REPL_SETTLE_MS);
                await this.transport.flushInput();
                await this.transport.write(CTRL_A);
                await this.transport.readUntil("raw REPL", RAW_REPL_PROBE_TIMEOUT_MS);
                await this.transport.readUntil(">", RAW_REPL_PROBE_TIMEOUT_MS);
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

    async cleanupUserGlobals(): Promise<void> {
        await this.transport.flushInput();
        await this.transport.write(CLEANUP_USER_GLOBALS);
        await this.transport.write(CTRL_D);
        await this.transport.readUntil("OK", 3000);
        await this.transport.readUntil(">", 5000);
        await this.transport.flushInput();
    }

    async stopScript(): Promise<void> {
        await this.interruptUserCode({ waitForFriendlyPrompt: false });
    }

    async softReset(): Promise<void> {
        await this.transport.flushInput();
        await this.interruptUserCode({ waitForFriendlyPrompt: true });
        await this.exitToFriendly();
        this.transport.clearInput();
        await this.transport.write(CTRL_D);
        await delay(500);
    }

    async recoverAfterFailedEnter(): Promise<void> {
        await this.exitToFriendly().catch(() => undefined);
    }

    private async interruptUserCode(options: { waitForFriendlyPrompt: boolean }): Promise<void> {
        // Multiple Ctrl-C with settle delay covers main.py stuck in long C calls (sensor.reset / skip_frames).
        await this.transport.flushInput();
        for (let i = 0; i < INTERRUPT_CTRL_C_COUNT; i++) {
            await this.transport.write(CTRL_C);
            await delay(REPL_SETTLE_MS);
        }
        if (options.waitForFriendlyPrompt) {
            await this.transport.readUntil(">>>", INTERRUPT_READY_TIMEOUT_MS).catch(() => undefined);
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
