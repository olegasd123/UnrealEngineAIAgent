import http from "node:http";

import { PlanOutputSchema, TaskRequestSchema } from "./contracts.js";
import { config } from "./config.js";
import { TaskLogStore } from "./logs/taskLogStore.js";
import { createProvider } from "./providers/createProvider.js";

const provider = createProvider({
  selected: config.provider,
  openai: config.providers.openai,
  gemini: config.providers.gemini
});
const taskLogStore = new TaskLogStore(config.taskLogPath);

function sendJson(res: http.ServerResponse, statusCode: number, body: unknown): void {
  res.writeHead(statusCode, { "Content-Type": "application/json" });
  res.end(JSON.stringify(body));
}

async function readBody(req: http.IncomingMessage): Promise<string> {
  const chunks: Buffer[] = [];
  for await (const chunk of req) {
    chunks.push(Buffer.from(chunk));
  }
  return Buffer.concat(chunks).toString("utf8");
}

const server = http.createServer(async (req, res) => {
  if (req.method === "GET" && req.url === "/health") {
    return sendJson(res, 200, {
      ok: true,
      provider: provider.name,
      model: provider.model,
      adapter: provider.adapter,
      providerConfigured: provider.hasApiKey
    });
  }

  if (req.method === "POST" && req.url === "/v1/task/plan") {
    const requestId = taskLogStore.createRequestId();
    const startedAt = Date.now();
    let rawBody = "";

    try {
      rawBody = await readBody(req);
      const parsed = TaskRequestSchema.parse(JSON.parse(rawBody));
      const providerPlan = await provider.planTask(parsed);
      const plan = PlanOutputSchema.parse(providerPlan);

      try {
        await taskLogStore.appendTaskPlanSuccess({
          requestId,
          provider,
          request: parsed,
          plan,
          durationMs: Date.now() - startedAt
        });
      } catch (logError) {
        // eslint-disable-next-line no-console
        console.warn("Task log write failed:", logError);
      }

      return sendJson(res, 200, { ok: true, requestId, plan });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";

      try {
        await taskLogStore.appendTaskPlanError({
          requestId,
          provider,
          rawBody,
          error: message,
          durationMs: Date.now() - startedAt
        });
      } catch (logError) {
        // eslint-disable-next-line no-console
        console.warn("Task log write failed:", logError);
      }

      return sendJson(res, 400, { ok: false, requestId, error: message });
    }
  }

  return sendJson(res, 404, { ok: false, error: "Not found" });
});

server.listen(config.port, config.host, () => {
  // eslint-disable-next-line no-console
  console.log(`Agent Core listening on http://${config.host}:${config.port}`);
  // eslint-disable-next-line no-console
  console.log(`Task log path: ${taskLogStore.getLogPath()}`);
});
