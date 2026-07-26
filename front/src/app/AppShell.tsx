import {
  ChartLine,
  CirclesThreePlus,
  Gauge,
  Pulse,
  TerminalWindow,
} from '@phosphor-icons/react'
import type { ReactNode } from 'react'

import { RunControlBar } from '../features/session/RunControlBar'
import { useSession } from '../features/session/SessionProvider'

export type PageId = 'overview' | 'charts' | 'diagnostics'

const NAV_ITEMS = [
  { id: 'overview' as const, label: '任务总览', caption: 'STATUS', icon: Gauge },
  { id: 'charts' as const, label: '曲线工作台', caption: 'PLOTS', icon: ChartLine },
  { id: 'diagnostics' as const, label: '报文与诊断', caption: 'TRACE', icon: TerminalWindow },
]

interface AppShellProps {
  page: PageId
  onPageChange: (page: PageId) => void
  children: ReactNode
}

export function AppShell({ page, onPageChange, children }: AppShellProps) {
  const { connectionDetail, connectionState, snapshot, wsUrl } = useSession()
  const activePage = NAV_ITEMS.find(({ id }) => id === page) ?? NAV_ITEMS[0]
  const descriptor = snapshot.descriptor
  const productLabel = descriptor.productModel || 'MB_DDF'
  const testLabel = descriptor.title || descriptor.algorithmId || '未选择测试'

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <div className="brand">
          <div className="brand__mark"><Pulse size={24} weight="bold" /></div>
          <div><strong>HW<span>//</span>TEST</strong><small>TELEMETRY CONSOLE</small></div>
        </div>

        <div className="sidebar__station">
          <span className="eyebrow">STATION</span>
          <strong>PC HOST · 01</strong>
          <small>{productLabel} / {testLabel}</small>
        </div>

        <nav aria-label="主导航">
          {NAV_ITEMS.map(({ id, label, caption, icon: Icon }, index) => (
            <button
              aria-current={page === id ? 'page' : undefined}
              className={page === id ? 'nav-item is-active' : 'nav-item'}
              key={id}
              onClick={() => onPageChange(id)}
              type="button"
            >
              <span className="nav-item__index">0{index + 1}</span>
              <Icon size={20} />
              <span><strong>{label}</strong><small>{caption}</small></span>
            </button>
          ))}
        </nav>

        <div className="sidebar__link">
          <span className="eyebrow">LOOPBACK LINK</span>
          <div><i className={`status-dot status-dot--${connectionState}`} /><strong>{connectionState}</strong></div>
          <code title={wsUrl}>{wsUrl}</code>
          {connectionDetail && <small>{connectionDetail}</small>}
        </div>
        <div className="sidebar__footer">
          <CirclesThreePlus size={15} />
          <span>Qt WebSocket · Protocol v1</span>
        </div>
      </aside>

      <div className="workspace">
        <header className="topbar">
          <div>
            <span className="topbar__path">HWTEST / {activePage.caption}</span>
            <h1>{activePage.label}</h1>
          </div>
          <div className="topbar__session">
            <span><small>控制资源</small><strong>{snapshot.controlResourceId || '未配置'}</strong></span>
            <span><small>任务 ID</small><strong title={snapshot.taskId}>{snapshot.taskId ? snapshot.taskId.slice(0, 8) : '—'}</strong></span>
            <span><small>算法</small><strong>{snapshot.algorithmId || descriptor.algorithmId || '未选择'}</strong></span>
          </div>
        </header>

        <RunControlBar />
        <div className="page-viewport">{children}</div>
      </div>
    </div>
  )
}
