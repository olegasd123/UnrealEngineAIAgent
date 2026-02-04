import http from "node:http";
import { z } from "zod";

import {
  PlanOutputSchema,
  SessionApproveRequestSchema,
  SessionNextRequestSchema,
  SessionResumeRequestSchema,
  SessionStartRequestSchema,
  TaskRequestSchema
} from "./contracts.js";
import { config } from "./config.js";
import { CredentialStore } from "./credentials/credentialStore.js";
import { TaskLogStore } from "./logs/taskLogStore.js";
import { createProvider } from "./providers/createProvider.js";
import { SessionStore } from "./sessions/sessionStore.js";

const taskLogStore = new TaskLogStore(config.taskLogPath);
const credentialStore = new CredentialStore();
const sessionStore = new SessionStore();

const ProviderSchema = z.enum(["openai", "gemini"]);
const CredentialSetSchema = z.object({
  provider: ProviderSchema,
  apiKey: z.string().trim().min(1)
});
const CredentialDeleteSchema = z.object({
  provider: ProviderSchema
});
const CredentialTestSchema = z.object({
  provider: ProviderSchema.optional()
});

type ProviderName = z.infer<typeof ProviderSchema>;

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

async function resolveProviderApiKey(provider: ProviderName): Promise<string | undefined> {
  if (provider === "openai") {
    return config.providers.openai.apiKey ?? (await credentialStore.get("openai"));
  }
  return config.providers.gemini.apiKey ?? (await credentialStore.get("gemini"));
}

async function resolveProvider(selected: ProviderName = config.provider) {
  const openaiKey = await resolveProviderApiKey("openai");
  const geminiKey = await resolveProviderApiKey("gemini");
  return createProvider({
    selected,
    openai: {
      ...config.providers.openai,
      apiKey: openaiKey
    },
    gemini: {
      ...config.providers.gemini,
      apiKey: geminiKey
    }
  });
}

async function readProviderStatus() {
  const [openaiKey, geminiKey] = await Promise.all([
    resolveProviderApiKey("openai"),
    resolveProviderApiKey("gemini")
  ]);
  return {
    openai: {
      configured: Boolean(openaiKey),
      model: config.providers.openai.model
    },
    gemini: {
      configured: Boolean(geminiKey),
      model: config.providers.gemini.model
    }
  };
}

const server = http.createServer(async (req, res) => {
  const requestUrl = new URL(req.url ?? "/", `http://${config.host}:${config.port}`);
  const pathname = requestUrl.pathname;

  if (req.method === "GET" && pathname === "/health") {
    try {
      const provider = await resolveProvider();
      return sendJson(res, 200, {
        ok: true,
        provider: provider.name,
        model: provider.model,
        adapter: provider.adapter,
        providerConfigured: provider.hasApiKey
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 500, { ok: false, error: message });
    }
  }

  if (req.method === "GET" && pathname === "/v1/providers/status") {
    try {
      const status = await readProviderStatus();
      return sendJson(res, 200, {
        ok: true,
        selectedProvider: config.provider,
        providers: status
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 500, { ok: false, error: message });
    }
  }

  if (req.method === "POST" && pathname === "/v1/credentials/set") {
    try {
      const rawBody = await readBody(req);
      const parsed = CredentialSetSchema.parse(JSON.parse(rawBody));
      await credentialStore.set(parsed.provider, parsed.apiKey);
      return sendJson(res, 200, { ok: true, provider: parsed.provider, configured: true });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 400, { ok: false, error: message });
    }
  }

  if (req.method === "POST" && pathname === "/v1/credentials/delete") {
    try {
      const rawBody = await readBody(req);
      const parsed = CredentialDeleteSchema.parse(JSON.parse(rawBody));
      await credentialStore.delete(parsed.provider);
      return sendJson(res, 200, { ok: true, provider: parsed.provider, configured: false });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 400, { ok: false, error: message });
    }
  }

  if (req.method === "POST" && pathname === "/v1/credentials/test") {
    try {
      const rawBody = await readBody(req);
      const parsed = CredentialTestSchema.parse(rawBody ? JSON.parse(rawBody) : {});
      const providerName = parsed.provider ?? config.provider;
      const provider = await resolveProvider(providerName);

      if (!provider.hasApiKey) {
        return sendJson(res, 200, {
          ok: false,
          provider: provider.name,
          configured: false,
          message: "API key is not configured."
        });
      }

      const plan = await provider.planTask({
        prompt: "Move selected actors +1 on X",
        mode: "chat",
        context: { selection: ["PreviewActor"] }
      });
      const firstStep = plan.steps[0] ?? "";
      const looksLikeFallback = /using local fallback|api_key is missing|call failed/i.test(firstStep);
      if (looksLikeFallback) {
        return sendJson(res, 200, {
          ok: false,
          provider: provider.name,
          configured: true,
          message: firstStep
        });
      }

      return sendJson(res, 200, {
        ok: true,
        provider: provider.name,
        configured: true,
        message: "Provider call succeeded."
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 200, { ok: false, error: message });
    }
  }

  if (req.method === "GET" && pathname === "/v1/task/logs") {
    const requestedLimit = Number(requestUrl.searchParams.get("limit") ?? "50");
    const normalizedLimit = Number.isFinite(requestedLimit)
      ? Math.max(1, Math.min(50, Math.trunc(requestedLimit)))
      : 50;

    try {
      const entries = await taskLogStore.readLastTaskPlanEntries(normalizedLimit);
      return sendJson(res, 200, {
        ok: true,
        limit: normalizedLimit,
        count: entries.length,
        entries
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 500, { ok: false, error: message });
    }
  }

  if (req.method === "POST" && pathname === "/v1/session/start") {
    try {
      const rawBody = await readBody(req);
      const parsed = SessionStartRequestSchema.parse(JSON.parse(rawBody));
      const provider = await resolveProvider();
      const providerPlan = await provider.planTask(parsed);
      const plan = PlanOutputSchema.parse(providerPlan);
      const decision = sessionStore.create(parsed, plan);
      return sendJson(res, 200, { ok: true, decision });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 400, { ok: false, error: message });
    }
  }

  if (req.method === "POST" && pathname === "/v1/session/next") {
    try {
      const rawBody = await readBody(req);
      const parsed = SessionNextRequestSchema.parse(JSON.parse(rawBody));
      const decision = sessionStore.next(parsed.sessionId, parsed.result);
      return sendJson(res, 200, { ok: true, decision });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 400, { ok: false, error: message });
    }
  }

  if (req.method === "POST" && pathname === "/v1/session/approve") {
    try {
      const rawBody = await readBody(req);
      const parsed = SessionApproveRequestSchema.parse(JSON.parse(rawBody));
      const decision = sessionStore.approve(parsed.sessionId, parsed.actionIndex, parsed.approved);
      return sendJson(res, 200, { ok: true, decision });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 400, { ok: false, error: message });
    }
  }

  if (req.method === "POST" && pathname === "/v1/session/resume") {
    try {
      const rawBody = await readBody(req);
      const parsed = SessionResumeRequestSchema.parse(JSON.parse(rawBody));
      const decision = sessionStore.resume(parsed.sessionId);
      return sendJson(res, 200, { ok: true, decision });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 400, { ok: false, error: message });
    }
  }

  if (req.method === "POST" && pathname === "/v1/task/plan") {
    const requestId = taskLogStore.createRequestId();
    const startedAt = Date.now();
    let rawBody = "";
    let provider: Awaited<ReturnType<typeof resolveProvider>> | undefined;

    try {
      provider = await resolveProvider();
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
        if (provider) {
          await taskLogStore.appendTaskPlanError({
            requestId,
            provider,
            rawBody,
            error: message,
            durationMs: Date.now() - startedAt
          });
        }
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
