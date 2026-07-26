import { describe, expect, it } from 'vitest'

import { normalizeTheme } from './theme'

describe('theme normalization', () => {
  it('keeps the persisted light preference', () => {
    expect(normalizeTheme('light')).toBe('light')
  })

  it('uses the established dark theme for unknown values', () => {
    expect(normalizeTheme('dark')).toBe('dark')
    expect(normalizeTheme('system')).toBe('dark')
    expect(normalizeTheme(null)).toBe('dark')
  })
})
