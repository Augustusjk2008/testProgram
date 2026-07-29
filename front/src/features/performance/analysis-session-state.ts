import type {
  AnalysisChannel,
  AnalysisIdentity,
  AnalysisResult,
  AnalysisSnapshot,
  AnalysisState,
} from '../../shared/protocol'

const TERMINAL_STATES = new Set<AnalysisState>([
  'completed',
  'partial',
  'unavailable',
  'failed',
  'cancelled',
])

const STAGE_LABELS: Record<AnalysisState, string> = {
  none: '未开始分析',
  capturing: '采集分析输入',
  queued: '等待尾样本',
  validating: '校验',
  preprocessing: '预处理',
  calculating: '计算',
  persisting: '保存结果',
  completed: '分析完成',
  partial: '部分完成',
  unavailable: '无法计算',
  failed: '分析失败',
  cancelled: '已取消',
}

export interface AnalysisResultCacheSnapshot {
  identity: AnalysisIdentity | null
  entries: ReadonlyArray<AnalysisResult | undefined>
}

function isSafeIdentity(identity: AnalysisIdentity | null | undefined): identity is AnalysisIdentity {
  return Boolean(identity && identity.taskId.trim() &&
    Number.isSafeInteger(identity.analysisGeneration) && identity.analysisGeneration >= 0)
}

function sameIdentity(left: AnalysisIdentity | null, right: AnalysisIdentity | null): boolean {
  if (!left || !right) return left === right
  return left.taskId === right.taskId && left.analysisGeneration === right.analysisGeneration
}

export function analysisIdentityKey(identity: AnalysisIdentity): string {
  return `${identity.taskId}:${identity.analysisGeneration}`
}

export function analysisIdentityFromSnapshot(analysis: AnalysisSnapshot): AnalysisIdentity | null {
  const identity = {
    taskId: analysis.taskId,
    analysisGeneration: analysis.analysisGeneration,
  }
  return analysis.supported && isSafeIdentity(identity) ? identity : null
}

export function isAnalysisTerminal(state: AnalysisState): boolean {
  return TERMINAL_STATES.has(state)
}

export function isAnalysisBlockingWrites(analysis: AnalysisSnapshot): boolean {
  return analysis.supported && analysis.state !== 'none' && !isAnalysisTerminal(analysis.state)
}

export function analysisStageLabel(state: AnalysisState, stage = ''): string {
  const normalizedStage = stage.toLowerCase()
  if (normalizedStage.includes('seal')) return '封存数据'
  if (normalizedStage.includes('tail')) return '等待尾样本'
  if (state === 'calculating') {
    const channelMatch = /(?:channel|servo|helm)[_-]?(\d+)/i.exec(normalizedStage)
    if (channelMatch) {
      const channel = Number(channelMatch[1])
      if (Number.isInteger(channel) && channel >= 0 && channel < 4) return `计算舵 ${channel + 1}`
    }
  }
  return STAGE_LABELS[state]
}

export class AnalysisResultCache {
  private identity: AnalysisIdentity | null = null
  private entries: Array<AnalysisResult | undefined> = [undefined, undefined, undefined, undefined]

  begin(nextIdentity: AnalysisIdentity | null): boolean {
    const normalized = isSafeIdentity(nextIdentity) ? { ...nextIdentity } : null
    if (sameIdentity(this.identity, normalized)) return false
    this.identity = normalized
    this.entries = [undefined, undefined, undefined, undefined]
    return true
  }

  store(identity: AnalysisIdentity, channel: AnalysisChannel, result: AnalysisResult): boolean {
    if (!sameIdentity(this.identity, identity) || result.channelSummary.channel !== channel) return false
    this.entries[channel] = result
    return true
  }

  get(identity: AnalysisIdentity, channel: AnalysisChannel): AnalysisResult | undefined {
    if (!sameIdentity(this.identity, identity)) return undefined
    return this.entries[channel]
  }

  isCurrent(identity: AnalysisIdentity): boolean {
    return sameIdentity(this.identity, identity)
  }

  snapshot(): AnalysisResultCacheSnapshot {
    return {
      identity: this.identity ? { ...this.identity } : null,
      entries: [...this.entries],
    }
  }
}
