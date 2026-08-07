import { error as writeError, info as writeInfo } from "@tauri-apps/plugin-log";
import {
  isPermissionGranted,
  requestPermission,
  sendNotification
} from "@tauri-apps/plugin-notification";
import { load } from "@tauri-apps/plugin-store";
import { getCurrentWindow } from "@tauri-apps/api/window";

export type ColorMode = "dark" | "light" | "system";
export type ResolvedTheme = Exclude<ColorMode, "system">;

export interface DesktopPreferences {
  notificationsEnabled: boolean;
  colorMode: ColorMode;
}

const defaults: DesktopPreferences = {
  notificationsEnabled: true,
  colorMode: "dark"
};

const store = load("desktop-preferences.json", {
  autoSave: 200,
  defaults: { notificationsEnabled: true, colorMode: "dark" }
});

export async function readPreferences(): Promise<DesktopPreferences> {
  const preferences = await (await store).get<DesktopPreferences>("preferences");
  return { ...defaults, ...preferences };
}

export async function savePreferences(preferences: DesktopPreferences): Promise<void> {
  await (await store).set("preferences", preferences);
}

export function resolveTheme(mode: ColorMode): ResolvedTheme {
  if (mode !== "system") return mode;
  return window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
}

export async function applyColorMode(mode: ColorMode): Promise<ResolvedTheme> {
  const theme = resolveTheme(mode);
  document.documentElement.dataset.theme = theme;
  try {
    await getCurrentWindow().setTheme(mode === "system" ? null : mode);
  } catch {
    // Browser-based UI tests do not expose a native window; CSS still updates.
  }
  return theme;
}

export function logInfo(message: string): void {
  void writeInfo(message).catch(() => undefined);
}

export function logError(message: string): void {
  void writeError(message).catch(() => undefined);
}

export async function notify(preferences: DesktopPreferences, title: string, body: string): Promise<void> {
  if (!preferences.notificationsEnabled) return;

  try {
    let granted = await isPermissionGranted();
    if (!granted) granted = (await requestPermission()) === "granted";
    if (granted) sendNotification({ title, body });
  } catch {
    // Notifications are a convenience; a WebView without Tauri plugins remains usable.
  }
}
