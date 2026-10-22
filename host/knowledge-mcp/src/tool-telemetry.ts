// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

import type { UsageStats } from "./usage-stats.js";

type ToolHandler<TArgs extends unknown[], TResult> = (...args: TArgs) => TResult | Promise<TResult>;

function isErrorResult(result: unknown): boolean {
  return typeof result === "object" && result !== null &&
    "isError" in result && (result as { isError?: unknown }).isError === true;
}

/**
 * Wrap a registered MCP handler and record one aggregate execution when it
 * resolves or throws. Arguments, result values and thrown errors pass through
 * unchanged; telemetry failures never affect the tool response.
 */
export function measureTool<TArgs extends unknown[], TResult>(
  stats: UsageStats,
  tool: string,
  version: string,
  handler: ToolHandler<TArgs, TResult>,
): (...args: TArgs) => Promise<TResult> {
  return async (...args: TArgs): Promise<TResult> => {
    const startedAt = performance.now();

    const record = (outcome: "success" | "error") => {
      try {
        stats.record(tool, version, outcome, Math.max(0, performance.now() - startedAt));
      } catch {
        // Statistics are best-effort and must never mask tool results or errors.
      }
    };

    try {
      const result = await handler(...args);
      record(isErrorResult(result) ? "error" : "success");
      return result;
    } catch (error) {
      record("error");
      throw error;
    }
  };
}
