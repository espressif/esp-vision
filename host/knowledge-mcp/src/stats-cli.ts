// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { readUsageReport, type UsageDailyMetrics, type UsageMetrics, type UsageReport, type UsageToolMetrics } from "./usage-stats.js";

const MIN_DAYS = 1;
const MAX_DAYS = 3650;

export type StatsReportFormat = "table" | "json";

export interface StatsCliOptions {
  days: number;
  format: StatsReportFormat;
  help: boolean;
}

export function parseStatsCliArgs(args: string[]): StatsCliOptions {
  let days = 7;
  let format: StatsReportFormat = "table";
  let hasDays = false;
  let hasFormat = false;
  let help = false;

  for (let index = 0; index < args.length; index += 1) {
    const argument = args[index];
    if (argument === "--help" || argument === "-h") {
      help = true;
      continue;
    }

    if (argument === "--days" || argument.startsWith("--days=")) {
      if (hasDays) throw new Error("Specify --days only once.");
      hasDays = true;
      const raw = argument === "--days" ? args[++index] : argument.slice("--days=".length);
      if (typeof raw !== "string" || !/^\d+$/.test(raw)) {
        throw new Error("--days must be an integer from 1 through 3650.");
      }
      days = Number(raw);
      if (!Number.isSafeInteger(days) || days < MIN_DAYS || days > MAX_DAYS) {
        throw new Error("--days must be an integer from 1 through 3650.");
      }
      continue;
    }

    if (argument === "--format" || argument.startsWith("--format=")) {
      if (hasFormat) throw new Error("Specify --format only once.");
      hasFormat = true;
      const raw = argument === "--format" ? args[++index] : argument.slice("--format=".length);
      if (raw !== "table" && raw !== "json") {
        throw new Error("--format must be table or json.");
      }
      format = raw;
      continue;
    }

    throw new Error("Unknown command-line argument.");
  }

  return { days, format, help };
}

function formatMs(value: number): string {
  return value.toFixed(2);
}

function formatRate(value: number): string {
  return (value * 100).toFixed(1) + "%";
}

function metricsCells(value: UsageMetrics): string[] {
  return [
    String(value.count),
    String(value.success),
    String(value.error),
    formatRate(value.errorRate),
    formatMs(value.avgDurationMs),
    formatMs(value.maxDurationMs),
  ];
}

function renderDailyRow(row: UsageDailyMetrics): string {
  return [row.date, ...metricsCells(row)].join(" | ");
}

function renderToolRow(row: UsageToolMetrics): string {
  return [row.tool, row.version, ...metricsCells(row)].join(" | ");
}

function renderTable(report: UsageReport): string {
  const lines = [
    "Usage statistics (UTC)",
    "Period: " + report.from + " through " + report.to,
    "",
    "Totals",
    "Count | Success | Error | Error rate | Avg ms | Max ms",
    metricsCells(report.totals).join(" | "),
  ];

  if (report.totals.count === 0) {
    lines.push("", "No usage data was retained for this period.");
    return lines.join("\n");
  }

  lines.push(
    "",
    "Daily",
    "Date | Count | Success | Error | Error rate | Avg ms | Max ms",
  );
  for (const row of report.daily) lines.push(renderDailyRow(row));

  lines.push(
    "",
    "Tools by version",
    "Tool | Version | Count | Success | Error | Error rate | Avg ms | Max ms",
  );
  for (const row of report.tools) lines.push(renderToolRow(row));

  return lines.join("\n");
}

function printHelp(): void {
  process.stdout.write(
    "Usage: node dist/stats-cli.js [--days 1..3650] [--format table|json]\n" +
    "Reads the aggregate SQLite database named by MCP_STATS_DB_PATH.\n",
  );
}

export function main(
  args = process.argv.slice(2),
  env: NodeJS.ProcessEnv = process.env,
): number {
  let options: StatsCliOptions;
  try {
    options = parseStatsCliArgs(args);
  } catch (error) {
    const message = error instanceof Error ? error.message : "Invalid command-line options.";
    process.stderr.write(message + "\n");
    process.stderr.write("Usage: node dist/stats-cli.js [--days 1..3650] [--format table|json]\n");
    return 2;
  }

  if (options.help) {
    printHelp();
    return 0;
  }

  const dbPath = env.MCP_STATS_DB_PATH;
  if (typeof dbPath !== "string" || dbPath.trim().length === 0) {
    process.stderr.write("Usage statistics are disabled: MCP_STATS_DB_PATH is not set.\n");
    return 1;
  }

  let report: UsageReport;
  try {
    report = readUsageReport(dbPath, options.days);
  } catch (error) {
    const message = error instanceof Error
      ? error.message
      : "Usage statistics report is unavailable; verify the configured database is initialized and readable.";
    process.stderr.write(message + "\n");
    return 1;
  }

  if (options.format === "json") {
    process.stdout.write(JSON.stringify(report, null, 2) + "\n");
    if (report.totals.count === 0) {
      process.stderr.write("No usage data was retained for this period.\n");
    }
  } else {
    process.stdout.write(renderTable(report) + "\n");
  }
  return 0;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  process.exitCode = main();
}
