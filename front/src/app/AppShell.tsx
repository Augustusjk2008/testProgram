import {
  ArrowLeft,
  ChartLine,
  Gear,
  Gauge,
  Moon,
  Pulse,
  Sun,
  TerminalWindow,
} from '@phosphor-icons/react'
import type { ReactNode } from 'react'

import { RunControlBar } from '../features/session/RunControlBar'
import { useSession } from '../features/session/SessionProvider'
import { isBoardTestAlgorithm } from '../features/board-test/board-test-navigation'
import { isPerformanceCapabilityEnabled } from '../features/performance/performance-navigation'
import { connectionStateLabel } from '../shared/format'
import { useTheme } from './ThemeProvider'

export type WorkspaceId = 'test' | 'configuration'
export type PageId = 'overview' | 'charts' | 'performance' | 'board-test' | 'diagnostics'

const NAV_ITEMS = [
  { id: 'overview' as const, label: '任务', icon: Gauge },
  { id: 'charts' as const, label: '曲线', icon: ChartLine },
  { id: 'performance' as const, label: '性能', icon: Pulse },
  { id: 'diagnostics' as const, label: '诊断', icon: TerminalWindow },
]

const BOARD_TEST_NAV_ITEM = { id: 'board-test' as const, label: '板级测试', icon: Pulse }

interface AppShellProps {
  workspace: WorkspaceId
  page: PageId
  onPageChange: (page: PageId) => void
  onWorkspaceChange: (workspace: WorkspaceId) => void
  children: ReactNode
}

export function AppShell({ workspace, page, onPageChange, onWorkspaceChange, children }: AppShellProps) {
  const { connectionDetail, connectionState, snapshot, wsUrl } = useSession()
  const { theme, toggleTheme } = useTheme()
  const descriptor = snapshot.descriptor
  const performanceAvailable = isPerformanceCapabilityEnabled(descriptor.postRunAnalysis)
  const boardTestAvailable = isBoardTestAlgorithm(snapshot.algorithmId || descriptor.algorithmId)
  const navItems = boardTestAvailable ? [...NAV_ITEMS, BOARD_TEST_NAV_ITEM] : NAV_ITEMS
  const displayedPage = navItems.find(({ id }) => id === page) ?? navItems[0]
  const configuring = workspace === 'configuration'

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <div className="brand">
          <div className="brand__mark"><Pulse size={24} weight="bold" /></div>
          <strong>HW<span>//</span>TEST</strong>
        </div>

        {configuring ? (
          <div className="sidebar__workspace-title">
            <Gear size={20} />
            <div><small>全局工作区</small><strong>系统配置</strong></div>
          </div>
        ) : (
          <nav aria-label="主导航">
            {navItems.map(({ id, label, icon: Icon }) => {
              const disabled = id === 'performance' && !performanceAvailable
              return (
                <button
                  aria-current={page === id ? 'page' : undefined}
                  className={page === id ? 'nav-item is-active' : 'nav-item'}
                  disabled={disabled}
                  key={id}
                  onClick={() => onPageChange(id)}
                  title={disabled ? '当前测试未声明后处理性能分析能力' : undefined}
                  type="button"
                >
                  <Icon size={20} />
                  <strong>{label}</strong>
                </button>
              )
            })}
          </nav>
        )}

        <div className="sidebar__workspace-switch">
          <button
            className={configuring ? 'nav-item' : 'nav-item nav-item--configuration'}
            onClick={() => onWorkspaceChange(configuring ? 'test' : 'configuration')}
            type="button"
          >
            {configuring ? <ArrowLeft size={20} /> : <Gear size={20} />}
            <strong>{configuring ? '返回测试工作台' : '系统配置'}</strong>
          </button>
        </div>

        <div className="sidebar__link">
          <div><i className={`status-dot status-dot--${connectionState}`} /><strong>{connectionStateLabel(connectionState)}</strong></div>
          <code title={wsUrl}>{wsUrl}</code>
          {connectionDetail && <small>{connectionDetail}</small>}
        </div>
      </aside>

      <div className={configuring ? 'workspace workspace--configuration' : 'workspace'}>
        <header className="topbar">
          <h1>{configuring ? '系统配置' : displayedPage.label}</h1>
          <div className="topbar__controls">
            {!configuring && (
              <div className="topbar__session">
                <span><small>控制资源</small><strong>{snapshot.controlResourceId || '未配置'}</strong></span>
                <span><small>任务 ID</small><strong title={snapshot.taskId}>{snapshot.taskId ? snapshot.taskId.slice(0, 8) : '—'}</strong></span>
                <span><small>算法</small><strong>{snapshot.algorithmId || descriptor.algorithmId || '未选择'}</strong></span>
              </div>
            )}
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

        {!configuring && <RunControlBar />}
        <div className="page-viewport">{children}</div>
      </div>
    </div>
  )
}
