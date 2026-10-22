// SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
// SPDX-License-Identifier: Apache-2.0
//
// Loads the API knowledge pack (produced by the main repo's docs/gen_mcp.py)
// and answers the queries the MCP tools expose. TypeScript port of the original
// pack.py; search uses MiniSearch (pure JS, no native build) instead of sqlite
// FTS5. The pack is plain JSON, so this is fully language-agnostic.

import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { dirname, resolve } from "node:path";
import MiniSearch from "minisearch";

// Accept loose target spellings from AI clients (esp32-p4, p4, ESP32P4, ...).
const TARGET_ALIASES: Record<string, string> = {
  esp32p4: "esp32p4", "esp32-p4": "esp32p4", p4: "esp32p4",
  esp32s3: "esp32s3", "esp32-s3": "esp32s3", s3: "esp32s3",
  esp32s31: "esp32s31", "esp32-s31": "esp32s31", s31: "esp32s31",
};

export function normalizeTarget(target?: string | null): string | undefined {
  if (!target) return undefined;
  const key = target.trim().toLowerCase().replace(/[_ ]/g, "");
  return TARGET_ALIASES[key] ?? key;
}

interface Param { name: string; doc?: string }
interface Symbol {
  kind: string; name: string; signatures: string[];
  params?: Param[]; doc?: string; targets?: string[]; aliasOf?: string;
}
interface Module { name: string; source: string; targets?: string[]; symbols: Symbol[]; mpVersion?: string }
interface DocPage { id: string; kind: string; title: string; bodyMd?: string; codeBlocks?: string[]; targets?: string[] }
interface Example { path: string; title: string; code: string; modules?: string[]; targets?: string[] }
interface Capability { target: string; modules: string[]; boards: string[]; features?: Record<string, unknown> }

type SymbolRec = Symbol & { module: string; source: string; id: string };

interface SearchDoc {
  id: string; ref: string; rtype: string; kind: string; source: string;
  title: string; name: string; text: string; symId?: string; path?: string;
}

export class Pack {
  private packSha256?: string;
  private provenance?: {
    schemaVersion: 1;
    packageName: string;
    packageVersion: string;
    packVersion: string;
    sourceCommit: string;
    packSha256: string;
  };
  private data: any;
  private meta: any;
  modules: Record<string, Module> = {};
  private capabilities: Record<string, Capability> = {};
  private docs: Record<string, DocPage> = {};
  private examples: Example[] = [];
  private symbols: SymbolRec[] = [];
  private byId: Record<string, SymbolRec> = {};
  private byName: Record<string, SymbolRec[]> = {};
  private mini!: MiniSearch<SearchDoc>;
  private searchStore: Record<string, SearchDoc> = {};

  constructor(data: any) {
    this.data = data;
    this.meta = data.meta ?? {};
    for (const m of data.modules ?? []) this.modules[m.name] = m;
    for (const c of data.capabilities ?? []) this.capabilities[c.target] = c;
    for (const d of data.docs ?? []) this.docs[d.id] = d;
    this.examples = data.examples ?? [];

    for (const m of data.modules ?? []) {
      for (const s of m.symbols) {
        const rec: SymbolRec = { ...s, module: m.name, source: m.source, id: `${m.name}.${s.name}` };
        this.symbols.push(rec);
        this.byId[rec.id.toLowerCase()] = rec;
        (this.byName[s.name.toLowerCase()] ??= []).push(rec);
        const bare = s.name.split(".").pop()!.toLowerCase();
        if (bare !== s.name.toLowerCase()) (this.byName[bare] ??= []).push(rec);
      }
    }
    this.buildSearch();
  }

  static load(path: string): Pack {
    const bytes = readFileSync(path);
    const pack = new Pack(JSON.parse(bytes.toString("utf-8")));
    pack.packSha256 = createHash("sha256").update(bytes).digest("hex");
    try {
      const provenance = JSON.parse(readFileSync(resolve(dirname(path), "provenance.json"), "utf-8"));
      if (provenance.schemaVersion === 1 &&
          provenance.packageName === "@esp-vision/knowledge-mcp" &&
          typeof provenance.packageVersion === "string" &&
          provenance.packVersion === pack.version() &&
          provenance.sourceCommit === pack.meta.sourceCommit &&
          provenance.packSha256 === pack.packSha256) {
        pack.provenance = provenance;
      }
    } catch {
      // Source-only packs and developer fixtures do not have npm provenance.
    }
    return pack;
  }

  version(): string {
    return this.meta.version ?? "v0.0.0-dev";
  }

  private buildSearch(): void {
    const documents: SearchDoc[] = [];
    for (const s of this.symbols) {
      documents.push({
        id: `symbol:${s.id}`, ref: `symbol:${s.id}`, rtype: "symbol",
        kind: s.kind, source: s.source, title: s.name, name: s.name,
        text: [s.id, s.name, (s.signatures ?? []).join(" "), s.doc ?? ""].join(" "),
        symId: s.id,
      });
    }
    for (const d of Object.values(this.docs)) {
      documents.push({
        id: `doc:${d.id}`, ref: `doc:${d.id}`, rtype: "doc", kind: d.kind,
        source: "esp-vision", title: d.title, name: d.title,
        text: `${d.title} ${d.bodyMd ?? ""}`,
      });
    }
    for (const e of this.examples) {
      documents.push({
        id: `example:${e.path}`, ref: `example:${e.path}`, rtype: "example",
        kind: "example", source: "esp-vision", title: e.title, name: e.title,
        text: `${e.title} ${(e.modules ?? []).join(" ")} ${e.code ?? ""}`,
        path: e.path,
      });
    }
    for (const d of documents) this.searchStore[d.ref] = d;
    this.mini = new MiniSearch<SearchDoc>({
      fields: ["name", "title", "text"],
      storeFields: ["ref", "rtype", "kind", "source", "title", "symId", "path"],
      searchOptions: { prefix: true, fuzzy: 0.1, combineWith: "OR", boost: { name: 3, title: 2 } },
    });
    this.mini.addAll(documents);
  }

  private canonical(s: SymbolRec): string | null {
    const tmpl: string | undefined = this.meta.canonicalUrlTemplate;
    if (!tmpl || s.source !== "esp-vision") return null;
    return tmpl
      .replace("{lang}", "en")
      .replace("{version}", this.meta.version ?? "latest")
      .replace("{target}", (s.targets ?? ["generic"])[0])
      .replace("{path}", `api-reference/${s.module}.html`)
      .replace("{anchor}", s.aliasOf ?? s.id);
  }

  private symbolView(s: SymbolRec, target?: string): Record<string, unknown> {
    const view: Record<string, unknown> = {
      id: s.id, module: s.module, source: s.source, kind: s.kind, name: s.name,
      signatures: s.signatures ?? [], params: s.params ?? [], doc: s.doc ?? "",
      targets: s.targets ?? [], canonicalUrl: this.canonical(s),
    };
    if (s.aliasOf) view.aliasOf = s.aliasOf;
    if (target) view.available = (s.targets ?? []).includes(target);
    return view;
  }

  getApi(query: string, target?: string, source?: string): Record<string, unknown> {
    target = normalizeTarget(target);
    const q = (query ?? "").trim();
    const ql = q.toLowerCase();

    if (this.modules[q]) {
      const m = this.modules[q];
      let syms = m.symbols;
      if (target) syms = syms.filter((s) => (s.targets ?? []).includes(target!));
      return {
        type: "module", name: m.name, source: m.source, targets: m.targets ?? [],
        symbolCount: syms.length,
        symbols: syms.map((s) => ({ kind: s.kind, name: s.name, signatures: s.signatures ?? [] })),
      };
    }

    let candidates: SymbolRec[] = [];
    const hit = this.byId[ql];
    if (hit) candidates = [hit];
    if (!candidates.length) {
      candidates = [...(this.byName[ql] ?? [])];
      if (!candidates.length && ql.includes(".")) {
        candidates = [...(this.byName[ql.split(".").pop()!] ?? [])];
      }
    }
    if (source) candidates = candidates.filter((c) => c.source === source);
    if (!candidates.length) {
      const sug = this.search(q, undefined, undefined, undefined, 5).map((r: any) => r.ref);
      return { type: "not_found", query: q, suggestions: sug };
    }
    return { type: "symbol", query: q, matches: candidates.map((c) => this.symbolView(c, target)) };
  }

  getCapabilities(name: string): Record<string, unknown> {
    const target = normalizeTarget(name);
    if (target && this.capabilities[target]) return { type: "target", ...this.capabilities[target] };
    for (const cap of Object.values(this.capabilities)) {
      if ((cap.boards ?? []).includes(name)) return { type: "board", board: name, ...cap };
    }
    return { type: "not_found", query: name, knownTargets: Object.keys(this.capabilities) };
  }

  search(query: string, target?: string, kind?: string, source?: string, limit = 10): any[] {
    target = normalizeTarget(target);
    const raw = this.mini.search(query ?? "");
    const out: any[] = [];
    for (const r of raw) {
      const ref = (r as any).ref as string;
      const meta = this.searchStore[ref];
      if (!meta) continue;
      if (kind && meta.rtype !== kind) continue;
      if (source && meta.source !== source) continue;
      const rec: any = { ref, type: meta.rtype, kind: meta.kind, source: meta.source, title: meta.title };
      if (meta.rtype === "symbol" && meta.symId) {
        const s = this.byId[meta.symId.toLowerCase()];
        if (s) {
          if (target && !(s.targets ?? []).includes(target)) continue;
          rec.id = s.id;
          rec.signatures = s.signatures ?? [];
          rec.snippet = (s.doc ?? "").slice(0, 160);
          rec.targets = s.targets ?? [];
        }
      }
      out.push(rec);
      if (out.length >= limit) break;
    }
    return out;
  }

  listModules(source?: string, target?: string): any[] {
    target = normalizeTarget(target);
    const out: any[] = [];
    for (const m of this.data.modules ?? []) {
      if (source && m.source !== source) continue;
      if (target && !(m.targets ?? []).includes(target)) continue;
      out.push({ name: m.name, source: m.source, targets: m.targets ?? [], symbolCount: m.symbols.length });
    }
    return out;
  }

  listSymbols(module: string, target?: string): Record<string, unknown> {
    target = normalizeTarget(target);
    const m = this.modules[module];
    if (!m) return { type: "not_found", module, knownModules: Object.keys(this.modules) };
    let syms = m.symbols;
    if (target) syms = syms.filter((s) => (s.targets ?? []).includes(target!));
    return {
      module, source: m.source,
      symbols: syms.map((s) => ({
        kind: s.kind, name: s.name, signatures: s.signatures ?? [],
        doc: (s.doc ?? "").slice(0, 160),
      })),
    };
  }

  getConcept(topic: string): Record<string, unknown> {
    if (this.docs[topic]) return this.docs[topic] as any;
    for (const [did, d] of Object.entries(this.docs)) {
      if (did.endsWith(`/${topic}`) || d.title.toLowerCase() === topic.toLowerCase()) return d as any;
    }
    const hits = this.search(topic, undefined, "doc", undefined, 5).map((h) => h.ref);
    return { type: "not_found", query: topic, suggestions: hits };
  }

  listExamples(module?: string): any[] {
    return this.examples
      .filter((e) => !module || (e.modules ?? []).includes(module))
      .map((e) => ({ path: e.path, title: e.title, modules: e.modules ?? [] }));
  }

  getExample(query: string): Record<string, unknown> {
    const ql = (query ?? "").toLowerCase();
    for (const e of this.examples) {
      if (e.path.toLowerCase().includes(ql) || e.title.toLowerCase().includes(ql)) return e as any;
    }
    const hits = this.search(query, undefined, "example", undefined, 5);
    if (hits.length) {
      const ref = (hits[0].ref as string).slice("example:".length);
      const e = this.examples.find((x) => x.path === ref);
      if (e) return e as any;
    }
    return { type: "not_found", query };
  }

  serverInfo(options: { npmArtifactSha256?: string } = {}): Record<string, unknown> {
    const info: Record<string, unknown> = {
      name: "esp-vision-mcp",
      version: this.version(),
      sourceCommit: this.meta.sourceCommit,
      packSha256: this.packSha256,
      micropythonVersion: this.meta.micropythonVersion,
      counts: this.meta.counts ?? {},
    };
    if (this.provenance) info.packageVersion = this.provenance.packageVersion;
    if (typeof options.npmArtifactSha256 === "string" && /^[a-f0-9]{64}$/.test(options.npmArtifactSha256)) {
      info.npmArtifactSha256 = options.npmArtifactSha256;
    }
    return info;
  }
}
