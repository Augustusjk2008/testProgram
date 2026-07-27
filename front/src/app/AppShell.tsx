import {
  ChartLine,
  Gauge,
  Moon,
  Pulse,
  Sun,
  TerminalWindow,
} from '@phosphor-icons/react'
import type { ReactNode } from 'react'

import { RunControlBar } from '../features/session/RunControlBar'
import { useSession } from '../features/session/SessionProvider'
import { connectionStateLabel } from '../shared/format'
import { useTheme } from './ThemeProvider'

export type PageId = 'overview' | 'charts' | 'diagnostics'

const NAV_ITEMS = [
  { id: 'overview' as const, label: '任务', icon: Gauge },
  { id: 'charts' as const, label: '曲线', icon: ChartLine },
  { id: 'diagnostics' as const, label: '诊断', icon: TerminalWindow },
]

interface AppShellProps {
  page: PageId
  onPageChange: (page: PageId) => void
  children: ReactNode
}

export function AppShell({ page, onPageChange, children }: AppShellProps) {
  const { connectionDetail, connectionState, snapshot, wsUrl } = useSession()
  const { theme, toggleTheme } = useTheme()
  const activePage = NAV_ITEMS.find(({ id }) => id === page) ?? NAV_ITEMS[0]
  const descriptor = snapshot.descriptor

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <div className="brand">
          <div className="brand__mark"><Pulse size={24} weight="bold" /></div>
          <strong>HW<span>//</span>TEST</strong>
        </div>

        <nav aria-label="主导航">
          {NAV_ITEMS.map(({ id, label, icon: Icon }) => (
            <button
              aria-current={page === id ? 'page' : undefined}
              className={page === id ? 'nav-item is-active' : 'nav-item'}
              key={id}
              onClick={() => onPageChange(id)}
              type="button"
            >
              <Icon size={20} />
              <strong>{label}</strong>
            </button>
          ))}
        </nav>

        <div className="sidebar__link">
          <div><i className={`status-dot status-dot--${connectionState}`} /><strong>{connectionStateLabel(connectionState)}</strong></div>
          <code title={wsUrl}>{wsUrl}</code>
          {connectionDetail && <small>{connectionDetail}</small>}
        </div>
      </aside>

      <div className="workspace">
        <header className="topbar">
          <h1>{activePage.label}</h1>
          <div className="topbar__controls">
            <div className="topbar__session">
              <span><small>控制资源</small><strong>{snapshot.controlResourceId || '未配置'}</strong></span>
              <span><small>任务 ID</small><strong title={snapshot.taskId}>{snapshot.taskId ? snapshot.taskId.slice(0, 8) : '—'}</strong></span>
              <span><small>算法</small><strong>{snapshot.algorithmId || descriptor.algorithmId || '未选择'}</strong></span>
            </div>
            <button
              aria-label="浅色主题"
              aria-pressed={theme === 'light'}
              className="theme-toggle"
              onClick={toggleTheme}
              title={theme === 'dark' ? '切换为浅色主题' : '切换为深色主题'}
              type="button"
            >
              {theme === 'dark' ? <Sun size={17} /> : <Moon size={17} />}
            </button>
          </div>
        </header>

        <RunControlBar />
        <div className="page-viewport">{children}</div>
      </div>
    </div>
  )
}
