import { ChartLine, SlidersHorizontal } from '@phosphor-icons/react'
import { useEffect, useMemo, useState } from 'react'

import { ChartConfigurator } from '../features/telemetry/ChartConfigurator'
import { TelemetryChart } from '../features/telemetry/TelemetryChart'
import {
  buildChartGroups,
  type ChartLayout,
  createAssignments,
  type SeriesAssignment,
} from '../features/telemetry/series-config'
import { useSession } from '../features/session/SessionProvider'
import { DEFAULT_TIME_WINDOW_SECONDS } from '../shared/config'
import { fieldLabel } from '../shared/format'

const STORAGE_KEY = 'hwtest.chart-workspace.SYSTEM_STATUS.v1'

interface StoredConfig {
  layout: ChartLayout
  windowSeconds: number
  assignments: SeriesAssignment[]
}

function loadConfig(): StoredConfig {
  try {
    const value = window.localStorage.getItem(STORAGE_KEY)
    if (value) return JSON.parse(value) as StoredConfig
  } catch {
    // Fall back to the initial workspace when storage is unavailable.
  }
  return { layout: 'combined', windowSeconds: DEFAULT_TIME_WINDOW_SECONDS, assignments: [] }
}

export function ChartsPage() {
  const { clearTelemetry, dataVersion, fields, telemetry } = useSession()
  const initial = useMemo(loadConfig, [])
  const [layout, setLayout] = useState<ChartLayout>(initial.layout)
  const [windowSeconds, setWindowSeconds] = useState(initial.windowSeconds)
  const [assignments, setAssignments] = useState<SeriesAssignment[]>(initial.assignments)

  useEffect(() => {
    setAssignments((current) => {
      const next = createAssignments(fields, current)
      return next.length === current.length ? current : next
    })
  }, [fields])

  useEffect(() => {
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify({
      layout,
      windowSeconds,
      assignments,
    }))
  }, [assignments, layout, windowSeconds])

  const groups = useMemo(
    () => buildChartGroups(assignments, layout),
    [assignments, layout],
  )

  return (
    <div className="charts-workspace">
      <ChartConfigurator
        assignments={assignments}
        layout={layout}
        onAssignmentsChange={setAssignments}
        onClear={clearTelemetry}
        onLayoutChange={setLayout}
        onWindowChange={setWindowSeconds}
        windowSeconds={windowSeconds}
      />

      <main className="charts-stage">
        <div className="page-heading">
          <div>
            <span className="eyebrow">TIME SERIES / CANVAS</span>
            <h2>曲线工作台</h2>
            <p>横轴固定为采样时间；字段选择、图组和时间窗均保存在本机浏览器。</p>
          </div>
          <div className="performance-note">
            <SlidersHorizontal size={18} />
            <span><strong>10 Hz</strong> 批量刷新 · <strong>50k</strong> 点/通道 · min/max 降采样</span>
          </div>
        </div>

        {groups.length === 0 ? (
          <section className="empty-state panel charts-empty">
            <ChartLine size={38} />
            <h3>{fields.length === 0 ? '等待可绘制量' : '尚未选择曲线'}</h3>
            <p>
              {fields.length === 0
                ? '启动 PC 周期测试后，数值字段会从 SYSTEM_STATUS 样本中自动出现。'
                : '在左侧勾选一个或多个量；可全部同图、每项一图或输入自定义图组。'}
            </p>
          </section>
        ) : (
          <div className={layout === 'separate' ? 'chart-grid chart-grid--split' : 'chart-grid'}>
            {groups.map((group) => (
              <TelemetryChart
                buffer={telemetry}
                dataVersion={dataVersion}
                fields={group.fields}
                key={`${group.id}:${group.fields.join(',')}`}
                title={layout === 'custom'
                  ? group.id
                  : group.fields.length === 1
                    ? fieldLabel(group.fields[0])
                    : 'SYSTEM_STATUS · 合并视图'}
                windowSeconds={windowSeconds}
              />
            ))}
          </div>
        )}
      </main>
    </div>
  )
}
