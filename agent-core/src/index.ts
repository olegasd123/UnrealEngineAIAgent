import http from "node:http";
import { z } from "zod";

import { AgentService } from "./agent/agentService.js";
import {
  ChatCreateRequestSchema,
  ChatUpdateRequestSchema,
  SessionApproveRequestSchema,
  SessionNextRequestSchema,
  SessionResumeRequestSchema,
  SessionStartRequestSchema,
  TaskRequestSchema
} from "./contracts.js";
import { ChatStore } from "./chats/chatStore.js";
import { resolveContextWithChatMemory } from "./chats/contextMemory.js";
import { config } from "./config.js";
import { CredentialStore } from "./credentials/credentialStore.js";
import { ExecutionLayer } from "./executor/executionLayer.js";
import { IntentLayer } from "./intent/intentLayer.js";
import { SessionLogStore } from "./logs/sessionLogStore.js";
import { TaskLogStore } from "./logs/taskLogStore.js";
import { PlanningLayer } from "./planner/planningLayer.js";
import { createProvider } from "./providers/createProvider.js";
import { SessionStore } from "./sessions/sessionStore.js";
import { ValidationLayer } from "./validator/validationLayer.js";
import { WorldStateCollector } from "./worldState/worldStateCollector.js";

const taskLogStore = new TaskLogStore(config.taskLogPath);
const sessionLogStore = new SessionLogStore(config.taskLogPath);
const chatStore = new ChatStore(config.dbPath);
const credentialStore = new CredentialStore();
const sessionStore = new SessionStore(config.policy);
const agentService = new AgentService(
  new IntentLayer(),
  new PlanningLayer(new WorldStateCollector()),
  new ValidationLayer(),
  new ExecutionLayer(sessionStore)
);

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

function errorStatusCode(error: unknown): number {
  if (error instanceof Error && /was not found/i.test(error.message)) {
    return 404;
  }
  return 400;
}

function parseChatRoute(pathname: string): { chatId: string; isHistory: boolean } | undefined {
  const match = /^\/v1\/chats\/([^/]+)(?:\/(history))?$/.exec(pathname);
  if (!match) {
    return undefined;
  }
  return { chatId: decodeURIComponent(match[1] ?? ""), isHistory: match[2] === "history" };
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

  if (req.method === "POST" && pathname === "/v1/chats") {
    try {
      const rawBody = await readBody(req);
      const parsed = ChatCreateRequestSchema.parse(rawBody ? JSON.parse(rawBody) : {});
      const chat = chatStore.createChat(parsed.title);
      return sendJson(res, 200, { ok: true, chat });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, errorStatusCode(error), { ok: false, error: message });
    }
  }

  if (req.method === "GET" && pathname === "/v1/chats") {
    const includeArchived = requestUrl.searchParams.get("includeArchived") === "true";
    try {
      const chats = chatStore.listChats(includeArchived);
      return sendJson(res, 200, { ok: true, count: chats.length, chats });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, errorStatusCode(error), { ok: false, error: message });
    }
  }

  const chatRoute = parseChatRoute(pathname);
  if (chatRoute && !chatRoute.isHistory) {
    if (req.method === "GET") {
      try {
        const chat = chatStore.getChat(chatRoute.chatId);
        return sendJson(res, 200, { ok: true, chat });
      } catch (error) {
        const message = error instanceof Error ? error.message : "Unknown error";
        return sendJson(res, errorStatusCode(error), { ok: false, error: message });
      }
    }

    if (req.method === "PATCH") {
      try {
        const rawBody = await readBody(req);
        const parsed = ChatUpdateRequestSchema.parse(JSON.parse(rawBody));
        const chat = chatStore.updateChat(chatRoute.chatId, parsed);
        return sendJson(res, 200, { ok: true, chat });
      } catch (error) {
        const message = error instanceof Error ? error.message : "Unknown error";
        return sendJson(res, errorStatusCode(error), { ok: false, error: message });
      }
    }

    if (req.method === "DELETE") {
      try {
        chatStore.deleteChat(chatRoute.chatId);
        return sendJson(res, 200, { ok: true });
      } catch (error) {
        const message = error instanceof Error ? error.message : "Unknown error";
        return sendJson(res, errorStatusCode(error), { ok: false, error: message });
      }
    }
  }

  if (chatRoute && chatRoute.isHistory && req.method === "GET") {
    const requestedLimit = Number(requestUrl.searchParams.get("limit") ?? "100");
    const normalizedLimit = Number.isFinite(requestedLimit)
      ? Math.max(1, Math.min(200, Math.trunc(requestedLimit)))
      : 100;
    try {
      const history = chatStore.listHistory(chatRoute.chatId, normalizedLimit);
      return sendJson(res, 200, { ok: true, count: history.length, history });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, errorStatusCode(error), { ok: false, error: message });
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

      const { plan } = await agentService.planTask(
        {
          prompt: "Move selected actors +1 on X",
          mode: "chat",
          context: { selection: ["PreviewActor"] }
        },
        provider
      );
      const firstStep = plan.steps[0] ?? "";
      const looksLikeFallback = /using local fallback|api_key is missing|call failed|planning failed/i.test(firstStep);
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

  if (req.method === "GET" && pathname === "/v1/session/logs") {
    const requestedLimit = Number(requestUrl.searchParams.get("limit") ?? "50");
    const normalizedLimit = Number.isFinite(requestedLimit)
      ? Math.max(1, Math.min(50, Math.trunc(requestedLimit)))
      : 50;

    try {
      const entries = await sessionLogStore.readLastSessionEntries(normalizedLimit);
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
    const requestId = sessionLogStore.createRequestId();
    const startedAt = Date.now();
    let rawBody = "";

    try {
      rawBody = await readBody(req);
      const parsed = SessionStartRequestSchema.parse(JSON.parse(rawBody));
      const resolvedContext = resolveContextWithChatMemory(parsed, chatStore);
      const requestWithResolvedContext = {
        ...parsed,
        context: resolvedContext
      };
      if (parsed.chatId) {
        chatStore.appendAsked(parsed.chatId, "/v1/session/start", parsed.prompt, {
          mode: parsed.mode,
          context: resolvedContext
        });
      }
      const provider = await resolveProvider();
      const decision = await agentService.startSession(requestWithResolvedContext, provider);

      if (parsed.chatId) {
        chatStore.appendDone(parsed.chatId, "/v1/session/start", `Session ${decision.status}`, {
          decision
        });
      }

      try {
        await sessionLogStore.appendSessionSuccess({
          requestId,
          route: "/v1/session/start",
          request: parsed,
          decision,
          durationMs: Date.now() - startedAt
        });
      } catch (logError) {
        // eslint-disable-next-line no-console
        console.warn("Session log write failed:", logError);
      }

      return sendJson(res, 200, { ok: true, requestId, decision });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      try {
        const parsedBody = rawBody ? SessionStartRequestSchema.safeParse(JSON.parse(rawBody)) : null;
        if (parsedBody?.success && parsedBody.data.chatId) {
          chatStore.appendDone(parsedBody.data.chatId, "/v1/session/start", `Error: ${message}`, { error: message });
        }
      } catch {
        // ignore chat log write/read error
      }

      try {
        await sessionLogStore.appendSessionError({
          requestId,
          route: "/v1/session/start",
          rawBody,
          error: message,
          durationMs: Date.now() - startedAt
        });
      } catch (logError) {
        // eslint-disable-next-line no-console
        console.warn("Session log write failed:", logError);
      }

      return sendJson(res, errorStatusCode(error), { ok: false, requestId, error: message });
    }
  }

  if (req.method === "POST" && pathname === "/v1/session/next") {
    const requestId = sessionLogStore.createRequestId();
    const startedAt = Date.now();
    let rawBody = "";

    try {
      rawBody = await readBody(req);
      const parsed = SessionNextRequestSchema.parse(JSON.parse(rawBody));
      if (parsed.chatId) {
        chatStore.appendAsked(parsed.chatId, "/v1/session/next", `Session ${parsed.sessionId} next`, {
          result: parsed.result
        });
      }
      const decision = agentService.next(parsed);

      if (parsed.chatId) {
        chatStore.appendDone(parsed.chatId, "/v1/session/next", `Session ${decision.status}`, {
          decision
        });
      }

      try {
        await sessionLogStore.appendSessionSuccess({
          requestId,
          route: "/v1/session/next",
          request: parsed,
          decision,
          durationMs: Date.now() - startedAt
        });
      } catch (logError) {
        // eslint-disable-next-line no-console
        console.warn("Session log write failed:", logError);
      }

      return sendJson(res, 200, { ok: true, requestId, decision });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      try {
        const parsedBody = rawBody ? SessionNextRequestSchema.safeParse(JSON.parse(rawBody)) : null;
        if (parsedBody?.success && parsedBody.data.chatId) {
          chatStore.appendDone(parsedBody.data.chatId, "/v1/session/next", `Error: ${message}`, { error: message });
        }
      } catch {
        // ignore chat log write/read error
      }

      try {
        await sessionLogStore.appendSessionError({
          requestId,
          route: "/v1/session/next",
          rawBody,
          error: message,
          durationMs: Date.now() - startedAt
        });
      } catch (logError) {
        // eslint-disable-next-line no-console
        console.warn("Session log write failed:", logError);
      }

      return sendJson(res, errorStatusCode(error), { ok: false, requestId, error: message });
    }
  }

  if (req.method === "POST" && pathname === "/v1/session/approve") {
    const requestId = sessionLogStore.createRequestId();
    const startedAt = Date.now();
    let rawBody = "";

    try {
      rawBody = await readBody(req);
      const parsed = SessionApproveRequestSchema.parse(JSON.parse(rawBody));
      if (parsed.chatId) {
        chatStore.appendAsked(
          parsed.chatId,
          "/v1/session/approve",
          `Session ${parsed.sessionId} action ${parsed.actionIndex} approval=${parsed.approved}`,
          {}
        );
      }
      const decision = agentService.approve(parsed);

      if (parsed.chatId) {
        chatStore.appendDone(parsed.chatId, "/v1/session/approve", `Session ${decision.status}`, {
          decision
        });
      }

      try {
        await sessionLogStore.appendSessionSuccess({
          requestId,
          route: "/v1/session/approve",
          request: parsed,
          decision,
          durationMs: Date.now() - startedAt
        });
      } catch (logError) {
        // eslint-disable-next-line no-console
        console.warn("Session log write failed:", logError);
      }

      return sendJson(res, 200, { ok: true, requestId, decision });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      try {
        const parsedBody = rawBody ? SessionApproveRequestSchema.safeParse(JSON.parse(rawBody)) : null;
        if (parsedBody?.success && parsedBody.data.chatId) {
          chatStore.appendDone(parsedBody.data.chatId, "/v1/session/approve", `Error: ${message}`, { error: message });
        }
      } catch {
        // ignore chat log write/read error
      }

      try {
        await sessionLogStore.appendSessionError({
          requestId,
          route: "/v1/session/approve",
          rawBody,
          error: message,
          durationMs: Date.now() - startedAt
        });
      } catch (logError) {
        // eslint-disable-next-line no-console
        console.warn("Session log write failed:", logError);
      }

      return sendJson(res, errorStatusCode(error), { ok: false, requestId, error: message });
    }
  }

  if (req.method === "POST" && pathname === "/v1/session/resume") {
    const requestId = sessionLogStore.createRequestId();
    const startedAt = Date.now();
    let rawBody = "";

    try {
      rawBody = await readBody(req);
      const parsed = SessionResumeRequestSchema.parse(JSON.parse(rawBody));
      if (parsed.chatId) {
        chatStore.appendAsked(parsed.chatId, "/v1/session/resume", `Session ${parsed.sessionId} resume`, {});
      }
      const decision = agentService.resume(parsed);

      if (parsed.chatId) {
        chatStore.appendDone(parsed.chatId, "/v1/session/resume", `Session ${decision.status}`, {
          decision
        });
      }

      try {
        await sessionLogStore.appendSessionSuccess({
          requestId,
          route: "/v1/session/resume",
          request: parsed,
          decision,
          durationMs: Date.now() - startedAt
        });
      } catch (logError) {
        // eslint-disable-next-line no-console
        console.warn("Session log write failed:", logError);
      }

      return sendJson(res, 200, { ok: true, requestId, decision });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      try {
        const parsedBody = rawBody ? SessionResumeRequestSchema.safeParse(JSON.parse(rawBody)) : null;
        if (parsedBody?.success && parsedBody.data.chatId) {
          chatStore.appendDone(parsedBody.data.chatId, "/v1/session/resume", `Error: ${message}`, { error: message });
        }
      } catch {
        // ignore chat log write/read error
      }

      try {
        await sessionLogStore.appendSessionError({
          requestId,
          route: "/v1/session/resume",
          rawBody,
          error: message,
          durationMs: Date.now() - startedAt
        });
      } catch (logError) {
        // eslint-disable-next-line no-console
        console.warn("Session log write failed:", logError);
      }

      return sendJson(res, errorStatusCode(error), { ok: false, requestId, error: message });
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
      const resolvedContext = resolveContextWithChatMemory(parsed, chatStore);
      const requestWithResolvedContext = {
        ...parsed,
        context: resolvedContext
      };
      if (parsed.chatId) {
        chatStore.appendAsked(parsed.chatId, "/v1/task/plan", parsed.prompt, {
          mode: parsed.mode,
          context: resolvedContext
        });
      }
      const { plan } = await agentService.planTask(requestWithResolvedContext, provider);

      if (parsed.chatId) {
        chatStore.appendDone(parsed.chatId, "/v1/task/plan", "Plan built", {
          summary: plan.summary,
          actions: plan.actions.length
        });
      }

      try {
        await taskLogStore.appendTaskPlanSuccess({
          requestId,
          provider,
          request: requestWithResolvedContext,
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
        const parsedBody = rawBody ? TaskRequestSchema.safeParse(JSON.parse(rawBody)) : null;
        if (parsedBody?.success && parsedBody.data.chatId) {
          chatStore.appendDone(parsedBody.data.chatId, "/v1/task/plan", `Error: ${message}`, { error: message });
        }
      } catch {
        // ignore chat log write/read error
      }

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

      return sendJson(res, errorStatusCode(error), { ok: false, requestId, error: message });
    }
  }

  return sendJson(res, 404, { ok: false, error: "Not found" });
});

server.listen(config.port, config.host, () => {
  // eslint-disable-next-line no-console
  console.log(`Agent Core listening on http://${config.host}:${config.port}`);
  // eslint-disable-next-line no-console
  console.log(`Task log path: ${taskLogStore.getLogPath()}`);
  // eslint-disable-next-line no-console
  console.log(`Session log path: ${sessionLogStore.getLogPath()}`);
  // eslint-disable-next-line no-console
  console.log(`Chat DB path: ${config.dbPath}`);
});
