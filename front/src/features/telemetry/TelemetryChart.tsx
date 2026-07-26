import { useLayoutEffect, useRef } from 'react'
import uPlot from 'uplot'
import 'uplot/dist/uPlot.min.css'

import { fieldLabel, fieldUnit } from '../../shared/format'
import type { SampleBuffer } from './sample-buffer'

const SERIES_COLORS = [
  '#b6f34a',
  '#45d6d0',
  '#f5b84b',
  '#d18cff',
  '#ff6b6b',
  '#7ca8ff',
  '#f0f3e8',
]

interface TelemetryChartProps {
  title: string
  fields: string[]
  buffer: SampleBuffer
  dataVersion: number
  windowSeconds: number
}

export function TelemetryChart({
  title,
  fields,
  buffer,
  dataVersion,
  windowSeconds,
}: TelemetryChartProps) {
  const hostRef = useRef<HTMLDivElement>(null)
  const plotRef = useRef<uPlot | null>(null)
  const fieldsKey = fields.join('|')

  useLayoutEffect(() => {
    const host = hostRef.current
    if (!host) return
    const width = Math.max(320, Math.floor(host.clientWidth))
    const data = buffer.query(fields, { windowSeconds, pixelWidth: width })
    const options: uPlot.Options = {
      width,
      height: 290,
      padding: [10, 12, 0, 0],
      cursor: {
        drag: { x: true, y: false },
        points: { size: 5 },
      },
      legend: {
        show: true,
        live: true,
      },
      scales: {
        x: { time: true },
      },
      axes: [
        {
          stroke: '#77827f',
          grid: { stroke: '#1a2425', width: 1 },
          ticks: { stroke: '#2a3637', width: 1 },
          font: '11px Geist Mono, monospace',
          size: 46,
        },
        {
          stroke: '#77827f',
          grid: { stroke: '#1a2425', width: 1 },
          ticks: { stroke: '#2a3637', width: 1 },
          font: '11px Geist Mono, monospace',
          size: 54,
        },
      ],
      series: [
        { label: '时间' },
        ...fields.map((field, index) => ({
          label: fieldLabel(field),
          stroke: SERIES_COLORS[index % SERIES_COLORS.length],
          width: 1.5,
          spanGaps: false,
          points: { show: false },
          value: (_plot: uPlot, value: number | null) => {
            if (value === null || value === undefined) return '—'
            const unit = fieldUnit(field)
            return `${value.toLocaleString('zh-CN', { maximumFractionDigits: 3 })}${unit ? ` ${unit}` : ''}`
          },
        })),
      ],
    }

    const plot = new uPlot(options, data as uPlot.AlignedData, host)
    plotRef.current = plot
    const observer = new ResizeObserver(([entry]) => {
      const nextWidth = Math.max(320, Math.floor(entry.contentRect.width))
      if (nextWidth !== plot.width) {
        plot.setSize({ width: nextWidth, height: 290 })
        const nextData = buffer.query(fields, {
          windowSeconds,
          pixelWidth: nextWidth,
        })
        plot.setData(nextData as uPlot.AlignedData)
      }
    })
    observer.observe(host)
    return () => {
      observer.disconnect()
      plot.destroy()
      plotRef.current = null
    }
    // fieldsKey intentionally represents the complete series identity.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [buffer, fieldsKey])

  useLayoutEffect(() => {
    const plot = plotRef.current
    const host = hostRef.current
    if (!plot || !host) return
    const data = buffer.query(fields, {
      windowSeconds,
      pixelWidth: Math.max(320, Math.floor(host.clientWidth)),
    })
    plot.setData(data as uPlot.AlignedData)
  }, [buffer, dataVersion, fields, windowSeconds])

  return (
    <article className="telemetry-chart panel">
      <header className="panel__header">
        <div>
          <span className="eyebrow">LIVE TELEMETRY</span>
          <h3>{title}</h3>
        </div>
        <span className="chart-point-count">{buffer.size.toLocaleString('zh-CN')} pts</span>
      </header>
      <div className="telemetry-chart__canvas" ref={hostRef} />
    </article>
  )
}
