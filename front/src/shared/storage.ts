export function setLocalStorageValue(key: string, value: string): void {
  try {
    window.localStorage.setItem(key, value)
  } catch {
    // file:// storage can be disabled by browser privacy settings.
  }
}

export function removeLocalStorageValue(key: string): void {
  try {
    window.localStorage.removeItem(key)
  } catch {
    // file:// storage can be disabled by browser privacy settings.
  }
}
