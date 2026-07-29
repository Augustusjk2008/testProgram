import { BracketsCurly, Bug, Radio, Trash } from '@phosphor-icons/react'

import { useSession, useTelemetry } from '../features/session/SessionProvider'
import { formatTimestamp } from '../shared/format'

function pretty(value: unknown): string {
  return value === undefined ? '—' : JSON.stringify(value, null, 2)
}

const EVENT_KIND_LABELS = {
  link: '连接',
  command: '指令',
  snapshot: '状态',
  sample: '样本',
  error: '错误',
} as const

export function DiagnosticsPage() {
  const { snapshot } = useSession()
  const { clearTelemetry, diagnostics, latestSample } = useTelemetry()
  const requestFrame = snapshot.rawData?.requestFrameHex
  const responseFrame = snapshot.rawData?.responseFrameHex

  return (
    <div className="page-stack diagnostics-page">
      <div className="diagnostics-grid">
        <section className="panel raw-panel">
          <header className="panel__header">
            <h3>解码样本</h3>
            <BracketsCurly size={20} />
          </header>
          <div className="raw-meta">
            <span>通道 <b>{latestSample?.channelId || '—'}</b></span>
            <span>轮次 <b>{latestSample?.cycleIndex || 0}</b></span>
            <span>{formatTimestamp(latestSample?.timestampUs)}</span>
          </div>
          <pre>{pretty(latestSample)}</pre>
        </section>

        <section className="panel frames-panel">
          <header className="panel__header">
            <h3>原始帧</h3>
            <Radio size={20} />
          </header>
          <div className="frame-block">
            <span>发送</span>
            <code>{typeof requestFrame === 'string' ? requestFrame : '等待请求帧'}</code>
          </div>
          <div className="frame-block">
            <span>接收</span>
            <code>{typeof responseFrame === 'string' ? responseFrame : '等待反馈帧'}</code>
          </div>
          {(snapshot.errorCode || snapshot.message) && (
            <div className="diagnostic-alert">
              <Bug size={18} />
              <span><strong>{snapshot.errorCode || '消息'}</strong>{snapshot.message}</span>
            </div>
          )}
        </section>
      </div>

      <section className="panel event-log">
        <header className="panel__header">
          <h3>事件记录</h3>
          <div className="panel__actions">
            <span className="mono-count">{diagnostics.length} / 500</span>
            <button aria-label="清空采样缓存" className="button button--quiet button--compact" onClick={clearTelemetry} type="button">
              <Trash size={15} />清空
            </button>
          </div>
        </header>
        {diagnostics.length === 0 ? (
          <div className="compact-empty">尚无会话事件。</div>
        ) : (
          <div className="event-table">
            {diagnostics.map((event) => (
              <details className={`event-row event-row--${event.kind}`} key={event.id}>
                <summary>
                  <time>{new Date(event.timestamp).toLocaleTimeString('zh-CN', { hour12: false })}</time>
                  <i>{EVENT_KIND_LABELS[event.kind]}</i>
                  <strong>{event.title}</strong>
                  <span>{event.detail}</span>
                </summary>
                {event.payload !== undefined && <pre>{pretty(event.payload)}</pre>}
              </details>
            ))}
          </div>
        )}
      </section>
    </div>
  )
}
