import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it, vi } from 'vitest'

import type { ConfigFormSchema } from './config-form-schema'
import { ConfigForm } from './ConfigForm'

const schema: ConfigFormSchema = {
  contractVersion: 1,
  mode: 'form',
  readOnlyPaths: [],
  sections: [{
    id: 'main',
    title: '产品参数',
    description: '只显示产品工程师需要调整的字段。',
    fields: [
      {
        path: '/mode', label: '工作模式', description: '', kind: 'choice', unit: '',
        required: true, options: [{ value: 0, label: '普通' }, { value: 4, label: '扫频' }],
        optionsSource: '', allowManualEntry: false, wide: false, readOnly: false,
      },
      {
        path: '/count', label: '循环次数', description: '', kind: 'integer', unit: '次',
        required: true, defaultValue: 50, minimum: 1, maximum: 1000, options: [], optionsSource: '',
        allowManualEntry: false, wide: false, readOnly: false,
      },
      {
        path: '/port', label: '控制串口', description: '', kind: 'choiceOrText', unit: '',
        required: true, options: [], optionsSource: 'serialPorts', allowManualEntry: true,
        wide: false, readOnly: false,
      },
      {
        path: '/sweepDuration', label: '扫频时长', description: '', kind: 'number', unit: 's',
        required: true, options: [], optionsSource: '', allowManualEntry: false,
        visibleWhen: { path: '/mode', equals: 4 }, wide: false, readOnly: false,
      },
    ],
    lists: [{
      path: '/criteria', label: '判定条件', description: '', addLabel: '增加判定条件',
      allowAdd: true, allowRemove: true,
      itemDefaults: { metric: 'status', op: 'Equal', ref: 0 },
      columns: [{
        path: '/metric', label: '测量项', description: '', kind: 'choice', unit: '',
        required: true, options: [{ value: 'status', label: '设备状态' }], optionsSource: '',
        allowManualEntry: false, wide: false, readOnly: false,
      }, {
        path: '/op', label: '比较', description: '', kind: 'choice', unit: '',
        required: true, acceptOptionIndex: true,
        options: [{ value: 'GreaterThan', label: '大于' }, { value: 'Equal', label: '等于' }],
        optionsSource: '', allowManualEntry: false, wide: false, readOnly: false,
      }, {
        path: '/ref', label: '参考值', description: '', kind: 'scalar', unit: '',
        required: false, options: [], optionsSource: '', allowManualEntry: false,
        wide: false, readOnly: false,
      }],
    }],
  }],
}

describe('ConfigForm', () => {
  it('renders typed controls, dynamic choices and repeatable rows without a JSON editor', () => {
    const markup = renderToStaticMarkup(
      <ConfigForm
        disabled={false}
        onChange={vi.fn()}
        optionSources={{ serialPorts: [{ value: 'COM7', label: 'COM7 · USB 串口' }] }}
        schema={schema}
        value={{
          mode: 0,
          count: 100,
          port: 'COM7',
          sweepDuration: 25,
          criteria: [{ metric: 'status', op: 'Equal', ref: 0 }],
        }}
      />,
    )

    expect(markup).toContain('产品参数')
    expect(markup).toContain('<select aria-label="工作模式"')
    expect(markup).toContain('type="number" value="100"')
    expect(markup).toContain('<datalist id=')
    expect(markup).toContain('COM7 · USB 串口')
    expect(markup).toContain('增加判定条件')
    expect(markup).toContain('删除第 1 项判定条件')
    expect(markup).not.toContain('扫频时长')
    expect(markup).not.toContain('JSON')
  })

  it('shows fields whose visibility condition is satisfied', () => {
    const markup = renderToStaticMarkup(
      <ConfigForm
        disabled={false}
        onChange={vi.fn()}
        schema={schema}
        value={{ mode: 4, count: 100, port: 'COM7', sweepDuration: 25, criteria: [] }}
      />,
    )

    expect(markup).toContain('扫频时长')
    expect(markup).toContain('value="25"')
  })

  it('shows inherited defaults without writing them into the document', () => {
    const markup = renderToStaticMarkup(
      <ConfigForm
        disabled={false}
        onChange={vi.fn()}
        schema={schema}
        value={{ mode: 0, port: 'COM7', criteria: [] }}
      />,
    )

    expect(markup).toContain('type="number" value="50"')
    expect(markup).toContain('当前使用默认值')
  })

  it('renders legacy criterion indexes and string references without a JSON fallback', () => {
    const markup = renderToStaticMarkup(
      <ConfigForm
        disabled={false}
        onChange={vi.fn()}
        schema={schema}
        value={{
          mode: 0,
          count: 1,
          port: 'COM7',
          criteria: [{ metric: 'status', op: 0, ref: 'READY' }],
        }}
      />,
    )

    expect(markup).toContain('<option value="0" selected="">大于</option>')
    expect(markup).toContain('aria-label="参考值"')
    expect(markup).toContain('value="READY"')
  })
})
