import { useId, useLayoutEffect, useMemo, useRef } from 'react'
import uPlot from 'uplot'
import 'uplot/dist/uPlot.min.css'

import { useTheme } from '../../app/ThemeProvider'
import { getChartPalette } from '../../shared/theme'
import {
  createBodePlotModel,
  type BodeCurve,
  type BodeMarker,
  type BodePlotModel,
  type FrequencyUnit,
} from './bode-chart-model'

const CHART_HEIGHT = 270

function markerPlugin(markers: readonly BodeMarker[], color: string): uPlot.Plugin {
  return {
    hooks: {
      draw: [(plot) => {
        const { ctx, bbox } = plot
        ctx.save()
        ctx.strokeStyle = color
        ctx.fillStyle = color
        ctx.globalAlpha = 0.72
        ctx.lineWidth = 1
        ctx.setLineDash([3, 3])
        ctx.font = '10px Geist Mono, monospace'
        markers.forEach((marker) => {
          const x = plot.valToPos(marker.frequency, 'x', true)
          if (x < bbox.left || x > bbox.left + bbox.width) return
          ctx.beginPath()
          ctx.moveTo(x, bbox.top)
          ctx.lineTo(x, bbox.top + bbox.height)
          ctx.stroke()
          ctx.setLineDash([])
          ctx.fillText(marker.label, x + 3, bbox.top + 12)
          ctx.setLineDash([3, 3])
        })
        ctx.restore()
      }],
    },
  }
}

interface BodeCanvasProps {
  title: string
  yLabel: string
  chart: BodePlotModel['magnitude']
  model: BodePlotModel
  syncKey: string
}

function BodeCanvas({ title, yLabel, chart, model, syncKey }: BodeCanvasProps) {
  const { theme } = useTheme()
  const hostRef = useRef<HTMLDivElement>(null)
  const palette = getChartPalette(theme)

  useLayoutEffect(() => {
    const host = hostRef.current
    if (!host) return
    const width = Math.max(320, Math.floor(host.clientWidth))
    const unitLabel = model.frequencyUnit === 'rad/s' ? 'rad/s' : 'Hz'
    const options: uPlot.Options = {
      width,
      height: CHART_HEIGHT,
      padding: [14, 12, 0, 0],
      cursor: {
        drag: { x: true, y: false },
        points: { size: 5 },
        sync: { key: syncKey },
      },
      legend: { show: true, live: true },
      scales: { x: { time: false, distr: model.xScale.distr } },
      axes: [
        {
          label: `频率 (${unitLabel})`,
          stroke: palette.axis,
          grid: { stroke: palette.grid, width: 1 },
          ticks: { stroke: palette.ticks, width: 1 },
          font: '12px Geist Mono, monospace',
          size: 46,
        },
        {
          label: yLabel,
          stroke: palette.axis,
          grid: { stroke: palette.grid, width: 1 },
          ticks: { stroke: palette.ticks, width: 1 },
          font: '12px Geist Mono, monospace',
          size: 54,
        },
      ],
      series: [
        { label: '频率' },
        ...chart.series.map((series, index) => ({
          label: series.label,
          stroke: palette.series[index % palette.series.length],
          width: 1.5,
          spanGaps: series.spanGaps,
          points: { show: false },
          value: (_plot: uPlot, value: number | null) => (
            value === null || value === undefined
              ? '—'
              : value.toLocaleString('zh-CN', { maximumFractionDigits: 3 })
          ),
        })),
      ],
      plugins: [markerPlugin(model.markers, palette.axis)],
    }
    const plot = new uPlot(options, chart.data as uPlot.AlignedData, host)
    const observer = new ResizeObserver(([entry]) => {
      const nextWidth = Math.max(320, Math.floor(entry.contentRect.width))
      if (nextWidth !== plot.width) plot.setSize({ width: nextWidth, height: CHART_HEIGHT })
    })
    observer.observe(host)
    return () => {
      observer.disconnect()
      plot.destroy()
    }
  }, [chart, model, palette, syncKey, yLabel])

  return (
    <article className="bode-chart panel">
      <header className="panel__header"><h3>{title}</h3></header>
      <div className="bode-chart__canvas" ref={hostRef} />
    </article>
  )
}

export function BodePlot({ curves, frequencyUnit }: {
  curves: readonly BodeCurve[]
  frequencyUnit: FrequencyUnit
}) {
  const model = useMemo(
    () => createBodePlotModel(curves, frequencyUnit),
    [curves, frequencyUnit],
  )
  const syncKey = `bode-${useId().replaceAll(':', '')}`

  if (model.frequency.length === 0) return null
  return (
    <section className="bode-plot" aria-label="伯德图">
      <BodeCanvas chart={model.magnitude} model={model} syncKey={syncKey} title="幅值" yLabel="幅值 (dB)" />
      <BodeCanvas chart={model.phase} model={model} syncKey={syncKey} title="相位" yLabel="相位 (°)" />
    </section>
  )
}
