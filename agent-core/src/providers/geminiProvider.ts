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

export class GeminiProvider implements LlmProvider {
  public readonly name = "gemini";
  public readonly model: string;
  public readonly hasApiKey: boolean;
  public readonly adapter: "stub" | "live";

  constructor(private readonly runtime: ProviderRuntimeConfig) {
    this.model = runtime.model;
    this.hasApiKey = Boolean(runtime.apiKey);
    this.adapter = this.hasApiKey ? "live" : "stub";
  }

  private getBaseUrl(): string {
    return this.runtime.baseUrl?.replace(/\/+$/, "") ?? "https://generativelanguage.googleapis.com/v1beta";
  }

  private async requestPlan(input: PlanInput): Promise<PlanOutput> {
    if (!this.runtime.apiKey) {
      throw new Error("GEMINI_API_KEY is missing.");
    }

    const endpoint =
      `${this.getBaseUrl()}/models/${encodeURIComponent(this.runtime.model)}:generateContent` +
      `?key=${encodeURIComponent(this.runtime.apiKey)}`;

    const response = await fetch(endpoint, {
      method: "POST",
      headers: {
        "Content-Type": "application/json"
      },
      body: JSON.stringify({
        systemInstruction: {
          parts: [{ text: "You are a planning assistant for Unreal Editor actions." }]
        },
        contents: [
          {
            role: "user",
            parts: [{ text: buildPlanPrompt(input) }]
          }
        ],
        generationConfig: {
          temperature: this.runtime.temperature,
          maxOutputTokens: this.runtime.maxTokens,
          responseMimeType: "application/json"
        }
      })
    });

    if (!response.ok) {
      const body = await response.text();
      throw new Error(`Gemini request failed (${response.status}): ${formatErrorBody(body)}`);
    }

    const data = (await response.json()) as {
      candidates?: Array<{ content?: { parts?: Array<{ text?: string }> } }>;
    };
    const content = data.candidates?.[0]?.content?.parts?.[0]?.text;
    if (!content || typeof content !== "string") {
      throw new Error("Gemini response did not include a JSON plan.");
    }

    return parsePlanOutput(content);
  }

  private async requestTextReply(input: TextReplyInput): Promise<string> {
    if (!this.runtime.apiKey) {
      throw new Error("GEMINI_API_KEY is missing.");
    }

    const endpoint =
      `${this.getBaseUrl()}/models/${encodeURIComponent(this.runtime.model)}:generateContent` +
      `?key=${encodeURIComponent(this.runtime.apiKey)}`;

    const response = await fetch(endpoint, {
      method: "POST",
      headers: {
        "Content-Type": "application/json"
      },
      body: JSON.stringify({
        systemInstruction: {
          parts: [
            {
              text: "You are an Unreal Engine assistant. Reply in plain text. Keep answers clear, helpful, and safe. Use enough detail to answer well. Use markdown only for headings, bullet lists, bold, italic, inline code, and fenced code blocks. Do not use markdown tables."
            }
          ]
        },
        contents: [
          {
            role: "user",
            parts: [{ text: input.prompt }]
          }
        ],
        generationConfig: {
          temperature: this.runtime.temperature,
          maxOutputTokens: this.runtime.maxTokens
        }
      })
    });

    if (!response.ok) {
      const body = await response.text();
      throw new Error(`Gemini request failed (${response.status}): ${formatErrorBody(body)}`);
    }

    const data = (await response.json()) as {
      candidates?: Array<{ content?: { parts?: Array<{ text?: string }> } }>;
    };
    const content = data.candidates?.[0]?.content?.parts?.[0]?.text;
    if (!content || typeof content !== "string") {
      throw new Error("Gemini response did not include a text reply.");
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
