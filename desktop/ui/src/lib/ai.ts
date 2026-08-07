import { invoke } from "@tauri-apps/api/core";

export type IgnoreContextMode = "private" | "precise";

export interface AiProviderStatus {
  endpoint: string;
  model: string;
  jsonMode: boolean;
  keyConfigured: boolean;
}

export interface AiProviderInput {
  endpoint: string;
  model: string;
  jsonMode: boolean;
  apiKey?: string;
}

export interface GeneratedIgnoreRules {
  rules: string[];
  explanation: string;
  provider: string;
  model: string;
}

export const ai = {
  status: () => invoke<AiProviderStatus>("ai_provider_status"),
  configure: (input: AiProviderInput) => invoke<AiProviderStatus>("configure_ai_provider", { input }),
  clearKey: () => invoke<AiProviderStatus>("clear_ai_provider_key"),
  generateIgnoreRules: (taskId: string, description: string, contextMode: IgnoreContextMode) =>
    invoke<GeneratedIgnoreRules>("generate_ignore_rules", { taskId, description, contextMode })
};

export function mergeGeneratedRules(current: string, generated: string[]): string {
  const existing = current.split(/\r?\n/);
  const seen = new Set(existing.map((rule) => rule.trim()).filter(Boolean));
  const additions = generated
    .map((rule) => rule.trim())
    .filter((rule) => {
      if (!rule || seen.has(rule)) return false;
      seen.add(rule);
      return true;
    });
  if (!additions.length) return current;
  const prefix = current.length && !current.endsWith("\n") ? `${current}\n` : current;
  return `${prefix}${prefix ? "\n" : ""}# AI 建议（应用前已由用户确认）\n${additions.join("\n")}\n`;
}
