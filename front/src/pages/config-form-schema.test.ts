import { describe, expect, it } from 'vitest'

import { parseConfigFormSchema } from './config-form-schema'

describe('configuration form schema', () => {
  it('parses the lightweight backend form contract', () => {
    const schema = parseConfigFormSchema({
      contractVersion: 1,
      mode: 'form',
      readOnlyPaths: ['/schemaVersion'],
      sections: [{
        id: 'defaults',
        title: '默认测试参数',
        fields: [{
          path: '/steps/0/parameters/protocol/requestValues/waveform',
          label: '波形',
          kind: 'choice',
          defaultValue: 0,
          options: [{ value: 0, label: '正弦波' }, { value: 4, label: '连续扫频' }],
          visibleWhen: { path: '/steps/0/enabled', equals: true },
        }],
        lists: [{
          path: '/steps/0/criteria',
          label: '判定条件',
          addLabel: '增加判定条件',
          allowAdd: true,
          allowRemove: true,
          itemDefaults: { metric: 'status', op: 'Equal', ref: 0 },
          columns: [
            { path: 'metric', label: '测量项', kind: 'choice' },
            {
              path: 'op', label: '比较', kind: 'choice', acceptOptionIndex: true,
              options: [{ value: 'Equal', label: '等于' }],
            },
            { path: 'ref', label: '参考值', kind: 'scalar' },
          ],
        }],
      }],
    })

    expect(schema).not.toBeNull()
    expect(schema?.sections[0]?.fields[0]).toMatchObject({
      kind: 'choice', label: '波形',
    })
    expect(schema?.sections[0]?.fields[0]?.options[0])
      .toEqual({ value: 0, label: '正弦波' })
    expect(schema?.sections[0]?.fields[0]?.defaultValue).toBe(0)
    expect(schema?.sections[0]?.lists[0]).toMatchObject({
      allowAdd: true, allowRemove: true, label: '判定条件',
    })
    expect(schema?.sections[0]?.lists[0]?.columns[0]?.path).toBe('/metric')
    expect(schema?.sections[0]?.lists[0]?.columns[1]?.acceptOptionIndex).toBe(true)
    expect(schema?.sections[0]?.lists[0]?.columns[2]?.kind).toBe('scalar')
  })

  it('rejects unsupported contracts instead of falling back to JSON editing', () => {
    expect(parseConfigFormSchema({ contractVersion: 2, mode: 'form', sections: [] }))
      .toBeNull()
    expect(parseConfigFormSchema({
      contractVersion: 1,
      mode: 'form',
      sections: [{ id: 'bad', title: '错误', fields: [{ path: '/x', label: 'X', kind: 'json' }] }],
    })).toBeNull()
  })
})
