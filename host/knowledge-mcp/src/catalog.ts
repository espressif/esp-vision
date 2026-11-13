// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { dirname, resolve } from "node:path";
import { Pack } from "./pack.js";

export interface SnapshotIdentity {
  firmwareVersion: string;
  snapshotVersion: string;
  firmwareSourceCommit: string;
  contentSourceCommit: string;
  packSha256: string;
}

export class KnowledgeVersionError extends Error {
  constructor(public code: string, message: string, public availableVersions: string[]) { super(message); }
}

/** Immutable per-request selection; no mutable current version or latest fallback. */
export class KnowledgeCatalog {
  private readonly snapshots = new Map<string, { identity: SnapshotIdentity; pack: Pack }>();
  private readonly active = new Map<string, string>();
  readonly sha256?: string;
  private provenance?: { packageVersion: string; sourceCommit: string };

  constructor(records: Array<{ identity: SnapshotIdentity; pack: Pack }>, active: Record<string, string>, sha256?: string, readonly latestFirmwareVersion?: string) {
    if (!records.length) throw new Error("Knowledge catalog is empty");
    const firmwareCommits = new Map<string, string>();
    for (const { identity, pack } of records) {
      if (!/^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$/.test(identity.firmwareVersion) ||
          !/^[A-Za-z0-9][A-Za-z0-9._+-]{0,127}$/.test(identity.snapshotVersion) ||
          !/^[a-f0-9]{40}$/.test(identity.firmwareSourceCommit) ||
          !/^[a-f0-9]{40}$/.test(identity.contentSourceCommit) ||
          !/^[a-f0-9]{64}$/.test(identity.packSha256) ||
          pack.version() !== identity.firmwareVersion ||
          (pack.serverInfo() as { sourceCommit: string }).sourceCommit !== identity.contentSourceCommit ||
          this.snapshots.has(identity.snapshotVersion)) throw new Error("Invalid or duplicate knowledge snapshot");
      const commit = firmwareCommits.get(identity.firmwareVersion);
      if (commit && commit !== identity.firmwareSourceCommit) throw new Error("Firmware version maps to conflicting commits");
      firmwareCommits.set(identity.firmwareVersion, identity.firmwareSourceCommit);
      this.snapshots.set(identity.snapshotVersion, { identity: Object.freeze({ ...identity }), pack });
    }
    for (const [firmware, snapshot] of Object.entries(active)) {
      if (this.snapshots.get(snapshot)?.identity.firmwareVersion !== firmware) throw new Error("Invalid active snapshot");
      this.active.set(firmware, snapshot);
    }
    if (records.some(record => !this.active.has(record.identity.firmwareVersion))) throw new Error("Missing active snapshot");
    if (!latestFirmwareVersion || latestFirmwareVersion === "latest" || !this.active.has(latestFirmwareVersion)) throw new Error("Latest firmware must identify an active snapshot");
    this.sha256 = sha256;
  }

  static fromPack(pack: Pack): KnowledgeCatalog {
    const info = pack.serverInfo() as { sourceCommit: string; packSha256?: string };
    const identity = { firmwareVersion: pack.version(), snapshotVersion: pack.version(),
      firmwareSourceCommit: info.sourceCommit, contentSourceCommit: info.sourceCommit,
      packSha256: info.packSha256! };
    return new KnowledgeCatalog([{ identity, pack }], { [pack.version()]: pack.version() }, undefined, pack.version());
  }

  static load(path: string): KnowledgeCatalog {
    const bytes = readFileSync(path);
    if (bytes.length > 64 * 1024 * 1024) throw new Error("Knowledge catalog exceeds 64 MiB");
    const catalog = JSON.parse(bytes.toString("utf8"));
    if (catalog.schemaVersion !== 1 || !Array.isArray(catalog.snapshots) ||
        !catalog.activeSnapshots || typeof catalog.activeSnapshots !== "object" || Array.isArray(catalog.activeSnapshots)) {
      throw new Error("Invalid knowledge catalog schema");
    }
    const records = catalog.snapshots.map((entry: SnapshotIdentity & { packJson: string }) => {
      if (typeof entry.packJson !== "string" || createHash("sha256").update(entry.packJson).digest("hex") !== entry.packSha256) {
        throw new Error("Knowledge snapshot integrity mismatch");
      }
      const { packJson, ...identity } = entry;
      return { identity, pack: new Pack(JSON.parse(packJson)) };
    });
    const result = new KnowledgeCatalog(records, catalog.activeSnapshots, createHash("sha256").update(bytes).digest("hex"), catalog.latestFirmwareVersion);
    // Production catalogs carry runtime provenance separately from snapshot commits.
    const provenancePath = resolve(dirname(path), "provenance.json");
    let provenance;
    try { provenance = JSON.parse(readFileSync(provenancePath, "utf8")); }
    catch (error) { if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error; }
    if (provenance) {
      if (provenance.schemaVersion !== 2 || provenance.catalogSha256 !== result.sha256 ||
          provenance.packageName !== "@esp-vision/knowledge-mcp" ||
          !/^\d+\.\d+\.\d+$/.test(provenance.packageVersion) ||
          !/^[a-f0-9]{40}$/.test(provenance.sourceCommit)) throw new Error("Invalid catalog provenance");
      result.provenance = provenance;
    }
    return result;
  }

  serverInfo(npmArtifactSha256?: string) {
    if (npmArtifactSha256 !== undefined && !/^[a-f0-9]{64}$/.test(npmArtifactSha256)) throw new Error("Invalid npm artifact SHA256");
    return { name: "esp-vision-mcp", schemaVersion: 2,
      packageVersion: this.provenance?.packageVersion, sourceCommit: this.provenance?.sourceCommit,
      catalogSha256: this.sha256, npmArtifactSha256, latestFirmwareVersion: this.latestFirmwareVersion, snapshots: this.list() };
  }

  versions(): string[] { return [...this.active.keys()].sort(); }
  list() {
    return [...this.snapshots.values()].map(({ identity }) => ({ ...identity,
      active: this.active.get(identity.firmwareVersion) === identity.snapshotVersion }));
  }
  select(firmwareVersion?: string, snapshotVersion?: string) {
    if (!firmwareVersion) throw new KnowledgeVersionError("FIRMWARE_VERSION_REQUIRED", "Read the device firmware version or select an explicit target version.", this.versions());
    const selected = this.snapshots.get(snapshotVersion ?? this.active.get(firmwareVersion) ?? "");
    if (!selected || selected.identity.firmwareVersion !== firmwareVersion) {
      throw new KnowledgeVersionError("KNOWLEDGE_VERSION_UNAVAILABLE", "No matching firmware knowledge snapshot; automatic version fallback is disabled; select an available firmware version explicitly.", this.versions());
    }
    return selected;
  }
}
