import type { ProviderRuntimeConfig } from "../config.js";
import type { PlanOutput } from "../contracts.js";
import { buildPlanPrompt, parsePlanOutput } from "../planner/planJson.js";
import type { LlmProvider, PlanInput, TextReplyInput } from "./types.js";

const LOCAL_REQUEST_TIMEOUT_MS = 45_000;
const LOCAL_RETRY_DELAYS_MS = [250, 900];

function formatErrorBody(body: string): string {
  const max = 4000;
  if (body.length <= max) {
    return body;
  }
  return `${body.slice(0, max)}... [truncated ${body.length - max} chars]`;
}

function delayMs(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

function getRetryDelayMs(attempt: number): number {
  return LOCAL_RETRY_DELAYS_MS[attempt] ?? LOCAL_RETRY_DELAYS_MS[LOCAL_RETRY_DELAYS_MS.length - 1] ?? 0;
}

function isRetryableStatus(status: number, body: string): boolean {
  if (status === 408 || status === 425 || status === 429 || status >= 500) {
    return true;
  }

  const normalized = body.toLowerCase();
  return (
    normalized.includes("channel error") ||
    normalized.includes("connection reset") ||
    normalized.includes("socket hang up") ||
    normalized.includes("temporarily unavailable") ||
    normalized.includes("broken pipe")
  );
}

function isRetryableException(error: unknown): boolean {
  if (!(error instanceof Error)) {
    return false;
  }

  const normalized = error.message.toLowerCase();
  return (
    normalized.includes("channel error") ||
    normalized.includes("fetch failed") ||
    normalized.includes("network") ||
    normalized.includes("econnreset") ||
    normalized.includes("socket hang up") ||
    normalized.includes("broken pipe") ||
    normalized.includes("timed out") ||
    normalized.includes("timeout") ||
    normalized.includes("aborted")
  );
}

type LocalChatMessage = {
  role: "system" | "user";
  content: string;
};

export class LocalProvider implements LlmProvider {
  public readonly name = "local";
  public readonly model: string;
  public readonly hasApiKey: boolean;
  public readonly adapter: "stub" | "live";

  constructor(private readonly runtime: ProviderRuntimeConfig) {
    this.model = runtime.model;
    this.hasApiKey = true;
    this.adapter = "live";
  }

  private getBaseUrl(): string {
    return this.runtime.baseUrl?.replace(/\/+$/, "") ?? "http://127.0.0.1:1234/v1";
  }

  private buildHeaders(): Record<string, string> {
    const headers: Record<string, string> = {
      "Content-Type": "application/json"
    };
    if (this.runtime.apiKey) {
      headers.Authorization = `Bearer ${this.runtime.apiKey}`;
    }
    return headers;
  }

  private async requestChatCompletion(messages: LocalChatMessage[]): Promise<string> {
    const endpoint = `${this.getBaseUrl()}/chat/completions`;
    const maxAttempts = LOCAL_RETRY_DELAYS_MS.length + 1;
    let lastError: Error | undefined;

    for (let attempt = 0; attempt < maxAttempts; attempt += 1) {
      const controller = new AbortController();
      const timeout = setTimeout(() => controller.abort(), LOCAL_REQUEST_TIMEOUT_MS);
      try {
        const response = await fetch(endpoint, {
          method: "POST",
          headers: this.buildHeaders(),
          signal: controller.signal,
          body: JSON.stringify({
            model: this.runtime.model,
            temperature: this.runtime.temperature,
            max_tokens: this.runtime.maxTokens,
            messages
          })
        });
        clearTimeout(timeout);

        if (!response.ok) {
          const body = await response.text();
          const requestError = new Error(`Local model request failed (${response.status}): ${formatErrorBody(body)}`);
          if (attempt < maxAttempts - 1 && isRetryableStatus(response.status, body)) {
            lastError = requestError;
            await delayMs(getRetryDelayMs(attempt));
            continue;
          }
          throw requestError;
        }

        let data: {
          choices?: Array<{ message?: { content?: string } }>;
        };
        try {
          data = (await response.json()) as {
            choices?: Array<{ message?: { content?: string } }>;
          };
        } catch (parseError) {
          const jsonError = new Error("Local model response is not valid JSON.");
          if (attempt < maxAttempts - 1) {
            lastError = jsonError;
            await delayMs(getRetryDelayMs(attempt));
            continue;
          }
          throw jsonError;
        }

        const content = data.choices?.[0]?.message?.content;
        if (!content || typeof content !== "string") {
          const responseError = new Error("Local model response did not include completion text.");
          if (attempt < maxAttempts - 1) {
            lastError = responseError;
            await delayMs(getRetryDelayMs(attempt));
            continue;
          }
          throw responseError;
        }

        return content;
      } catch (error) {
        clearTimeout(timeout);
        if (attempt < maxAttempts - 1 && isRetryableException(error)) {
          const retryError = error instanceof Error ? error : new Error("Local model request failed.");
          lastError = retryError;
          await delayMs(getRetryDelayMs(attempt));
          continue;
        }
        throw error;
      }
    }

    throw lastError ?? new Error("Local model request failed.");
  }

  private async requestPlan(input: PlanInput): Promise<PlanOutput> {
    const content = await this.requestChatCompletion([
      {
        role: "system",
        content: "You are a planning assistant for Unreal Editor actions."
      },
      {
        role: "user",
        content: buildPlanPrompt(input)
      }
    ]);
    return parsePlanOutput(content);
  }

  private async requestTextReply(input: TextReplyInput): Promise<string> {
    const content = await this.requestChatCompletion([
      {
        role: "system",
        content:
          "You are an Unreal Engine assistant. Reply in plain text. Keep answers clear, helpful, and safe. Use enough detail to answer well. Use markdown only for headings, bullet lists, bold, italic, inline code, and fenced code blocks. Do not use markdown tables. Do not output JSON."
      },
      {
        role: "user",
        content: input.prompt
      }
    ]);
    return content.trim();
  }

  async planTask(input: PlanInput): Promise<PlanOutput> {
    return this.requestPlan(input);
  }

  async respondText(input: TextReplyInput): Promise<string> {
    return this.requestTextReply(input);
  }
}
