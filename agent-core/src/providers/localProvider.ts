import type { ProviderRuntimeConfig } from "../config.js";
import type { PlanOutput } from "../contracts.js";
import { buildPlanPrompt, parsePlanOutput } from "../planner/planJson.js";
import type { LlmProvider, PlanInput, TextReplyInput } from "./types.js";

function formatErrorBody(body: string): string {
  const max = 4000;
  if (body.length <= max) {
    return body;
  }
  return `${body.slice(0, max)}... [truncated ${body.length - max} chars]`;
}

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

  private async requestPlan(input: PlanInput): Promise<PlanOutput> {
    const headers: Record<string, string> = {
      "Content-Type": "application/json"
    };
    if (this.runtime.apiKey) {
      headers.Authorization = `Bearer ${this.runtime.apiKey}`;
    }

    const response = await fetch(`${this.getBaseUrl()}/chat/completions`, {
      method: "POST",
      headers,
      body: JSON.stringify({
        model: this.runtime.model,
        temperature: this.runtime.temperature,
        max_tokens: this.runtime.maxTokens,
        messages: [
          {
            role: "system",
            content: "You are a planning assistant for Unreal Editor actions."
          },
          {
            role: "user",
            content: buildPlanPrompt(input)
          }
        ]
      })
    });

    if (!response.ok) {
      const body = await response.text();
      throw new Error(`Local model request failed (${response.status}): ${formatErrorBody(body)}`);
    }

    const data = (await response.json()) as {
      choices?: Array<{ message?: { content?: string } }>;
    };
    const content = data.choices?.[0]?.message?.content;
    if (!content || typeof content !== "string") {
      throw new Error("Local model response did not include a JSON plan.");
    }

    return parsePlanOutput(content);
  }

  private async requestTextReply(input: TextReplyInput): Promise<string> {
    const headers: Record<string, string> = {
      "Content-Type": "application/json"
    };
    if (this.runtime.apiKey) {
      headers.Authorization = `Bearer ${this.runtime.apiKey}`;
    }

    const response = await fetch(`${this.getBaseUrl()}/chat/completions`, {
      method: "POST",
      headers,
      body: JSON.stringify({
        model: this.runtime.model,
        temperature: this.runtime.temperature,
        max_tokens: this.runtime.maxTokens,
        messages: [
          {
            role: "system",
            content:
              "You are an Unreal Engine assistant. Reply in plain text. Keep answers clear, helpful, and safe. Use enough detail to answer well. Use markdown only for headings, bullet lists, bold, italic, inline code, and fenced code blocks. Do not use markdown tables. Do not output JSON."
          },
          {
            role: "user",
            content: input.prompt
          }
        ]
      })
    });

    if (!response.ok) {
      const body = await response.text();
      throw new Error(`Local model request failed (${response.status}): ${formatErrorBody(body)}`);
    }

    const data = (await response.json()) as {
      choices?: Array<{ message?: { content?: string } }>;
    };
    const content = data.choices?.[0]?.message?.content;
    if (!content || typeof content !== "string") {
      throw new Error("Local model response did not include a text reply.");
    }

    return content.trim();
  }

  async planTask(input: PlanInput): Promise<PlanOutput> {
    return this.requestPlan(input);
  }

  async respondText(input: TextReplyInput): Promise<string> {
    return this.requestTextReply(input);
  }
}
