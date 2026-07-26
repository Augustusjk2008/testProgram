import { CheckSquare, Eraser, SquaresFour } from '@phosphor-icons/react'

import { fieldLabel, fieldUnit } from '../../shared/format'
import type { ChartLayout, SeriesAssignment } from './series-config'
import { useSession } from '../session/SessionProvider'

interface ChartConfiguratorProps {
  assignments: SeriesAssignment[]
  layout: ChartLayout
  windowSeconds: number
  onAssignmentsChange: (assignments: SeriesAssignment[]) => void
  onLayoutChange: (layout: ChartLayout) => void
  onWindowChange: (seconds: number) => void
  onClear: () => void
}

const LAYOUTS: Array<{ value: ChartLayout; label: string }> = [
  { value: 'combined', label: '全部同图' },
  { value: 'separate', label: '每项一图' },
  { value: 'custom', label: '自定义组' },
]

export function ChartConfigurator({
  assignments,
  layout,
  windowSeconds,
  onAssignmentsChange,
  onLayoutChange,
  onWindowChange,
  onClear,
}: ChartConfiguratorProps) {
  const { snapshot } = useSession()
  const measurements = snapshot.descriptor.measurements
  const selectedCount = assignments.filter(({ enabled }) => enabled).length

  function update(path: string, patch: Partial<SeriesAssignment>) {
    onAssignmentsChange(assignments.map((assignment) => (
      assignment.path === path ? { ...assignment, ...patch } : assignment
    )))
  }

  return (
    <aside className="chart-config panel">
      <header className="panel__header">
        <div>
          <span className="eyebrow">PLOT MATRIX</span>
          <h3>曲线配置</h3>
        </div>
        <SquaresFour size={20} />
      </header>

      <section className="config-section">
        <label className="config-label">布局方式</label>
        <div className="segmented-control">
          {LAYOUTS.map(({ value, label }) => (
            <button
              className={layout === value ? 'is-active' : ''}
              key={value}
              onClick={() => onLayoutChange(value)}
              type="button"
            >
              {label}
            </button>
          ))}
        </div>
      </section>

      <section className="config-section">
        <label className="config-label" htmlFor="time-window">显示时间窗</label>
        <select
          id="time-window"
          onChange={(event) => onWindowChange(Number(event.target.value))}
          value={windowSeconds}
        >
          <option value={60}>最近 1 分钟</option>
          <option value={300}>最近 5 分钟</option>
          <option value={900}>最近 15 分钟</option>
          <option value={3600}>最近 1 小时</option>
          <option value={0}>全部保留点</option>
        </select>
      </section>

      <section className="config-section config-section--series">
        <div className="config-section__title">
          <label className="config-label">可绘制量</label>
          <span>{selectedCount}/{assignments.length}</span>
        </div>
        <div className="inline-actions">
          <button
            onClick={() => onAssignmentsChange(assignments.map((item) => ({ ...item, enabled: true })))}
            type="button"
          >
            <CheckSquare size={15} />全选
          </button>
          <button
            onClick={() => onAssignmentsChange(assignments.map((item) => ({ ...item, enabled: false })))}
            type="button"
          >
            清空选择
          </button>
        </div>

        {assignments.length === 0 ? (
          <div className="compact-empty">首条样本到达后，将自动发现数值字段。</div>
        ) : (
          <div className="series-list">
            {assignments.map((assignment) => (
              <div className={assignment.enabled ? 'series-row is-selected' : 'series-row'} key={assignment.path}>
                <label>
                  <input
                    checked={assignment.enabled}
                    onChange={(event) => update(assignment.path, { enabled: event.target.checked })}
                    type="checkbox"
                  />
                  <span>
                    <strong>{fieldLabel(assignment.path, measurements)}</strong>
                    <small>{assignment.path}{fieldUnit(assignment.path, measurements) ? ` · ${fieldUnit(assignment.path, measurements)}` : ''}</small>
                  </span>
                </label>
                {layout === 'custom' && assignment.enabled && (
                  <input
                    aria-label={`${fieldLabel(assignment.path)} 所属图组`}
                    className="group-input"
                    onChange={(event) => update(assignment.path, { groupId: event.target.value })}
                    value={assignment.groupId}
                  />
                )}
              </div>
            ))}
          </div>
        )}
      </section>

      <button className="button button--quiet config-clear" onClick={onClear} type="button">
        <Eraser size={16} />清空采样缓存
      </button>
    </aside>
  )
}
