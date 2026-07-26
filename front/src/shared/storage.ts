export function setLocalStorageValue(key: string, value: string): void {
  try {
    window.localStorage.setItem(key, value)
  } catch {
    // file:// storage can be disabled by browser privacy settings.
  }
}
