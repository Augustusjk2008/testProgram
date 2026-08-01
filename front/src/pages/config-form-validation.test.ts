import { describe, expect, it } from 'vitest'

import type { ConfigFormSchema } from './config-form-schema'
import { validateConfigForm } from './config-form-validation'

const schema: ConfigFormSchema = {
  contractVersion: 1,
  mode: 'form',
  readOnlyPaths: [],
  sections: [{
    id: 'main', title: '参数', description: '', lists: [], fields: [
      {
        path: '/count', label: '循环次数', description: '', kind: 'integer', unit: '次',
        required: true, defaultValue: 2, minimum: 1, maximum: 3, options: [], optionsSource: '',
        allowManualEntry: false, wide: false, readOnly: false,
      },
      {
        path: '/mode', label: '模式', description: '', kind: 'choice', unit: '', required: true,
        options: [{ value: 0, label: '普通' }, { value: 4, label: '扫频' }], optionsSource: '',
        allowManualEntry: false, wide: false, readOnly: false,
      },
      {
        path: '/duration', label: '扫频时长', description: '', kind: 'number', unit: 's',
        required: true, minimum: 0, options: [], optionsSource: '', allowManualEntry: false,
        visibleWhen: { path: '/mode', equals: 4 }, wide: false, readOnly: false,
      },
      {
        path: '/op', label: '比较', description: '', kind: 'choice', unit: '',
        required: true, defaultValue: 'Equal', acceptOptionIndex: true,
        options: [{ value: 'GreaterThan', label: '大于' }, { value: 'Equal', label: '等于' }],
        optionsSource: '', allowManualEntry: false, wide: false, readOnly: false,
      },
      {
        path: '/ref', label: '参考值', description: '', kind: 'scalar', unit: '',
        required: false, options: [], optionsSource: '', allowManualEntry: false,
        wide: false, readOnly: false,
      },
    ],
  }],
}

describe('configuration form validation', () => {
  it('accepts typed values inside declared ranges', () => {
    expect(validateConfigForm(schema, { count: 2, mode: 4, duration: 25 })).toEqual([])
  })

  it('uses a declared default when a required field is absent from the document', () => {
    expect(validateConfigForm(schema, { mode: 0 })).toEqual([])
  })

  it('accepts legacy option indexes and string scalar references', () => {
    expect(validateConfigForm(schema, { count: 2, mode: 0, op: 0, ref: 'READY' }))
      .toEqual([])
  })

  it('reports invalid visible fields and ignores hidden fields', () => {
    expect(validateConfigForm(schema, { count: 4.5, mode: 0, duration: -1 }))
      .toEqual([
        { path: '/count', message: '循环次数必须是整数' },
        { path: '/count', message: '循环次数不能大于 3 次' },
      ])
  })
})
