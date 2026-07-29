import { describe, expect, it } from 'vitest'

import {
  PerformanceNavigationGate,
  isPerformanceCapabilityEnabled,
} from './performance-navigation'

const capability = {
  supported: true,
  analyzerId: 'mbddf.helm.performance',
  schemaVersion: '1',
}

describe('performance navigation', () => {
  it('keeps performance navigation disabled for old or unsupported descriptors', () => {
    expect(isPerformanceCapabilityEnabled({
      supported: false,
      analyzerId: '',
      schemaVersion: '',
    })).toBe(false)
    expect(isPerformanceCapabilityEnabled(capability)).toBe(true)
  })

  it('automatically navigates once only after a successful stop reaches queued work', () => {
    const gate = new PerformanceNavigationGate()
    const identity = { taskId: 'task-a', analysisGeneration: 1 }

    expect(gate.observe(identity, 'queued')).toBeNull()
    gate.recordStopSucceeded(identity)
    expect(gate.observe(identity, 'capturing')).toBeNull()
    expect(gate.observe(identity, 'queued')).toEqual(identity)
    expect(gate.observe(identity, 'calculating')).toBeNull()
    expect(gate.observe(identity, 'completed')).toBeNull()
  })

  it('allows a later successful task to navigate once without reviving its predecessor', () => {
    const gate = new PerformanceNavigationGate()
    const first = { taskId: 'task-a', analysisGeneration: 1 }
    const second = { taskId: 'task-a', analysisGeneration: 2 }
    gate.recordStopSucceeded(first)
    expect(gate.observe(first, 'queued')).toEqual(first)
    gate.recordStopSucceeded(second)
    expect(gate.observe(first, 'persisting')).toBeNull()
    expect(gate.observe(second, 'queued')).toEqual(second)
  })
})
