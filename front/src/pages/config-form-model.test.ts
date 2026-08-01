import { describe, expect, it } from 'vitest'

import {
  appendConfigArrayItem,
  configValueAtPath,
  removeConfigArrayItem,
  updateConfigValueAtPath,
} from './config-form-model'

describe('configuration form value mapping', () => {
  const document = {
    schemaVersion: '1.0',
    configId: 'sample',
    steps: [{
      algorithmId: 'mbddf.sample',
      timeoutMs: 2000,
      criteria: [{ metric: 'status', op: 'Equal', ref: 0 }],
    }],
    hidden: { protocolProfileId: 'fixed' },
  }

  it('reads and immutably updates a JSON pointer while preserving hidden fields', () => {
    expect(configValueAtPath(document, '/steps/0/timeoutMs')).toBe(2000)

    const updated = updateConfigValueAtPath(document, '/steps/0/timeoutMs', 3500)

    expect(configValueAtPath(updated, '/steps/0/timeoutMs')).toBe(3500)
    expect(configValueAtPath(document, '/steps/0/timeoutMs')).toBe(2000)
    expect(updated.hidden).toEqual(document.hidden)
    expect(configValueAtPath(updated, '/steps/0/algorithmId')).toBe('mbddf.sample')
  })

  it('adds and removes declared repeatable rows without replacing the document', () => {
    const appended = appendConfigArrayItem(document, '/steps/0/criteria', {
      metric: 'err_code', op: 'Equal', ref: 0,
    })
    expect(configValueAtPath(appended, '/steps/0/criteria')).toEqual([
      { metric: 'status', op: 'Equal', ref: 0 },
      { metric: 'err_code', op: 'Equal', ref: 0 },
    ])

    const removed = removeConfigArrayItem(appended, '/steps/0/criteria', 0)
    expect(configValueAtPath(removed, '/steps/0/criteria')).toEqual([
      { metric: 'err_code', op: 'Equal', ref: 0 },
    ])
    expect(removed.hidden).toEqual(document.hidden)
  })

  it('creates an omitted repeatable list when the first row is added', () => {
    const source = { steps: [{ name: '测试' }] }

    expect(appendConfigArrayItem(
      source,
      '/steps/0/criteria',
      { metric: 'status', op: 'Equal', ref: 0 },
    )).toEqual({
      steps: [{
        name: '测试',
        criteria: [{ metric: 'status', op: 'Equal', ref: 0 }],
      }],
    })
  })

  it('supports escaped object keys in JSON pointers', () => {
    const value = { 'a/b': { '~key': 1 } }
    const updated = updateConfigValueAtPath(value, '/a~1b/~0key', 2)

    expect(configValueAtPath(updated, '/a~1b/~0key')).toBe(2)
  })

  it('creates missing object branches for sparse station overlays', () => {
    const updated = updateConfigValueAtPath(
      { schemaVersion: '1', resources: {} },
      '/resources/CONTROL_SERIAL/portName',
      'COM7',
    )

    expect(updated).toEqual({
      schemaVersion: '1',
      resources: { CONTROL_SERIAL: { portName: 'COM7' } },
    })
  })
})
