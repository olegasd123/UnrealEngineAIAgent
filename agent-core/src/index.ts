import http from "node:http";

import { PlanOutputSchema, TaskRequestSchema } from "./contracts.js";
import { config } from "./config.js";
import { MockProvider } from "./providers/mockProvider.js";

const providerName = process.env.AGENT_PROVIDER === "gemini" ? "gemini" : "openai";
const provider = new MockProvider(providerName);

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
    return sendJson(res, 200, { ok: true, provider: provider.name });
  }

  if (req.method === "POST" && req.url === "/v1/task/plan") {
    try {
      const rawBody = await readBody(req);
      const parsed = TaskRequestSchema.parse(JSON.parse(rawBody));
      const providerPlan = await provider.planTask(parsed);
      const plan = PlanOutputSchema.parse(providerPlan);
      return sendJson(res, 200, { ok: true, plan });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 400, { ok: false, error: message });
    }
  }

  return sendJson(res, 404, { ok: false, error: "Not found" });
});

server.listen(config.port, config.host, () => {
  // eslint-disable-next-line no-console
  console.log(`Agent Core listening on http://${config.host}:${config.port}`);
});
