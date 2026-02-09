import { randomUUID } from "node:crypto";
import { mkdirSync } from "node:fs";
import { dirname } from "node:path";
import { DatabaseSync } from "node:sqlite";

export type ChatHistoryKind = "asked" | "done";

export interface ChatRecord {
  id: string;
  title: string;
  archived: boolean;
  createdAt: string;
  updatedAt: string;
  lastActivityAt: string;
}

export interface ChatHistoryEntry {
  id: string;
  chatId: string;
  kind: ChatHistoryKind;
  route: string;
  summary: string;
  payload?: unknown;
  createdAt: string;
}

function toIsoNow(): string {
  return new Date().toISOString();
}

function parseJson(raw: string | null): unknown {
  if (!raw) {
    return undefined;
  }
  try {
    return JSON.parse(raw) as unknown;
  } catch {
    return undefined;
  }
}

function stringifyJson(value: unknown): string | null {
  if (value === undefined) {
    return null;
  }
  return JSON.stringify(value);
}

function uniqueStrings(values: string[]): string[] {
  return Array.from(new Set(values.map((value) => value.trim()).filter((value) => value.length > 0)));
}

function readSelectionNamesFromContext(value: unknown): string[] {
  if (!value || typeof value !== "object") {
    return [];
  }

  const context = value as Record<string, unknown>;
  const names: string[] = [];

  const selectionNames = context.selectionNames;
  if (Array.isArray(selectionNames)) {
    for (const item of selectionNames) {
      if (typeof item === "string") {
        names.push(item);
      }
    }
  }

  const selection = context.selection;
  if (Array.isArray(selection)) {
    for (const item of selection) {
      if (typeof item === "string") {
        names.push(item);
        continue;
      }
      if (item && typeof item === "object") {
        const record = item as Record<string, unknown>;
        if (typeof record.name === "string") {
          names.push(record.name);
        }
      }
    }
  }

  return uniqueStrings(names);
}

function readSelectionNamesFromPayload(value: unknown): string[] {
  if (!value || typeof value !== "object") {
    return [];
  }

  const payload = value as Record<string, unknown>;
  return readSelectionNamesFromContext(payload.context);
}

function makeAutoTitleFromSummary(summary: string): string {
  const normalized = summary.replace(/\s+/g, " ").trim();
  if (!normalized) {
    return "New chat";
  }

  const maxLen = 80;
  if (normalized.length <= maxLen) {
    return normalized;
  }
  return `${normalized.slice(0, maxLen - 3).trimEnd()}...`;
}

export class ChatStore {
  private readonly db: DatabaseSync;

  constructor(private readonly dbPath: string) {
    mkdirSync(dirname(dbPath), { recursive: true });
    this.db = new DatabaseSync(dbPath);
    this.db.exec("PRAGMA foreign_keys = ON;");
    this.db.exec(`
      CREATE TABLE IF NOT EXISTS chats (
        id TEXT PRIMARY KEY,
        title TEXT NOT NULL,
        title_auto INTEGER NOT NULL DEFAULT 1,
        archived INTEGER NOT NULL DEFAULT 0,
        created_at TEXT NOT NULL,
        updated_at TEXT NOT NULL,
        last_activity_at TEXT NOT NULL
      );

      CREATE TABLE IF NOT EXISTS chat_details (
        id TEXT PRIMARY KEY,
        chat_id TEXT NOT NULL,
        kind TEXT NOT NULL CHECK(kind IN ('asked', 'done')),
        route TEXT NOT NULL,
        summary TEXT NOT NULL,
        payload_json TEXT,
        created_at TEXT NOT NULL,
        FOREIGN KEY(chat_id) REFERENCES chats(id) ON DELETE CASCADE
      );

      CREATE INDEX IF NOT EXISTS idx_chat_details_chat_created
      ON chat_details(chat_id, created_at DESC);

      CREATE INDEX IF NOT EXISTS idx_chats_last_activity
      ON chats(last_activity_at DESC);
    `);

    this.ensureSchemaMigrations();
  }

  public createChat(title?: string): ChatRecord {
    const id = randomUUID();
    const now = toIsoNow();
    const normalizedTitle = title?.trim() ? title.trim() : "New chat";
    const titleAuto = title?.trim() ? 0 : 1;

    this.db
      .prepare(
        `INSERT INTO chats (id, title, title_auto, archived, created_at, updated_at, last_activity_at)
         VALUES (?, ?, ?, 0, ?, ?, ?)`
      )
      .run(id, normalizedTitle, titleAuto, now, now, now);

    return this.getChat(id);
  }

  public listChats(includeArchived = false): ChatRecord[] {
    const rows = includeArchived
      ? this.db
          .prepare(
            `SELECT id, title, archived, created_at, updated_at, last_activity_at
             FROM chats
             ORDER BY archived ASC, last_activity_at DESC, created_at DESC`
          )
          .all()
      : this.db
          .prepare(
            `SELECT id, title, archived, created_at, updated_at, last_activity_at
             FROM chats
             WHERE archived = 0
             ORDER BY last_activity_at DESC, created_at DESC`
          )
          .all();

    return rows.map((row) => this.mapChatRow(row));
  }

  public getChat(chatId: string): ChatRecord {
    const row = this.db
      .prepare(
        `SELECT id, title, archived, created_at, updated_at, last_activity_at
         FROM chats
         WHERE id = ?`
      )
      .get(chatId);

    if (!row) {
      throw new Error(`Chat ${chatId} was not found.`);
    }

    return this.mapChatRow(row);
  }

  public updateChat(chatId: string, updates: { title?: string; archived?: boolean }): ChatRecord {
    this.getChat(chatId);

    const now = toIsoNow();
    const hasTitle = updates.title !== undefined;
    const hasArchived = updates.archived !== undefined;

    if (!hasTitle && !hasArchived) {
      throw new Error("No chat fields to update.");
    }

    if (hasTitle && hasArchived) {
      const normalizedTitle = updates.title?.trim();
      if (!normalizedTitle) {
        throw new Error("Title must not be empty.");
      }

      this.db
        .prepare(`UPDATE chats SET title = ?, title_auto = 0, archived = ?, updated_at = ? WHERE id = ?`)
        .run(normalizedTitle, updates.archived ? 1 : 0, now, chatId);

      return this.getChat(chatId);
    }

    if (hasTitle) {
      const normalizedTitle = updates.title?.trim();
      if (!normalizedTitle) {
        throw new Error("Title must not be empty.");
      }

      this.db.prepare(`UPDATE chats SET title = ?, title_auto = 0, updated_at = ? WHERE id = ?`).run(normalizedTitle, now, chatId);
      return this.getChat(chatId);
    }

    this.db.prepare(`UPDATE chats SET archived = ?, updated_at = ? WHERE id = ?`).run(updates.archived ? 1 : 0, now, chatId);
    return this.getChat(chatId);
  }

  public deleteChat(chatId: string): void {
    const now = toIsoNow();
    const result = this.db
      .prepare(`UPDATE chats SET archived = 1, updated_at = ? WHERE id = ?`)
      .run(now, chatId);
    if (result.changes === 0) {
      throw new Error(`Chat ${chatId} was not found.`);
    }
  }

  public listHistory(chatId: string, limit = 100): ChatHistoryEntry[] {
    this.getChat(chatId);
    const normalizedLimit = Math.max(1, Math.min(200, Math.trunc(limit)));

    const rows = this.db
      .prepare(
        `SELECT id, chat_id, kind, route, summary, payload_json, created_at
         FROM chat_details
         WHERE chat_id = ?
         ORDER BY created_at ASC
         LIMIT ?`
      )
      .all(chatId, normalizedLimit);

    return rows.map((row) => this.mapHistoryRow(row));
  }

  public appendAsked(chatId: string, route: string, summary: string, payload?: unknown): ChatHistoryEntry {
    this.tryAutoSetTitleFromSummary(chatId, summary);
    return this.appendHistory(chatId, "asked", route, summary, payload);
  }

  public appendDone(chatId: string, route: string, summary: string, payload?: unknown): ChatHistoryEntry {
    return this.appendHistory(chatId, "done", route, summary, payload);
  }

  public getLatestSelectionNames(chatId: string, limit = 30): string[] {
    this.getChat(chatId);
    const normalizedLimit = Math.max(1, Math.min(200, Math.trunc(limit)));
    const rows = this.db
      .prepare(
        `SELECT payload_json
         FROM chat_details
         WHERE chat_id = ? AND kind = 'asked'
         ORDER BY created_at DESC, rowid DESC
         LIMIT ?`
      )
      .all(chatId, normalizedLimit) as Array<{ payload_json?: string | null }>;

    for (const row of rows) {
      const payload = parseJson(row.payload_json ?? null);
      const selectionNames = readSelectionNamesFromPayload(payload);
      if (selectionNames.length > 0) {
        return selectionNames;
      }
    }

    return [];
  }

  private appendHistory(
    chatId: string,
    kind: ChatHistoryKind,
    route: string,
    summary: string,
    payload?: unknown
  ): ChatHistoryEntry {
    this.getChat(chatId);

    const id = randomUUID();
    const now = toIsoNow();

    this.db
      .prepare(
        `INSERT INTO chat_details (id, chat_id, kind, route, summary, payload_json, created_at)
         VALUES (?, ?, ?, ?, ?, ?, ?)`
      )
      .run(id, chatId, kind, route, summary, stringifyJson(payload), now);

    this.db
      .prepare(`UPDATE chats SET updated_at = ?, last_activity_at = ? WHERE id = ?`)
      .run(now, now, chatId);

    const row = this.db
      .prepare(
        `SELECT id, chat_id, kind, route, summary, payload_json, created_at
         FROM chat_details
         WHERE id = ?`
      )
      .get(id);

    return this.mapHistoryRow(row);
  }

  private mapChatRow(row: any): ChatRecord {
    return {
      id: String(row.id),
      title: String(row.title),
      archived: Number(row.archived) === 1,
      createdAt: String(row.created_at),
      updatedAt: String(row.updated_at),
      lastActivityAt: String(row.last_activity_at)
    };
  }

  private mapHistoryRow(row: any): ChatHistoryEntry {
    return {
      id: String(row.id),
      chatId: String(row.chat_id),
      kind: row.kind as ChatHistoryKind,
      route: String(row.route),
      summary: String(row.summary),
      payload: parseJson((row.payload_json as string | null) ?? null),
      createdAt: String(row.created_at)
    };
  }

  private ensureSchemaMigrations(): void {
    const columns = this.db.prepare(`PRAGMA table_info(chats)`).all() as Array<{ name?: string }>;
    const hasTitleAuto = columns.some((column) => String(column.name) === "title_auto");
    if (!hasTitleAuto) {
      this.db.exec(`ALTER TABLE chats ADD COLUMN title_auto INTEGER NOT NULL DEFAULT 1;`);
      this.db.exec(`UPDATE chats SET title_auto = CASE WHEN title = 'New chat' THEN 1 ELSE 0 END;`);
    }
  }

  private tryAutoSetTitleFromSummary(chatId: string, summary: string): void {
    const row = this.db.prepare(`SELECT title_auto FROM chats WHERE id = ?`).get(chatId) as { title_auto?: number } | undefined;
    if (!row || Number(row.title_auto) !== 1) {
      return;
    }

    const autoTitle = makeAutoTitleFromSummary(summary);
    const now = toIsoNow();
    this.db
      .prepare(`UPDATE chats SET title = ?, title_auto = 0, updated_at = ? WHERE id = ?`)
      .run(autoTitle, now, chatId);
  }
}
