import { BracketsCurly, Bug, Radio, Trash } from '@phosphor-icons/react'

import { useSession } from '../features/session/SessionProvider'
import { formatTimestamp } from '../shared/format'

function pretty(value: unknown): string {
  return value === undefined ? '—' : JSON.stringify(value, null, 2)
}

export function DiagnosticsPage() {
  const {
    clearTelemetry,
    diagnostics,
    latestSample,
    snapshot,
  } = useSession()
  const requestFrame = snapshot.rawData?.requestFrameHex
  const responseFrame = snapshot.rawData?.responseFrameHex

  return (
    <div className="page-stack diagnostics-page">
      <div className="page-heading">
        <div>
          <span className="eyebrow">PROTOCOL INSPECTOR</span>
          <h2>报文与诊断</h2>
          <p>查看最近样本、请求/反馈帧和浏览器会话事件；最多保留 500 条。</p>
        </div>
        <button className="button button--quiet" onClick={clearTelemetry} type="button">
          <Trash size={16} />清空采样缓存
        </button>
      </div>

      <div className="diagnostics-grid">
        <section className="panel raw-panel">
          <header className="panel__header">
            <div><span className="eyebrow">LATEST SAMPLE</span><h3>解码样本</h3></div>
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
            <div><span className="eyebrow">WIRE FRAMES</span><h3>原始帧</h3></div>
            <Radio size={20} />
          </header>
          <div className="frame-block">
            <span>TX / REQUEST</span>
            <code>{typeof requestFrame === 'string' ? requestFrame : '等待请求帧'}</code>
          </div>
          <div className="frame-block">
            <span>RX / RESPONSE</span>
            <code>{typeof responseFrame === 'string' ? responseFrame : '等待反馈帧'}</code>
          </div>
          {(snapshot.errorCode || snapshot.message) && (
            <div className="diagnostic-alert">
              <Bug size={18} />
              <span><strong>{snapshot.errorCode || 'MESSAGE'}</strong>{snapshot.message}</span>
            </div>
          )}
        </section>
      </div>

      <section className="panel event-log">
        <header className="panel__header">
          <div><span className="eyebrow">SESSION TRACE</span><h3>事件记录</h3></div>
          <span className="mono-count">{diagnostics.length} / 500</span>
        </header>
        {diagnostics.length === 0 ? (
          <div className="compact-empty">尚无会话事件。</div>
        ) : (
          <div className="event-table">
            {diagnostics.map((event) => (
              <details className={`event-row event-row--${event.kind}`} key={event.id}>
                <summary>
                  <time>{new Date(event.timestamp).toLocaleTimeString('zh-CN', { hour12: false })}</time>
                  <i>{event.kind}</i>
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
