import { appendFile, mkdir, readFile } from "node:fs/promises";
import { randomUUID } from "node:crypto";

import type {
  SessionApproveRequest,
  SessionNextRequest,
  SessionResumeRequest,
  SessionStartRequest
} from "../contracts.js";
import type { SessionDecision } from "../sessions/sessionStore.js";
import { buildDailyLogPath, resolveLogDirectory } from "./dailyLogPath.js";

type SessionRoute =
  | "/v1/session/start"
  | "/v1/session/next"
  | "/v1/session/approve"
  | "/v1/session/resume";

interface SessionLogBase {
  id: string;
  timestamp: string;
  route: SessionRoute;
  durationMs: number;
}

interface SessionSuccessLog extends SessionLogBase {
  ok: true;
  request:
    | SessionStartRequest
    | SessionNextRequest
    | SessionApproveRequest
    | SessionResumeRequest;
  decision: SessionDecision;
}

interface SessionErrorLog extends SessionLogBase {
  ok: false;
  request: {
    rawBodySample: string;
  };
  error: string;
}

type SessionLogEntry = SessionSuccessLog | SessionErrorLog;

export class SessionLogStore {
  private readonly logDirectory: string;

  constructor(pathOrDirectory: string) {
    this.logDirectory = resolveLogDirectory(pathOrDirectory);
  }

  public createRequestId(): string {
    return randomUUID();
  }

  public getLogPath(): string {
    return this.getCurrentLogPath();
  }

  public async appendSessionSuccess(params: {
    requestId: string;
    route: SessionRoute;
    request:
      | SessionStartRequest
      | SessionNextRequest
      | SessionApproveRequest
      | SessionResumeRequest;
    decision: SessionDecision;
    durationMs: number;
  }): Promise<void> {
    const entry: SessionSuccessLog = {
      id: params.requestId,
      timestamp: new Date().toISOString(),
      route: params.route,
      durationMs: params.durationMs,
      ok: true,
      request: params.request,
      decision: params.decision
    };
    await this.append(entry);
  }

  public async appendSessionError(params: {
    requestId: string;
    route: SessionRoute;
    rawBody: string;
    error: string;
    durationMs: number;
  }): Promise<void> {
    const entry: SessionErrorLog = {
      id: params.requestId,
      timestamp: new Date().toISOString(),
      route: params.route,
      durationMs: params.durationMs,
      ok: false,
      request: {
        rawBodySample: params.rawBody.slice(0, 4000)
      },
      error: params.error
    };
    await this.append(entry);
  }

  public async readLastSessionEntries(limit: number): Promise<SessionLogEntry[]> {
    const normalizedLimit = Math.max(1, Math.min(50, Math.trunc(limit)));
    const logPath = this.getCurrentLogPath();

    let content = "";
    try {
      content = await readFile(logPath, "utf8");
    } catch (error) {
      const code = (error as NodeJS.ErrnoException).code;
      if (code === "ENOENT") {
        return [];
      }
      throw error;
    }

    const lines = content.split("\n");
    const entries: SessionLogEntry[] = [];
    for (let i = lines.length - 1; i >= 0 && entries.length < normalizedLimit; i -= 1) {
      const line = lines[i]?.trim();
      if (!line) {
        continue;
      }

      try {
        entries.push(JSON.parse(line) as SessionLogEntry);
      } catch {
        // Skip broken lines and keep reading.
      }
    }

    entries.reverse();
    return entries;
  }

  private async append(entry: SessionLogEntry): Promise<void> {
    const logPath = this.getCurrentLogPath();
    await mkdir(this.logDirectory, { recursive: true });
    await appendFile(logPath, `${JSON.stringify(entry)}\n`, "utf8");
  }

  private getCurrentLogPath(): string {
    return buildDailyLogPath(this.logDirectory, "session");
  }
}

