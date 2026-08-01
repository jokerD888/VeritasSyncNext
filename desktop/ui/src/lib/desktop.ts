import { error as writeError, info as writeInfo } from "@tauri-apps/plugin-log";
import {
  isPermissionGranted,
  requestPermission,
  sendNotification
} from "@tauri-apps/plugin-notification";
import { load } from "@tauri-apps/plugin-store";

export interface DesktopPreferences {
  notificationsEnabled: boolean;
}

const defaults: DesktopPreferences = {
  notificationsEnabled: true
};

const store = load("desktop-preferences.json", {
  autoSave: 200,
  defaults: { notificationsEnabled: true }
});

export async function readPreferences(): Promise<DesktopPreferences> {
  const preferences = await (await store).get<DesktopPreferences>("preferences");
  return { ...defaults, ...preferences };
}

export async function savePreferences(preferences: DesktopPreferences): Promise<void> {
  await (await store).set("preferences", preferences);
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
