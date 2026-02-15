import { mkdirSync } from "node:fs";
import { dirname } from "node:path";
import { DatabaseSync } from "node:sqlite";

import type { ProviderName } from "../config.js";
import type { ModelRef } from "./modelCatalog.js";
import { normalizeModelRef } from "./modelCatalog.js";

function toIsoNow(): string {
  return new Date().toISOString();
}

function uniqueModelRefs(items: ModelRef[]): ModelRef[] {
  const seen = new Set<string>();
  const out: ModelRef[] = [];
  for (const item of items) {
    const normalized = normalizeModelRef(item);
    if (!normalized) {
      continue;
    }

    const key = `${normalized.provider}:${normalized.model}`;
    if (seen.has(key)) {
      continue;
    }
    seen.add(key);
    out.push(normalized);
  }
  return out;
}

export class ModelPreferenceStore {
  private readonly db: DatabaseSync;

  constructor(dbPath: string) {
    mkdirSync(dirname(dbPath), { recursive: true });
    this.db = new DatabaseSync(dbPath);
    this.db.exec(`
      CREATE TABLE IF NOT EXISTS preferred_models (
        provider TEXT NOT NULL CHECK(provider IN ('openai', 'gemini', 'local')),
        model TEXT NOT NULL,
        created_at TEXT NOT NULL,
        PRIMARY KEY(provider, model)
      );
    `);
  }

  public list(): ModelRef[] {
    const rows = this.db
      .prepare(`SELECT provider, model FROM preferred_models ORDER BY created_at ASC`)
      .all() as Array<{ provider: ProviderName; model: string }>;
    return uniqueModelRefs(rows.map((row) => ({ provider: row.provider, model: row.model })));
  }

  public replace(items: ModelRef[]): ModelRef[] {
    const normalized = uniqueModelRefs(items);
    const now = toIsoNow();

    this.db.exec("BEGIN");
    try {
      this.db.prepare(`DELETE FROM preferred_models`).run();
      const insert = this.db.prepare(
        `INSERT INTO preferred_models (provider, model, created_at) VALUES (?, ?, ?)`
      );
      for (const item of normalized) {
        insert.run(item.provider, item.model, now);
      }
      this.db.exec("COMMIT");
    } catch (error) {
      this.db.exec("ROLLBACK");
      throw error;
    }

    return normalized;
  }
}
