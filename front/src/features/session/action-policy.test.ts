import { describe, expect, it } from 'vitest'

import { tracksGlobalBusyAction } from './action-policy'

describe('global action busy policy', () => {
  it('keeps read-only analysisResult independent from the hardware command busy indicator', () => {
    expect(tracksGlobalBusyAction('analysisResult')).toBe(false)
    expect(tracksGlobalBusyAction('setDigitalStimulus')).toBe(false)
    expect(tracksGlobalBusyAction('resetDigitalStimulus')).toBe(false)
    expect(tracksGlobalBusyAction('stop')).toBe(true)
    expect(tracksGlobalBusyAction('start')).toBe(true)
  })
})
