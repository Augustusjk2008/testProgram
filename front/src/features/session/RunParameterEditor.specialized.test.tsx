import { renderToStaticMarkup } from 'react-dom/server'
import { Children, isValidElement, type ReactElement, type ReactNode } from 'react'
import { describe, expect, it, vi } from 'vitest'

import type {
  TestDescriptor,
  TestRunParameterDescriptor,
} from '../../shared/protocol'
import { RunParameterEditor } from './RunParameterEditor'

function parameter(
  id: string,
  label: string,
  kind: TestRunParameterDescriptor['kind'] = 'boolean',
): TestRunParameterDescriptor {
  return {
    id,
    label,
    description: '',
    kind,
    unit: '',
    required: true,
    minimumExclusive: false,
    maximumExclusive: false,
    choices: [],
  }
}

function descriptor(
  algorithmId: string,
  runParameters: TestRunParameterDescriptor[],
  runParameterDefaults: Record<string, unknown>,
): TestDescriptor {
  return {
    configId: algorithmId,
    productModel: 'MB_DDF_v2',
    productName: '测试',
    configVersion: '1',
    stepId: 'STEP',
    testItemId: 'item',
    algorithmId,
    title: '测试',
    description: '',
    stoppable: true,
    supportedRunModes: ['single'],
    measurements: [],
    taskMeasurements: [],
    runParameterSchemaVersion: '1',
    runParameters,
    runParameterDefaults,
    postRunAnalysis: { supported: false, analyzerId: '', schemaVersion: '' },
  }
}

function inputFor(markup: string, label: string): string {
  const labelAt = markup.indexOf(`aria-label="${label}"`)
  const start = markup.lastIndexOf('<input', labelAt)
  const end = markup.indexOf('>', labelAt)
  return start >= 0 && end >= start ? markup.slice(start, end + 1) : ''
}

function renderEditor(
  testDescriptor: TestDescriptor,
  values: Record<string, unknown>,
): string {
  return renderToStaticMarkup(
    <RunParameterEditor
      descriptor={testDescriptor}
      disabled={false}
      effective={false}
      onChange={vi.fn()}
      onReset={vi.fn()}
      values={values}
    />,
  )
}

type InspectableElementProps = {
  children?: ReactNode
  'aria-label'?: string
  onChange?: unknown
}

function elementWithAriaLabel(
  node: ReactNode,
  label: string,
): ReactElement<InspectableElementProps> | null {
  if (!isValidElement<InspectableElementProps>(node)) return null
  if (node.props['aria-label'] === label) return node
  let found: ReactElement<InspectableElementProps> | null = null
  Children.forEach(node.props.children, (child) => {
    if (found === null) found = elementWithAriaLabel(child, label)
  })
  return found
}

describe('RunParameterEditor specialized parameter presentation', () => {
  it('renders DH integer enable values as numeric checkboxes with bulk channel controls', () => {
    const channels = Array.from({ length: 23 }, (_, index) => parameter(
      `channel_enabled[${index}]`,
      `DH${index} 点火通道`,
    ))
    const values = Object.fromEntries(channels.map((item) => [item.id, false]))
    Object.assign(values, { power_enable: 1, return_enable: 0, delay_frames: 5 })

    const markup = renderEditor(
      descriptor(
        'mbddf.dh_ignite_stream',
        [
          parameter('power_enable', '点火电源使能', 'integer'),
          parameter('return_enable', '点火回线使能', 'integer'),
          parameter('delay_frames', '等待帧数', 'integer'),
          ...channels,
        ],
        values,
      ),
      values,
    )

    expect(inputFor(markup, '点火电源使能')).toContain('type="checkbox"')
    expect(inputFor(markup, '点火电源使能')).toContain('checked=""')
    expect(inputFor(markup, '点火回线使能')).toContain('type="checkbox"')
    expect(inputFor(markup, '点火回线使能')).not.toContain('checked=""')
    expect(inputFor(markup, 'DH0 点火通道')).toContain('type="checkbox"')
    expect(markup).toContain('等待帧数')
    expect(markup).toContain('23 路点火通道')
    expect(markup).toContain('全选')
    expect(markup).toContain('全不选')
  })

  it('emits numeric zero or one when a DH integer enable checkbox changes', () => {
    const values = { power_enable: 1 }
    const onChange = vi.fn()
    const view = RunParameterEditor({
      descriptor: descriptor(
        'mbddf.dh_ignite_stream',
        [parameter('power_enable', '点火电源使能', 'integer')],
        values,
      ),
      disabled: false,
      effective: false,
      onChange,
      onReset: vi.fn(),
      values,
    })
    const checkbox = elementWithAriaLabel(view, '点火电源使能')

    expect(checkbox).not.toBeNull()
    const onInputChange = checkbox?.props.onChange as ((event: { target: { checked: boolean } }) => void)
    onInputChange({ target: { checked: false } })
    expect(onChange).toHaveBeenCalledWith({ power_enable: 0 })
  })

  it('renders DO channels in a guarded sixteen-bit grid', () => {
    const channels = Array.from({ length: 16 }, (_, index) => parameter(
      `channel_enabled[${index}]`,
      `DO${index}`,
    ))
    const values = Object.fromEntries(channels.map((item) => [item.id, true]))
    const markup = renderEditor(
      descriptor('mbddf.do_write', channels, values),
      values,
    )

    expect(markup).toContain('16 路数字输出')
    expect(markup).toContain('DO3：低有效，默认安全位为 1（关闭）')
    expect(markup).toContain('DO4：低有效，默认安全位为 1（关闭）')
    expect(inputFor(markup, 'DO3')).toContain('type="checkbox"')
    expect(inputFor(markup, 'DO5')).toContain('type="checkbox"')
    expect(inputFor(markup, 'DO5')).toContain('disabled=""')
    expect(inputFor(markup, 'DO5')).not.toContain('checked=""')
    expect(inputFor(markup, 'DO6')).toContain('disabled=""')
    expect(inputFor(markup, 'DO6')).not.toContain('checked=""')
  })
})
