import { appendFile, mkdir, readFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { randomUUID } from "node:crypto";

import type { PlanOutput, TaskRequest } from "../contracts.js";
import type { LlmProvider } from "../providers/types.js";

interface TaskPlanLogBase {
  id: string;
  timestamp: string;
  route: "/v1/task/plan";
  durationMs: number;
  provider: {
    name: LlmProvider["name"];
    model: string;
    adapter: LlmProvider["adapter"];
    configured: boolean;
  };
}

interface TaskPlanSuccessLog extends TaskPlanLogBase {
  ok: true;
  request: TaskRequest;
  plan: PlanOutput;
}

interface TaskPlanErrorLog extends TaskPlanLogBase {
  ok: false;
  request: {
    rawBodySample: string;
  };
  error: string;
}

type TaskPlanLogEntry = TaskPlanSuccessLog | TaskPlanErrorLog;

export class TaskLogStore {
  private readonly absolutePath: string;

  constructor(filePath: string) {
    this.absolutePath = resolve(process.cwd(), filePath);
  }

  public createRequestId(): string {
    return randomUUID();
  }

  public getLogPath(): string {
    return this.absolutePath;
  }

  public async appendTaskPlanSuccess(params: {
    requestId: string;
    provider: LlmProvider;
    request: TaskRequest;
    plan: PlanOutput;
    durationMs: number;
  }): Promise<void> {
    const entry: TaskPlanSuccessLog = {
      id: params.requestId,
      timestamp: new Date().toISOString(),
      route: "/v1/task/plan",
      durationMs: params.durationMs,
      provider: {
        name: params.provider.name,
        model: params.provider.model,
        adapter: params.provider.adapter,
        configured: params.provider.hasApiKey
      },
      ok: true,
      request: params.request,
      plan: params.plan
    };
    await this.append(entry);
  }

  public async appendTaskPlanError(params: {
    requestId: string;
    provider: LlmProvider;
    rawBody: string;
    error: string;
    durationMs: number;
  }): Promise<void> {
    const entry: TaskPlanErrorLog = {
      id: params.requestId,
      timestamp: new Date().toISOString(),
      route: "/v1/task/plan",
      durationMs: params.durationMs,
      provider: {
        name: params.provider.name,
        model: params.provider.model,
        adapter: params.provider.adapter,
        configured: params.provider.hasApiKey
      },
      ok: false,
      request: {
        rawBodySample: params.rawBody.slice(0, 4000)
      },
      error: params.error
    };
    await this.append(entry);
  }

  public async readLastTaskPlanEntries(limit: number): Promise<TaskPlanLogEntry[]> {
    const normalizedLimit = Math.max(1, Math.min(50, Math.trunc(limit)));

    let content = "";
    try {
      content = await readFile(this.absolutePath, "utf8");
    } catch (error) {
      const code = (error as NodeJS.ErrnoException).code;
      if (code === "ENOENT") {
        return [];
      }
      throw error;
    }

    const lines = content.split("\n");
    const entries: TaskPlanLogEntry[] = [];
    for (let i = lines.length - 1; i >= 0 && entries.length < normalizedLimit; i -= 1) {
      const line = lines[i]?.trim();
      if (!line) {
        continue;
      }

      try {
        entries.push(JSON.parse(line) as TaskPlanLogEntry);
      } catch {
        // Skip broken lines and keep reading.
      }
    }

    entries.reverse();
    return entries;
  }

  private async append(entry: TaskPlanLogEntry): Promise<void> {
    await mkdir(dirname(this.absolutePath), { recursive: true });
    await appendFile(this.absolutePath, `${JSON.stringify(entry)}\n`, "utf8");
  }
}
