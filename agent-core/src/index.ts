import http from "node:http";
import { z } from "zod";

import { AgentService } from "./agent/agentService.js";
import {
  type PlanOutput,
  type TaskRequest,
  ChatCreateRequestSchema,
  ProviderNameSchema,
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
import { normalizeModelRef, type ModelRef } from "./models/modelCatalog.js";
import { ModelPreferenceStore } from "./models/modelPreferenceStore.js";
import { PlanningLayer } from "./planner/planningLayer.js";
import { createProvider } from "./providers/createProvider.js";
import { SessionStore } from "./sessions/sessionStore.js";
import type { SessionDecision, SessionStatus } from "./sessions/sessionTypes.js";
import { ValidationLayer } from "./validator/validationLayer.js";
import { WorldStateCollector } from "./worldState/worldStateCollector.js";

const taskLogStore = new TaskLogStore(config.taskLogPath);
const sessionLogStore = new SessionLogStore(config.taskLogPath);
const chatStore = new ChatStore(config.dbPath);
const modelPreferenceStore = new ModelPreferenceStore(config.dbPath);
const credentialStore = new CredentialStore();
const sessionStore = new SessionStore(config.policy);
const agentService = new AgentService(
  new IntentLayer(),
  new PlanningLayer(new WorldStateCollector()),
  new ValidationLayer(),
  new ExecutionLayer(sessionStore)
);

const ProviderSchema = ProviderNameSchema;
const CredentialSetSchema = z.object({
  provider: ProviderSchema,
  apiKey: z.string().trim().min(1)
});
const CredentialDeleteSchema = z.object({
  provider: ProviderSchema
});
const CredentialTestSchema = z.object({
  provider: ProviderSchema.optional(),
  model: z.string().trim().min(1).optional()
});

type ProviderName = z.infer<typeof ProviderSchema>;

const ModelPreferenceItemSchema = z.object({
  provider: ProviderSchema,
  model: z.string().trim().min(1)
});
const ModelPreferencesSetSchema = z.object({
  models: z.array(ModelPreferenceItemSchema).default([])
});

const MODEL_REQUEST_TIMEOUT_MS = 12_000;

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
  if (provider === "gemini") {
    return config.providers.gemini.apiKey ?? (await credentialStore.get("gemini"));
  }
  return config.providers.local.apiKey ?? (await credentialStore.get("local"));
}

function getOpenAiCompatibleBaseUrl(provider: "openai" | "local"): string {
  if (provider === "openai") {
    return config.providers.openai.baseUrl?.replace(/\/+$/, "") ?? "https://api.openai.com/v1";
  }
  return config.providers.local.baseUrl?.replace(/\/+$/, "") ?? "http://127.0.0.1:1234/v1";
}

function getGeminiBaseUrl(): string {
  return config.providers.gemini.baseUrl?.replace(/\/+$/, "") ?? "https://generativelanguage.googleapis.com/v1beta";
}

function uniqueModels(items: string[]): string[] {
  const seen = new Set<string>();
  for (const item of items) {
    const normalized = item.trim();
    if (!normalized || seen.has(normalized)) {
      continue;
    }
    seen.add(normalized);
  }
  return [...seen].sort((a, b) => a.localeCompare(b));
}

function dedupeModelRefs(items: ModelRef[]): ModelRef[] {
  const seen = new Set<string>();
  const out: ModelRef[] = [];
  for (const item of items) {
    const normalized = normalizeModelRef(item);
    if (!normalized) {
      continue;
    }
    const key = `${normalized.provider}:${normalized.model.toLowerCase()}`;
    if (seen.has(key)) {
      continue;
    }
    seen.add(key);
    out.push(normalized);
  }
  return out;
}

async function fetchOpenAiCompatibleModels(provider: "openai" | "local", apiKey?: string): Promise<string[]> {
  const headers: Record<string, string> = {};
  if (apiKey) {
    headers.Authorization = `Bearer ${apiKey}`;
  }

  const response = await fetch(`${getOpenAiCompatibleBaseUrl(provider)}/models`, {
    method: "GET",
    headers,
    signal: AbortSignal.timeout(MODEL_REQUEST_TIMEOUT_MS)
  });
  if (!response.ok) {
    throw new Error(`${provider} models request failed (${response.status}).`);
  }

  const payload = (await response.json()) as { data?: Array<{ id?: unknown }> };
  const models: string[] = [];
  if (Array.isArray(payload.data)) {
    for (const entry of payload.data) {
      if (typeof entry?.id === "string" && entry.id.trim()) {
        models.push(entry.id.trim());
      }
    }
  }
  return uniqueModels(models);
}

async function fetchGeminiModels(apiKey: string | undefined): Promise<string[]> {
  if (!apiKey) {
    return [];
  }

  const response = await fetch(`${getGeminiBaseUrl()}/models?key=${encodeURIComponent(apiKey)}`, {
    method: "GET",
    signal: AbortSignal.timeout(MODEL_REQUEST_TIMEOUT_MS)
  });
  if (!response.ok) {
    throw new Error(`gemini models request failed (${response.status}).`);
  }

  const payload = (await response.json()) as { models?: Array<{ name?: unknown }> };
  const models: string[] = [];
  if (Array.isArray(payload.models)) {
    for (const entry of payload.models) {
      if (typeof entry?.name !== "string" || !entry.name.trim()) {
        continue;
      }
      const normalized = entry.name.startsWith("models/") ? entry.name.slice("models/".length) : entry.name;
      if (normalized.trim()) {
        models.push(normalized.trim());
      }
    }
  }
  return uniqueModels(models);
}

async function readAvailableModelsForProvider(provider: ProviderName): Promise<{
  provider: ProviderName;
  configured: boolean;
  models: string[];
}> {
  if (provider === "openai") {
    const apiKey = await resolveProviderApiKey("openai");
    if (!apiKey) {
      return { provider, configured: false, models: [] };
    }
    const models = await fetchOpenAiCompatibleModels("openai", apiKey);
    return { provider, configured: true, models };
  }

  if (provider === "gemini") {
    const apiKey = await resolveProviderApiKey("gemini");
    if (!apiKey) {
      return { provider, configured: false, models: [] };
    }
    const models = await fetchGeminiModels(apiKey);
    return { provider, configured: true, models };
  }

  const apiKey = await resolveProviderApiKey("local");
  const models = await fetchOpenAiCompatibleModels("local", apiKey);
  return { provider, configured: true, models };
}

async function resolveRequestedModel(provider: ProviderName, requestedModel?: string): Promise<string | undefined> {
  const requested = requestedModel?.trim();
  if (requested) {
    return requested;
  }

  const preferred = modelPreferenceStore.list().find((item) => item.provider === provider)?.model.trim();
  if (preferred) {
    return preferred;
  }

  return undefined;
}

async function resolveProvider(
  selected: ProviderName = config.provider,
  requestedModel?: string
) {
  const model = (await resolveRequestedModel(selected, requestedModel)) ?? "model-not-selected";
  const [openaiKey, geminiKey, localKey] = await Promise.all([
    resolveProviderApiKey("openai"),
    resolveProviderApiKey("gemini"),
    resolveProviderApiKey("local")
  ]);
  return createProvider({
    selected,
    openai: {
      ...config.providers.openai,
      model,
      apiKey: openaiKey
    },
    gemini: {
      ...config.providers.gemini,
      model,
      apiKey: geminiKey
    },
    local: {
      ...config.providers.local,
      model,
      apiKey: localKey
    }
  });
}

async function readProviderStatus() {
  const [openaiKey, geminiKey, localKey] = await Promise.all([
    resolveProviderApiKey("openai"),
    resolveProviderApiKey("gemini"),
    resolveProviderApiKey("local")
  ]);
  return {
    openai: {
      configured: Boolean(openaiKey)
    },
    gemini: {
      configured: Boolean(geminiKey)
    },
    local: {
      configured: true,
      hasApiKey: Boolean(localKey)
    }
  };
}

function errorStatusCode(error: unknown): number {
  if (error instanceof Error && /was not found/i.test(error.message)) {
    return 404;
  }
  return 400;
}

function parseChatRoute(pathname: string): { chatId: string; isDetails: boolean } | undefined {
  const match = /^\/v1\/chats\/([^/]+)(?:\/(details))?$/.exec(pathname);
  if (!match) {
    return undefined;
  }
  return { chatId: decodeURIComponent(match[1] ?? ""), isDetails: match[2] === "details" };
}

function normalizePromptForDisplay(prompt: string): string {
  const normalized = prompt.replace(/\s+/g, " ").trim();
  return normalized.length > 0 ? normalized : "User prompt";
}

function stripProgressSegment(message: string): string {
  return message.replace(/\s*Progress:\s*\d+\/\d+\s+actions completed\.\s*/gi, " ").replace(/\s+/g, " ").trim();
}

function extractFailedReason(message: string): string | undefined {
  const normalized = stripProgressSegment(message);
  if (!normalized) {
    return undefined;
  }

  if (/Rejected by user\./i.test(normalized)) {
    return "action was rejected.";
  }

  const lastErrorMatch = /Last error:\s*(.+)$/i.exec(normalized);
  if (lastErrorMatch?.[1]) {
    return lastErrorMatch[1].trim();
  }

  const stopConditionMatch = /Stopped by stopCondition=([a-z_]+)\.?/i.exec(normalized);
  if (stopConditionMatch?.[1]) {
    return `stopped by ${stopConditionMatch[1].replace(/_/g, " ")}.`;
  }

  const actionFailedMatch = /Action\s+\d+\s+failed(?:\s+after\s+\d+\s+attempt\(s\))?\.?/i.exec(normalized);
  if (actionFailedMatch?.[0]) {
    return actionFailedMatch[0].trim();
  }

  return normalized;
}

function summarizeSessionMessage(status: SessionStatus, message: string): string | undefined {
  const normalized = message.replace(/\s+/g, " ").trim();
  if (!normalized) {
    return undefined;
  }

  if (status === "completed") {
    return "Completed.";
  }
  if (status === "awaiting_approval") {
    return "Waiting for your approval on the next action.";
  }
  if (status === "failed") {
    const reason = extractFailedReason(normalized);
    return reason ? `Failed: ${reason}` : "Failed.";
  }
  if (status === "ready_to_execute") {
    return "Working on the plan.";
  }

  return normalized;
}

function buildSessionAssistantReply(decision: SessionDecision): { summary: string; text: string } {
  const summary = decision.summary.trim() || "Session updated.";
  const note = summarizeSessionMessage(decision.status, decision.message);
  const text = note ? `${summary}\n${note}` : summary;
  return { summary, text };
}

function buildPlanAssistantReply(plan: PlanOutput): { summary: string; text: string } {
  const summary = plan.summary.trim() || "Plan is ready.";
  const details: string[] = [`Prepared ${plan.actions.length} action(s).`];
  for (const step of plan.steps.slice(0, 3)) {
    details.push(step);
  }
  return {
    summary,
    text: [summary, ...details].join("\n")
  };
}

function buildNoActionFallbackText(plan: PlanOutput): string {
  const summary = plan.summary.trim() || "No action is needed right now.";
  const steps = plan.steps
    .map((step) => step.trim())
    .filter((step) => step.length > 0)
    .slice(0, 3);
  if (steps.length === 0) {
    return summary;
  }
  return [summary, ...steps].join("\n");
}

function formatNoActionReplyPrompt(userPrompt: string, mode: TaskRequest["mode"], plan: PlanOutput): string {
  const planSteps = plan.steps
    .map((step) => step.trim())
    .filter((step) => step.length > 0)
    .slice(0, 5)
    .map((step) => `- ${step}`)
    .join("\n");

  return [
    "User prompt:",
    userPrompt.trim(),
    "",
    "Planner result:",
    `- Mode: ${mode}`,
    `- Actions planned: ${plan.actions.length}`,
    `- Plan summary: ${plan.summary.trim() || "No summary."}`,
    planSteps ? `- Plan steps:\n${planSteps}` : "- Plan steps: none",
    "",
    "Task:",
    "Reply directly to the user in plain English.",
    "If this is an informational question, answer it.",
    "If this asks for scene changes but details are missing, explain what is missing and ask one short question."
  ].join("\n");
}

async function buildNoActionAssistantText(
  prompt: string,
  mode: TaskRequest["mode"],
  plan: PlanOutput,
  provider: Awaited<ReturnType<typeof resolveProvider>>
): Promise<string> {
  if (plan.actions.length > 0) {
    return buildPlanAssistantReply(plan).text;
  }

  try {
    const text = await provider.respondText({
      prompt: formatNoActionReplyPrompt(prompt, mode, plan)
    });
    const normalized = text.trim();
    if (normalized.length > 0) {
      return normalized;
    }
  } catch {
    // Keep fallback below.
  }

  return buildNoActionFallbackText(plan);
}

function appendUserPromptDetail(
  chatId: string,
  route: string,
  prompt: string,
  payload?: Record<string, unknown>
): void {
  const displayPrompt = normalizePromptForDisplay(prompt);
  chatStore.appendAsked(chatId, route, displayPrompt, {
    ...payload,
    displayRole: "user",
    displayText: displayPrompt
  });
}

function appendAssistantDetail(
  chatId: string,
  route: string,
  summary: string,
  text: string,
  payload?: Record<string, unknown>
): void {
  chatStore.appendDone(chatId, route, summary, {
    ...payload,
    displayRole: "assistant",
    displayText: text
  });
}

function shouldStoreSessionDecision(decision: SessionDecision): boolean {
  return decision.status !== "ready_to_execute";
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

  if (req.method === "GET" && pathname === "/v1/models") {
    try {
      const preferredModels = dedupeModelRefs(modelPreferenceStore.list());
      const providerParam = requestUrl.searchParams.get("provider");
      if (!providerParam) {
        return sendJson(res, 200, {
          ok: true,
          preferredModels
        });
      }

      const provider = ProviderSchema.parse(providerParam);
      const providerState = await readAvailableModelsForProvider(provider);
      return sendJson(res, 200, {
        ok: true,
        provider: providerState.provider,
        configured: providerState.configured,
        models: providerState.models,
        preferredModels
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 400, { ok: false, error: message });
    }
  }

  if (req.method === "POST" && pathname === "/v1/models/preferences") {
    try {
      const rawBody = await readBody(req);
      const parsed = ModelPreferencesSetSchema.parse(rawBody ? JSON.parse(rawBody) : {});
      const saved = modelPreferenceStore.replace(parsed.models);
      return sendJson(res, 200, {
        ok: true,
        savedCount: saved.length,
        preferredModels: dedupeModelRefs(saved)
      });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      return sendJson(res, 400, { ok: false, error: message });
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
  if (chatRoute && !chatRoute.isDetails) {
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

  if (chatRoute && chatRoute.isDetails && req.method === "GET") {
    const requestedLimit = Number(requestUrl.searchParams.get("limit") ?? "100");
    const normalizedLimit = Number.isFinite(requestedLimit)
      ? Math.max(1, Math.min(200, Math.trunc(requestedLimit)))
      : 100;
    try {
      const details = chatStore.listDetails(chatRoute.chatId, normalizedLimit);
      return sendJson(res, 200, { ok: true, count: details.length, details });
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
      const selectedModel = await resolveRequestedModel(providerName, parsed.model);
      if (!selectedModel) {
        return sendJson(res, 400, {
          ok: false,
          provider: providerName,
          configured: false,
          message: "No model selected. Select a model in Settings."
        });
      }
      const provider = await resolveProvider(providerName, selectedModel);

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
          context: { selection: ["PreviewActor"] },
          provider: providerName,
          model: selectedModel
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
      const selectedProvider = parsed.provider ?? config.provider;
      const selectedModel = await resolveRequestedModel(selectedProvider, parsed.model);
      if (!selectedModel) {
        return sendJson(res, 400, { ok: false, error: "No model selected. Select a model in Settings." });
      }
      if (parsed.chatId) {
        appendUserPromptDetail(parsed.chatId, "/v1/session/start", parsed.prompt, {
          mode: parsed.mode,
          context: resolvedContext,
          provider: selectedProvider,
          model: selectedModel
        });
      }
      const provider = await resolveProvider(selectedProvider, selectedModel);
      const { decision, plan } = await agentService.startSession(requestWithResolvedContext, provider);
      const noActionAssistantText =
        plan.actions.length === 0
          ? await buildNoActionAssistantText(parsed.prompt, parsed.mode, plan, provider)
          : undefined;

      if (parsed.chatId) {
        if (noActionAssistantText) {
          appendAssistantDetail(parsed.chatId, "/v1/session/start", plan.summary, noActionAssistantText, {
            decision,
            summary: plan.summary,
            actions: plan.actions.length,
            noAction: true
          });
        } else if (shouldStoreSessionDecision(decision)) {
          const assistant = buildSessionAssistantReply(decision);
          appendAssistantDetail(parsed.chatId, "/v1/session/start", assistant.summary, assistant.text, {
            decision
          });
        }
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

      return sendJson(res, 200, { ok: true, requestId, decision, assistantText: noActionAssistantText });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      try {
        const parsedBody = rawBody ? SessionStartRequestSchema.safeParse(JSON.parse(rawBody)) : null;
        if (parsedBody?.success && parsedBody.data.chatId) {
          appendAssistantDetail(
            parsedBody.data.chatId,
            "/v1/session/start",
            "Request failed.",
            `Request failed.\n${message}`,
            { error: message }
          );
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
      const decision = agentService.next(parsed);

      if (parsed.chatId && shouldStoreSessionDecision(decision)) {
        const assistant = buildSessionAssistantReply(decision);
        appendAssistantDetail(parsed.chatId, "/v1/session/next", assistant.summary, assistant.text, {
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
          appendAssistantDetail(
            parsedBody.data.chatId,
            "/v1/session/next",
            "Request failed.",
            `Request failed.\n${message}`,
            { error: message }
          );
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
      const decision = agentService.approve(parsed);

      if (parsed.chatId && shouldStoreSessionDecision(decision)) {
        const assistant = buildSessionAssistantReply(decision);
        appendAssistantDetail(parsed.chatId, "/v1/session/approve", assistant.summary, assistant.text, {
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
          appendAssistantDetail(
            parsedBody.data.chatId,
            "/v1/session/approve",
            "Request failed.",
            `Request failed.\n${message}`,
            { error: message }
          );
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
      const decision = agentService.resume(parsed);

      if (parsed.chatId && shouldStoreSessionDecision(decision)) {
        const assistant = buildSessionAssistantReply(decision);
        appendAssistantDetail(parsed.chatId, "/v1/session/resume", assistant.summary, assistant.text, {
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
          appendAssistantDetail(
            parsedBody.data.chatId,
            "/v1/session/resume",
            "Request failed.",
            `Request failed.\n${message}`,
            { error: message }
          );
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
      rawBody = await readBody(req);
      const parsed = TaskRequestSchema.parse(JSON.parse(rawBody));
      const selectedProvider = parsed.provider ?? config.provider;
      const selectedModel = await resolveRequestedModel(selectedProvider, parsed.model);
      if (!selectedModel) {
        return sendJson(res, 400, { ok: false, error: "No model selected. Select a model in Settings." });
      }
      provider = await resolveProvider(selectedProvider, selectedModel);
      const resolvedContext = resolveContextWithChatMemory(parsed, chatStore);
      const requestWithResolvedContext = {
        ...parsed,
        context: resolvedContext,
        provider: selectedProvider,
        model: selectedModel
      };
      if (parsed.chatId) {
        appendUserPromptDetail(parsed.chatId, "/v1/task/plan", parsed.prompt, {
          mode: parsed.mode,
          context: resolvedContext,
          provider: selectedProvider,
          model: selectedModel
        });
      }
      const { plan } = await agentService.planTask(requestWithResolvedContext, provider);
      const noActionAssistantText =
        plan.actions.length === 0
          ? await buildNoActionAssistantText(parsed.prompt, parsed.mode, plan, provider)
          : undefined;

      if (parsed.chatId) {
        if (noActionAssistantText) {
          appendAssistantDetail(parsed.chatId, "/v1/task/plan", plan.summary, noActionAssistantText, {
            summary: plan.summary,
            actions: plan.actions.length,
            noAction: true
          });
        } else {
          const assistant = buildPlanAssistantReply(plan);
          appendAssistantDetail(parsed.chatId, "/v1/task/plan", assistant.summary, assistant.text, {
            summary: plan.summary,
            actions: plan.actions.length
          });
        }
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

      return sendJson(res, 200, { ok: true, requestId, plan, assistantText: noActionAssistantText });
    } catch (error) {
      const message = error instanceof Error ? error.message : "Unknown error";
      try {
        const parsedBody = rawBody ? TaskRequestSchema.safeParse(JSON.parse(rawBody)) : null;
        if (parsedBody?.success && parsedBody.data.chatId) {
          appendAssistantDetail(
            parsedBody.data.chatId,
            "/v1/task/plan",
            "Planning failed.",
            `Planning failed.\n${message}`,
            { error: message }
          );
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
