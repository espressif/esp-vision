// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0

import { DatabaseSync } from "node:sqlite";

export const TOOL_NAMES = [
  "get_api",
  "get_capabilities",
  "search_docs",
  "list_modules",
  "list_symbols",
  "get_example",
  "list_examples",
  "get_concept",
  "server_info",
] as const;

export type UsageOutcome = "success" | "error";

export interface UsageStatsOptions {
  dbPath?: string;
  retentionDays?: number;
  now?: () => Date;
}

export interface UsageMetrics {
  count: number;
  success: number;
  error: number;
  errorRate: number;
  avgDurationMs: number;
  maxDurationMs: number;
}

export interface UsageDailyMetrics extends UsageMetrics {
  date: string;
}

export interface UsageToolMetrics extends UsageMetrics {
  tool: string;
  version: string;
}

export interface UsageReport {
  timezone: "UTC";
  from: string;
  to: string;
  totals: UsageMetrics;
  daily: UsageDailyMetrics[];
  tools: UsageToolMetrics[];
}

const DEFAULT_RETENTION_DAYS = 90;
const MAX_RETENTION_DAYS = 3650;
const SQLITE_SETUP_BUSY_TIMEOUT_MS = 1500;
const SQLITE_RECORD_BUSY_TIMEOUT_MS = 50;
const SCHEMA_VERSION = 1;
const VERSION_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._+-]{0,79}$/;

const TOOL_NAME_SQL = TOOL_NAMES.map((name) => "'" + name + "'").join(", ");

const CREATE_SCHEMA_SQL = [
  "CREATE TABLE IF NOT EXISTS usage_stats_meta (",
  "  singleton INTEGER NOT NULL PRIMARY KEY CHECK (singleton = 1),",
  "  schema_version INTEGER NOT NULL,",
  "  retention_days INTEGER NOT NULL CHECK (retention_days BETWEEN 1 AND 3650)",
  ");",
  "CREATE TABLE IF NOT EXISTS usage_stats_aggregates (",
  "  utc_day TEXT NOT NULL CHECK (",
  "    length(utc_day) = 10 AND",
  "    utc_day GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]'",
  "  ),",
  "  tool_name TEXT NOT NULL CHECK (tool_name IN (" + TOOL_NAME_SQL + ")),",
  "  pack_version TEXT NOT NULL,",
  "  outcome TEXT NOT NULL CHECK (outcome IN ('success', 'error')),",
  "  execution_count INTEGER NOT NULL CHECK (execution_count > 0),",
  "  total_duration_ms REAL NOT NULL CHECK (total_duration_ms >= 0),",
  "  max_duration_ms REAL NOT NULL CHECK (max_duration_ms >= 0),",
  "  PRIMARY KEY (utc_day, tool_name, pack_version, outcome)",
  ") WITHOUT ROWID;",
].join("\n");

interface AggregateRow {
  utc_day: unknown;
  tool_name: unknown;
  pack_version: unknown;
  outcome: unknown;
  execution_count: unknown;
  total_duration_ms: unknown;
  max_duration_ms: unknown;
}

interface RawMetrics {
  count: number;
  success: number;
  error: number;
  totalDurationMs: number;
  maxDurationMs: number;
}

function isRetentionDays(value: number | undefined): value is number {
  return value !== undefined && Number.isSafeInteger(value) &&
    value >= 1 && value <= MAX_RETENTION_DAYS;
}

function isUsageVersion(value: unknown): value is string {
  return typeof value === "string" && value.length <= 80 && VERSION_PATTERN.test(value);
}

function isToolName(value: unknown): value is (typeof TOOL_NAMES)[number] {
  return typeof value === "string" && (TOOL_NAMES as readonly string[]).includes(value);
}

function dayString(value: Date): string {
  if (!(value instanceof Date) || !Number.isFinite(value.getTime())) {
    throw new Error("Invalid UTC date.");
  }
  return value.toISOString().slice(0, 10);
}

function shiftUtcDay(day: string, offset: number): string {
  const date = new Date(day + "T00:00:00.000Z");
  date.setUTCDate(date.getUTCDate() + offset);
  return dayString(date);
}

function latestDay(left: string, right: string): string {
  return left >= right ? left : right;
}

function metricsAccumulator(): RawMetrics {
  return { count: 0, success: 0, error: 0, totalDurationMs: 0, maxDurationMs: 0 };
}

function addMetrics(
  target: RawMetrics,
  outcome: UsageOutcome,
  count: number,
  totalDurationMs: number,
  maxDurationMs: number,
): void {
  target.count += count;
  if (outcome === "success") target.success += count;
  else target.error += count;
  target.totalDurationMs += totalDurationMs;
  target.maxDurationMs = Math.max(target.maxDurationMs, maxDurationMs);
}

function presentMetrics(value: RawMetrics): UsageMetrics {
  return {
    count: value.count,
    success: value.success,
    error: value.error,
    errorRate: value.count === 0 ? 0 : value.error / value.count,
    avgDurationMs: value.count === 0 ? 0 : value.totalDurationMs / value.count,
    maxDurationMs: value.maxDurationMs,
  };
}

function rollbackQuietly(database: DatabaseSync): void {
  try {
    if (database.isOpen && database.isTransaction) database.exec("ROLLBACK");
  } catch {
    // Preserve the original operation's error.
  }
}

function closeQuietly(database: DatabaseSync | undefined): void {
  try {
    if (database?.isOpen) database.close();
  } catch {
    // Closing a failed or read-only connection must not mask the useful result.
  }
}

export class UsageStats {
  private database: DatabaseSync | undefined;
  private readonly now: () => Date;
  private readonly retentionDays: number;
  private readonly warned = new Set<string>();
  private aggregateStatement: ReturnType<DatabaseSync["prepare"]> | undefined;
  private purgeStatement: ReturnType<DatabaseSync["prepare"]> | undefined;
  private lastPurgeDay: string | undefined;
  private closed = false;

  constructor(options: UsageStatsOptions = {}) {
    this.now = options.now ?? (() => new Date());
    const hasRetentionSetting = options.retentionDays !== undefined;
    const validRetentionSetting = !hasRetentionSetting || isRetentionDays(options.retentionDays);
    this.retentionDays = validRetentionSetting && options.retentionDays !== undefined
      ? options.retentionDays
      : DEFAULT_RETENTION_DAYS;

    const dbPath = options.dbPath;
    if (typeof dbPath !== "string" || dbPath.trim().length === 0) return;
    if (!validRetentionSetting) {
      this.warnOnce("configuration", "MCP usage statistics retention setting is invalid; collection is disabled.");
      return;
    }

    let database: DatabaseSync | undefined;
    try {
      database = new DatabaseSync(dbPath, { timeout: SQLITE_SETUP_BUSY_TIMEOUT_MS });
      this.database = database;
      database.exec("PRAGMA busy_timeout = " + SQLITE_SETUP_BUSY_TIMEOUT_MS);
      database.exec("PRAGMA journal_mode = WAL");
      database.exec("PRAGMA synchronous = NORMAL");
      database.exec(CREATE_SCHEMA_SQL);

      this.aggregateStatement = database.prepare([
        "INSERT INTO usage_stats_aggregates (",
        "  utc_day, tool_name, pack_version, outcome,",
        "  execution_count, total_duration_ms, max_duration_ms",
        ") VALUES (?, ?, ?, ?, 1, ?, ?)",
        "ON CONFLICT (utc_day, tool_name, pack_version, outcome) DO UPDATE SET",
        "  execution_count = execution_count + excluded.execution_count,",
        "  total_duration_ms = total_duration_ms + excluded.total_duration_ms,",
        "  max_duration_ms = MAX(max_duration_ms, excluded.max_duration_ms)",
      ].join("\n"));
      this.purgeStatement = database.prepare(
        "DELETE FROM usage_stats_aggregates WHERE utc_day < ?",
      );

      const today = dayString(this.now());
      database.exec("BEGIN IMMEDIATE");
      try {
        database.prepare([
          "INSERT INTO usage_stats_meta (singleton, schema_version, retention_days)",
          "VALUES (1, ?, ?)",
          "ON CONFLICT (singleton) DO UPDATE SET",
          "  schema_version = excluded.schema_version,",
          "  retention_days = excluded.retention_days",
        ].join("\n")).run(SCHEMA_VERSION, this.retentionDays);
        this.purgeStatement.run(this.retentionStart(today));
        database.exec("COMMIT");
      } catch (error) {
        rollbackQuietly(database);
        throw error;
      }
      database.exec("PRAGMA busy_timeout = " + SQLITE_RECORD_BUSY_TIMEOUT_MS);
      this.lastPurgeDay = today;
    } catch {
      closeQuietly(database);
      this.database = undefined;
      this.warnOnce("storage", "MCP usage statistics storage is unavailable; collection is disabled.");
    }
  }

  /**
   * `version` must come from the trusted loaded Pack.version(), never tool arguments.
   * Returns true only when the aggregate upsert succeeded; false means it was not stored.
   */
  record(tool: string, version: string, outcome: UsageOutcome, durationMs: number): boolean {
    if (this.closed || !this.database) return false;
    if (!isToolName(tool) || !isUsageVersion(version)) return false;
    if (outcome !== "success" && outcome !== "error") return false;
    if (typeof durationMs !== "number" || !Number.isFinite(durationMs) || durationMs < 0) return false;

    let today: string;
    try {
      today = dayString(this.now());
    } catch {
      return false;
    }

    try {
      if (this.lastPurgeDay !== today) {
        this.purgeExpired(today);
        this.lastPurgeDay = today;
      }
      if (!this.aggregateStatement) return false;
      const result = this.aggregateStatement.run(today, tool, version, outcome, durationMs, durationMs);
      return Number(result.changes) === 1;
    } catch {
      this.warnOnce("write", "MCP usage statistics write failed; this execution may be omitted.");
      return false;
    }
  }

  close(): void {
    if (this.closed) return;
    this.closed = true;
    try {
      if (this.database?.isOpen) this.database.close();
    } catch {
      this.warnOnce("close", "MCP usage statistics storage could not be closed cleanly.");
    } finally {
      this.database = undefined;
    }
  }

  private retentionStart(today: string): string {
    return shiftUtcDay(today, -(this.retentionDays - 1));
  }

  private purgeExpired(today: string): void {
    const database = this.database;
    if (!database || !this.purgeStatement) return;

    database.exec("BEGIN IMMEDIATE");
    try {
      this.purgeStatement.run(this.retentionStart(today));
      database.exec("COMMIT");
    } catch (error) {
      rollbackQuietly(database);
      throw error;
    }
  }

  private warnOnce(key: string, message: string): void {
    if (this.warned.has(key)) return;
    this.warned.add(key);
    try {
      console.warn("[mcp-usage-stats] " + message);
    } catch {
      // Warnings must never affect service requests.
    }
  }
}

export function readUsageReport(
  dbPath: string,
  days: number,
  now: () => Date = () => new Date(),
): UsageReport {
  if (!Number.isSafeInteger(days) || days < 1 || days > MAX_RETENTION_DAYS) {
    throw new Error("Report days must be an integer from 1 through 3650.");
  }
  if (typeof dbPath !== "string" || dbPath.trim().length === 0) {
    throw new Error("Usage statistics database path is not configured.");
  }

  let today: string;
  try {
    today = dayString(now());
  } catch {
    throw new Error("Usage statistics report clock is invalid.");
  }
  const requestedFrom = shiftUtcDay(today, -(days - 1));

  let database: DatabaseSync | undefined;
  try {
    database = new DatabaseSync(dbPath, {
      readOnly: true,
      timeout: SQLITE_SETUP_BUSY_TIMEOUT_MS,
    });
    database.exec("PRAGMA busy_timeout = " + SQLITE_SETUP_BUSY_TIMEOUT_MS);
    database.exec("PRAGMA query_only = ON");
    database.exec("BEGIN");

    const metadata = database.prepare([
      "SELECT schema_version, retention_days",
      "FROM usage_stats_meta",
      "WHERE singleton = 1",
    ].join("\n")).get() as {
      schema_version?: unknown;
      retention_days?: unknown;
    } | undefined;

    if (!metadata || metadata.schema_version !== SCHEMA_VERSION ||
        !Number.isSafeInteger(metadata.retention_days) ||
        (metadata.retention_days as number) < 1 ||
        (metadata.retention_days as number) > MAX_RETENTION_DAYS) {
      throw new Error("Usage statistics schema is unavailable.");
    }

    const retainedFrom = shiftUtcDay(today, -((metadata.retention_days as number) - 1));
    const from = latestDay(requestedFrom, retainedFrom);
    const rows = database.prepare([
      "SELECT utc_day, tool_name, pack_version, outcome,",
      "       execution_count, total_duration_ms, max_duration_ms",
      "FROM usage_stats_aggregates",
      "WHERE utc_day >= ? AND utc_day <= ?",
      "ORDER BY utc_day ASC, tool_name ASC, pack_version ASC, outcome ASC",
    ].join("\n")).all(from, today) as unknown as AggregateRow[];

    database.exec("COMMIT");
    return buildReport(from, today, rows);
  } catch {
    if (database) rollbackQuietly(database);
    throw new Error(
      "Usage statistics report is unavailable; verify the configured database is initialized and readable.",
    );
  } finally {
    closeQuietly(database);
  }
}

function buildReport(from: string, to: string, rows: AggregateRow[]): UsageReport {
  const totals = metricsAccumulator();
  const dailyMap = new Map<string, RawMetrics>();
  const toolsMap = new Map<string, { tool: string; version: string; metrics: RawMetrics }>();

  for (const row of rows) {
    if (typeof row.utc_day !== "string" ||
        !/^\d{4}-\d{2}-\d{2}$/.test(row.utc_day) ||
        !isToolName(row.tool_name) ||
        !isUsageVersion(row.pack_version) ||
        (row.outcome !== "success" && row.outcome !== "error")) {
      continue;
    }

    const count = Number(row.execution_count);
    const totalDurationMs = Number(row.total_duration_ms);
    const maxDurationMs = Number(row.max_duration_ms);
    if (!Number.isSafeInteger(count) || count < 1 ||
        !Number.isFinite(totalDurationMs) || totalDurationMs < 0 ||
        !Number.isFinite(maxDurationMs) || maxDurationMs < 0) {
      continue;
    }

    const outcome = row.outcome as UsageOutcome;
    addMetrics(totals, outcome, count, totalDurationMs, maxDurationMs);

    const day = dailyMap.get(row.utc_day) ?? metricsAccumulator();
    addMetrics(day, outcome, count, totalDurationMs, maxDurationMs);
    dailyMap.set(row.utc_day, day);

    const key = row.tool_name + "\u0000" + row.pack_version;
    const tool = toolsMap.get(key) ?? {
      tool: row.tool_name,
      version: row.pack_version,
      metrics: metricsAccumulator(),
    };
    addMetrics(tool.metrics, outcome, count, totalDurationMs, maxDurationMs);
    toolsMap.set(key, tool);
  }

  const daily = [...dailyMap.entries()]
    .sort(([left], [right]) => left.localeCompare(right))
    .map(([date, value]) => ({ date, ...presentMetrics(value) }));

  const tools = [...toolsMap.values()]
    .sort((left, right) =>
      left.tool.localeCompare(right.tool) || left.version.localeCompare(right.version),
    )
    .map(({ tool, version, metrics }) => ({
      tool,
      version,
      ...presentMetrics(metrics),
    }));

  return {
    timezone: "UTC",
    from,
    to,
    totals: presentMetrics(totals),
    daily,
    tools,
  };
}
