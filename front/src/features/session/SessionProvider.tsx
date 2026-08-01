import {
  createContext,
  type PropsWithChildren,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  useSyncExternalStore,
} from 'react'

import {
  CHART_COMMIT_INTERVAL_MS,
  DIAGNOSTIC_CAPACITY,
  HWTEST_WS_URL,
  SAMPLE_CAPACITY,
} from '../../shared/config'
import { connectionStateLabel, phaseLabel } from '../../shared/format'
import {
  EMPTY_SNAPSHOT,
  type ActionName,
  type AnalysisChannel,
  type AnalysisIdentity,
  type AnalysisResult,
  type ApplicationSample,
  type ApplicationSnapshot,
  type ConfigCatalog,
  type ConfigDocument,
  type DigitalStimulusSnapshot,
  type ReplyMessage,
  type SaveConfigRequest,
  type SerialPortInfo,
  type TestConfigOption,
  type TestRunOptions,
} from '../../shared/protocol'
import {
  parseTestConfigCatalog,
  type ConnectionState,
  HwtestClient,
  supportsTelemetryBatch,
} from '../../shared/ws/HwtestClient'
import type { SampleBuffer } from '../telemetry/sample-buffer'
import {
  TelemetryStore,
  type TelemetryStoreSnapshot,
} from '../telemetry/telemetry-store'
import {
  AnalysisResultCache,
  analysisIdentityFromSnapshot,
  analysisIdentityKey,
} from '../performance/analysis-session-state'
import { PerformanceNavigationGate } from '../performance/performance-navigation'
import { tracksGlobalBusyAction } from './action-policy'
import { selectInitialTestConfig } from './config-selection'

export interface DiagnosticEntry {
  id: number
  timestamp: number
  kind: 'link' | 'command' | 'snapshot' | 'sample' | 'error'
  title: string
  detail: string
  payload?: unknown
}

interface SessionContextValue {
  wsUrl: string
  connectionState: ConnectionState
  connectionDetail: string
  snapshot: ApplicationSnapshot
  configCatalog: ConfigCatalog | null
  configCatalogReady: boolean
  configCatalogError: string
  testConfigs: TestConfigOption[]
  testConfigsReady: boolean
  serialPorts: SerialPortInfo[]
  selectedConfigId: string
  busyAction: ActionName | null
  actionError: string
  analysisResults: ReadonlyArray<AnalysisResult | undefined>
  analysisResultLoading: ReadonlyArray<boolean>
  analysisResultErrors: ReadonlyArray<string>
  performanceNavigationIdentity: AnalysisIdentity | null
  connect: (reconnecting?: boolean) => Promise<void>
  invoke: (action: ActionName, params?: Record<string, unknown>) => Promise<ReplyMessage>
  refreshConfigCatalog: () => Promise<ConfigCatalog>
  getConfigDocument: (documentId: string) => Promise<ConfigDocument>
  saveConfig: (request: SaveConfigRequest) => Promise<ConfigDocument>
  start: (options: TestRunOptions) => Promise<ReplyMessage>
  setDigitalStimulus: (
    switchId: string,
    active: boolean,
    expectedRevision: number,
  ) => Promise<DigitalStimulusSnapshot>
  resetDigitalStimulus: () => Promise<DigitalStimulusSnapshot>
  fetchAnalysisResult: (
    identity: AnalysisIdentity,
    channel: AnalysisChannel,
  ) => Promise<AnalysisResult>
}

interface TelemetryContextValue {
  store: TelemetryStore
  diagnostics: DiagnosticEntry[]
  clearTelemetry: () => void
}

export interface SessionTelemetryValue {
  latestSample: ApplicationSample | null
  telemetry: SampleBuffer
  fields: string[]
  dataVersion: number
  telemetryStats: TelemetryStoreSnapshot
  diagnostics: DiagnosticEntry[]
  clearTelemetry: () => void
}

const SessionContext = createContext<SessionContextValue | null>(null)
const TelemetryContext = createContext<TelemetryContextValue | null>(null)

function parseSerialPorts(value: unknown): SerialPortInfo[] {
  if (!Array.isArray(value)) throw new Error('后端 ports 不是数组')
  const names = new Set<string>()
  return value.map((item) => {
    if (typeof item !== 'object' || item === null || Array.isArray(item)) {
      throw new Error('后端串口条目不是对象')
    }
    const record = item as Record<string, unknown>
    const read = (field: keyof SerialPortInfo) => {
      const fieldValue = record[field]
      if (typeof fieldValue !== 'string') throw new Error(`后端串口字段 ${field} 不是字符串`)
      return fieldValue
    }
    const port: SerialPortInfo = {
      portName: read('portName').trim(),
      description: read('description'),
      manufacturer: read('manufacturer'),
      serialNumber: read('serialNumber'),
      systemLocation: read('systemLocation'),
    }
    if (!port.portName || names.has(port.portName.toLowerCase())) {
      throw new Error('后端串口名称为空或重复')
    }
    names.add(port.portName.toLowerCase())
    return port
  })
}

export function SessionProvider({ children }: PropsWithChildren) {
  const clientRef = useRef<HwtestClient | null>(null)
  const telemetryStoreRef = useRef<TelemetryStore | null>(null)
  const diagnosticsRef = useRef<DiagnosticEntry[]>([])
  const diagnosticSequence = useRef(0)
  const descriptorConfigId = useRef('')
  const configCatalogRef = useRef<ConfigCatalog | null>(null)
  const snapshotRef = useRef<ApplicationSnapshot>(EMPTY_SNAPSHOT)
  const sampleCountMismatchRef = useRef('')
  const autoLoadInFlight = useRef(false)
  const analysisCacheRef = useRef(new AnalysisResultCache())
  const analysisRequestRef = useRef(new Map<string, Promise<AnalysisResult>>())
  const performanceNavigationGateRef = useRef(new PerformanceNavigationGate())

  const [connectionState, setConnectionState] = useState<ConnectionState>('disconnected')
  const [connectionDetail, setConnectionDetail] = useState('')
  const [snapshot, setSnapshot] = useState<ApplicationSnapshot>(EMPTY_SNAPSHOT)
  const [configCatalog, setConfigCatalog] = useState<ConfigCatalog | null>(null)
  const [configCatalogReady, setConfigCatalogReady] = useState(false)
  const [configCatalogError, setConfigCatalogError] = useState('')
  const [testConfigs, setTestConfigs] = useState<TestConfigOption[]>([])
  const [testConfigsReady, setTestConfigsReady] = useState(false)
  const [serialPorts, setSerialPorts] = useState<SerialPortInfo[]>([])
  const [selectedConfigId, setSelectedConfigId] = useState('')
  const [diagnostics, setDiagnostics] = useState<DiagnosticEntry[]>([])
  const [busyAction, setBusyAction] = useState<ActionName | null>(null)
  const [actionError, setActionError] = useState('')
  const [analysisResults, setAnalysisResults] = useState<ReadonlyArray<AnalysisResult | undefined>>(
    [undefined, undefined, undefined, undefined],
  )
  const [analysisResultLoading, setAnalysisResultLoading] = useState<ReadonlyArray<boolean>>(
    [false, false, false, false],
  )
  const [analysisResultErrors, setAnalysisResultErrors] = useState<ReadonlyArray<string>>(
    ['', '', '', ''],
  )
  const [performanceNavigationIdentity, setPerformanceNavigationIdentity] = useState<AnalysisIdentity | null>(null)

  if (!telemetryStoreRef.current) {
    telemetryStoreRef.current = new TelemetryStore(SAMPLE_CAPACITY, {
      commitIntervalMs: CHART_COMMIT_INTERVAL_MS,
      onCommit: () => setDiagnostics(diagnosticsRef.current),
    })
  }
  const telemetryStore = telemetryStoreRef.current

  const pushDiagnostic = useCallback((
    kind: DiagnosticEntry['kind'],
    title: string,
    detail: string,
    payload?: unknown,
    deferCommit = false,
  ) => {
    const entry: DiagnosticEntry = {
      id: ++diagnosticSequence.current,
      timestamp: Date.now(),
      kind,
      title,
      detail,
      payload,
    }
    diagnosticsRef.current = [entry, ...diagnosticsRef.current]
      .slice(0, DIAGNOSTIC_CAPACITY)
    if (!deferCommit) setDiagnostics(diagnosticsRef.current)
  }, [])

  const clearTelemetry = useCallback(() => {
    sampleCountMismatchRef.current = ''
    telemetryStore.clear()
  }, [telemetryStore])

  const publishPerformanceNavigation = useCallback((identity: AnalysisIdentity | null) => {
    if (identity) setPerformanceNavigationIdentity({ ...identity })
  }, [])

  const synchronizeAnalysisIdentity = useCallback((nextSnapshot: ApplicationSnapshot) => {
    const identity = analysisIdentityFromSnapshot(nextSnapshot.analysis)
    if (analysisCacheRef.current.begin(identity)) {
      analysisRequestRef.current.clear()
      setAnalysisResults(analysisCacheRef.current.snapshot().entries)
      setAnalysisResultLoading([false, false, false, false])
      setAnalysisResultErrors(['', '', '', ''])
    }
    publishPerformanceNavigation(
      performanceNavigationGateRef.current.observe(identity, nextSnapshot.analysis.state),
    )
  }, [publishPerformanceNavigation])

  const connect = useCallback(async (reconnecting = true) => {
    setActionError('')
    await clientRef.current?.connect(reconnecting)
  }, [])

  const invoke = useCallback(async (
    action: ActionName,
    params: Record<string, unknown> = {},
  ): Promise<ReplyMessage> => {
    const client = clientRef.current
    if (!client) throw new Error('WebSocket client is unavailable')
    const tracksGlobalBusy = tracksGlobalBusyAction(action)
    if (tracksGlobalBusy) setBusyAction(action)
    setActionError('')
    pushDiagnostic('command', `发送 · ${action}`, JSON.stringify(params), params)
    try {
      const reply = await client.request(action, params)
      pushDiagnostic(
        reply.ok ? 'command' : 'error',
        `${reply.ok ? '完成' : '拒绝'} · ${action}`,
        reply.message || reply.code || 'ok',
        reply,
      )
      if (!reply.ok) setActionError(reply.message || reply.code)
      if (action === 'stop' && reply.ok) {
        const analysis = snapshotRef.current.analysis
        const identity = analysisIdentityFromSnapshot(analysis)
        performanceNavigationGateRef.current.recordStopSucceeded(identity)
        publishPerformanceNavigation(
          performanceNavigationGateRef.current.observe(identity, analysis.state),
        )
      }
      return reply
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error)
      setActionError(message)
      pushDiagnostic('error', `失败 · ${action}`, message)
      throw error
    } finally {
      if (tracksGlobalBusy) setBusyAction(null)
    }
  }, [publishPerformanceNavigation, pushDiagnostic])

  const refreshConfigCatalog = useCallback(async (): Promise<ConfigCatalog> => {
    const client = clientRef.current
    if (!client) throw new Error('WebSocket client is unavailable')
    setConfigCatalogReady(false)
    setConfigCatalogError('')
    try {
      const catalog = await client.getConfigCatalog()
      configCatalogRef.current = catalog
      setConfigCatalog(catalog)
      pushDiagnostic('command', '完成 · configCatalog', `${catalog.items.length} 个配置文档`, catalog)
      return catalog
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error)
      configCatalogRef.current = null
      setConfigCatalog(null)
      setConfigCatalogError(message)
      pushDiagnostic('error', '读取配置目录失败', message)
      throw error
    } finally {
      setConfigCatalogReady(true)
    }
  }, [pushDiagnostic])

  const getConfigDocument = useCallback(async (documentId: string): Promise<ConfigDocument> => {
    const client = clientRef.current
    if (!client) throw new Error('WebSocket client is unavailable')
    pushDiagnostic('command', '发送 · configDocument', documentId, { documentId })
    try {
      const document = await client.getConfigDocument(documentId)
      pushDiagnostic('command', '完成 · configDocument', document.documentId, document)
      return document
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error)
      pushDiagnostic('error', '读取配置文档失败', message, { documentId })
      throw error
    }
  }, [pushDiagnostic])

  const saveConfig = useCallback(async (request: SaveConfigRequest): Promise<ConfigDocument> => {
    const client = clientRef.current
    if (!client) throw new Error('WebSocket client is unavailable')
    pushDiagnostic('command', '发送 · saveConfig', request.documentId, request)
    try {
      const document = await client.saveConfig(request)
      pushDiagnostic('command', '完成 · saveConfig', document.documentId, document)
      await refreshConfigCatalog().catch(() => undefined)
      if (request.documentId === 'test-config-catalog') {
        try {
          const reply = await client.request('testConfigs')
          if (!reply.ok) throw new Error(reply.message || reply.code)
          const tests = parseTestConfigCatalog(reply.data)
          setTestConfigs(tests.configs)
          setSelectedConfigId(tests.selectedConfigId)
          setTestConfigsReady(true)
        } catch (error) {
          const message = error instanceof Error ? error.message : String(error)
          pushDiagnostic('error', '刷新运行测试目录失败', message)
        }
      }
      return document
    } catch (error) {
      const message = error instanceof Error ? error.message : String(error)
      pushDiagnostic('error', '保存配置失败', message, { documentId: request.documentId })
      throw error
    }
  }, [pushDiagnostic, refreshConfigCatalog])

  const fetchAnalysisResult = useCallback(async (
    identity: AnalysisIdentity,
    channel: AnalysisChannel,
  ): Promise<AnalysisResult> => {
    const cache = analysisCacheRef.current
    if (!cache.isCurrent(identity)) {
      throw new Error('分析身份已过期，已丢弃结果读取请求')
    }
    const cached = cache.get(identity, channel)
    if (cached) return cached

    const requestKey = `${analysisIdentityKey(identity)}:${channel}`
    const pending = analysisRequestRef.current.get(requestKey)
    if (pending) return pending

    const client = clientRef.current
    if (!client) throw new Error('WebSocket client is unavailable')
    const request = (async () => {
      if (cache.isCurrent(identity)) {
        setAnalysisResultLoading((current) => current.map((value, index) => (
          index === channel ? true : value
        )))
        setAnalysisResultErrors((current) => current.map((value, index) => (
          index === channel ? '' : value
        )))
      }
      try {
        const params = {
          taskId: identity.taskId,
          analysisGeneration: identity.analysisGeneration,
          channel,
        }
        pushDiagnostic('command', '发送 · analysisResult', JSON.stringify(params), params)
        const reply = await client.request('analysisResult', params)
        if (!reply.ok) throw new Error(reply.message || reply.code || 'analysisResult was rejected')
        const result = reply.data.analysisResult
        if (!result) throw new Error('后端未返回 analysisResult；该服务端可能不支持性能结果读取')
        if (result.channelSummary.channel !== channel) {
          throw new Error('后端返回的 analysisResult 通道与请求不一致')
        }
        if (cache.store(identity, channel, result)) {
          setAnalysisResults(cache.snapshot().entries)
          setAnalysisResultErrors((current) => current.map((value, index) => (
            index === channel ? '' : value
          )))
          pushDiagnostic('command', '完成 · analysisResult', `舵 ${channel + 1} 结果已加载`, reply)
        }
        return result
      } catch (error) {
        const message = error instanceof Error ? error.message : String(error)
        if (cache.isCurrent(identity)) {
          setAnalysisResultErrors((current) => current.map((value, index) => (
            index === channel ? message : value
          )))
          pushDiagnostic('error', '失败 · analysisResult', message)
        }
        throw error
      } finally {
        analysisRequestRef.current.delete(requestKey)
        if (cache.isCurrent(identity)) {
          setAnalysisResultLoading((current) => current.map((value, index) => (
            index === channel ? false : value
          )))
        }
      }
    })()
    analysisRequestRef.current.set(requestKey, request)
    return request
  }, [pushDiagnostic])

  useEffect(() => {
    const client = new HwtestClient(HWTEST_WS_URL)
    clientRef.current = client
    const unsubscribe = client.subscribe((event) => {
      if (event.type === 'connection') {
        if (event.state === 'disconnected' || event.state === 'reconnecting') {
          telemetryStore.markReconnected()
        }
        setConnectionState(event.state)
        setConnectionDetail(event.detail ?? '')
        pushDiagnostic(
          event.state === 'error' ? 'error' : 'link',
          `WebSocket · ${connectionStateLabel(event.state)}`,
          event.detail ?? HWTEST_WS_URL,
        )
        return
      }

      if (event.type === 'samples') {
        try {
          const summary = telemetryStore.appendMany({
            firstSeq: event.firstSeq,
            lastSeq: event.lastSeq,
            samples: event.samples,
          })
          const sequenceDetail = summary.sequenceIssue === undefined
            ? ''
            : `；序号${summary.sequenceIssue === 'gap' ? '存在缺口' : '重复或倒序'}`
          pushDiagnostic(
            summary.sequenceIssue === undefined ? 'sample' : 'error',
            summary.sequenceIssue === undefined
              ? `样本 · #${summary.cycleIndexRange[0]}–${summary.cycleIndexRange[1]}`
              : '样本序号不完整',
            `seq ${summary.firstSeq}–${summary.lastSeq} · ${summary.sampleCount} 条 · ${summary.channelIds.join('、')}${sequenceDetail}`,
            summary,
            summary.sequenceIssue === undefined,
          )
        } catch (error) {
          const detail = error instanceof Error ? error.message : String(error)
          pushDiagnostic('error', '遥测批次被拒绝', detail)
        }
        return
      }

      const message = event.message
      if (message.type === 'snapshot') {
        const nextConfigId = message.snapshot.descriptor.configId
        if (nextConfigId && nextConfigId !== descriptorConfigId.current) {
          if (descriptorConfigId.current) clearTelemetry()
          descriptorConfigId.current = nextConfigId
        }
        const nextSnapshot = { ...EMPTY_SNAPSHOT, ...message.snapshot }
        snapshotRef.current = nextSnapshot
        setSnapshot(nextSnapshot)
        synchronizeAnalysisIdentity(nextSnapshot)
        const telemetryStats = telemetryStore.currentStats()
        const isTerminal = ['finished', 'stopped', 'error'].includes(nextSnapshot.phase)
        const mismatchKey = `${nextSnapshot.taskId}:${nextSnapshot.sampleCount}:${telemetryStats.receivedCount}`
        if (isTerminal &&
            telemetryStats.sequenceComplete &&
            telemetryStats.taskId === nextSnapshot.taskId &&
            Number.isSafeInteger(nextSnapshot.sampleCount) &&
            nextSnapshot.sampleCount !== telemetryStats.receivedCount &&
            sampleCountMismatchRef.current !== mismatchKey) {
          sampleCountMismatchRef.current = mismatchKey
          pushDiagnostic(
            'error',
            '样本计数不一致',
            `后端 ${nextSnapshot.sampleCount} 条，浏览器 ${telemetryStats.receivedCount} 条`,
          )
        }
        pushDiagnostic(
          'snapshot',
          `状态 · ${phaseLabel(message.snapshot.phase)}`,
          message.snapshot.progressStep || `seq ${message.seq}`,
          message,
        )
      } else if (message.type === 'hello') {
        setTestConfigsReady(false)
        pushDiagnostic(
          'link',
          `已连接 ${message.server}`,
          `协议 v${message.protocolVersion}`,
          message,
        )
        void client.request('ports').then((reply) => {
          if (!reply.ok) throw new Error(reply.message || reply.code || '读取串口失败')
          setSerialPorts(parseSerialPorts(reply.data.ports))
        }).catch((error) => {
          const detail = error instanceof Error ? error.message : String(error)
          setSerialPorts([])
          pushDiagnostic('error', '读取本地串口失败', detail)
        })
        const requestTestConfigs = () => {
          void client.request('testConfigs').then((reply) => {
            setTestConfigsReady(true)
            if (!reply.ok) {
              setActionError(reply.message || reply.code)
              pushDiagnostic(
                'error',
                '测试配置目录不可用',
                reply.message || reply.code,
                reply,
              )
              return
            }
            try {
              const catalog = parseTestConfigCatalog(reply.data)
              const disabledConfigIds = new Set((configCatalogRef.current?.items ?? [])
                .filter((item) => !item.enabled)
                .map((item) => item.configId))
              const initialConfig = selectInitialTestConfig(catalog, disabledConfigIds)
              setTestConfigs(catalog.configs)
              setSelectedConfigId(initialConfig?.configId ?? '')
              if (!initialConfig) {
                setActionError('没有可加载的测试配置')
                pushDiagnostic('error', '测试配置目录为空', '没有发现可用测试配置', catalog)
                return
              }
              if (snapshotRef.current.phase !== 'empty' || autoLoadInFlight.current) return

              autoLoadInFlight.current = true
              const action: ActionName = initialConfig.configId === catalog.selectedConfigId
                ? 'load'
                : 'selectTest'
              const params = action === 'selectTest'
                ? { configId: initialConfig.configId }
                : {}
              void invoke(action, params)
                .catch(() => undefined)
                .finally(() => {
                  autoLoadInFlight.current = false
                })
            } catch (error) {
              const detail = error instanceof Error ? error.message : String(error)
              setActionError(detail)
              pushDiagnostic('error', '测试配置目录格式错误', detail, reply)
            }
          }).catch((error) => {
            const detail = error instanceof Error ? error.message : String(error)
            setTestConfigsReady(true)
            setActionError(detail)
            pushDiagnostic('error', '读取测试配置目录失败', detail)
          })
        }
        const requestConfigCatalog = () => refreshConfigCatalog().catch(() => undefined)

        if (!supportsTelemetryBatch(message)) {
          void requestConfigCatalog().finally(requestTestConfigs)
          return
        }

        const deliveryParams = { mode: 'batch' }
        pushDiagnostic('command', '发送 · setTelemetryDelivery', JSON.stringify(deliveryParams), deliveryParams)
        void client.request('setTelemetryDelivery', deliveryParams)
          .then((reply) => {
            if (!reply.ok) {
              pushDiagnostic(
                'error',
                '批量遥测协商失败，已回退单条模式',
                reply.message || reply.code || '后端拒绝批量遥测协商',
                reply,
              )
              return
            }
            pushDiagnostic('command', '完成 · setTelemetryDelivery', '已启用批量遥测', reply)
          })
          .catch((error) => {
            const detail = error instanceof Error ? error.message : String(error)
            pushDiagnostic('error', '批量遥测协商失败，已回退单条模式', detail)
          })
          .finally(() => {
            void requestConfigCatalog().finally(requestTestConfigs)
          })
      }
    })

    void client.connect().catch(() => undefined)
    return () => {
      unsubscribe()
      client.close()
      telemetryStore.dispose()
      autoLoadInFlight.current = false
    }
  }, [clearTelemetry, invoke, pushDiagnostic, refreshConfigCatalog, synchronizeAnalysisIdentity, telemetryStore])

  const start = useCallback(async (options: TestRunOptions) => {
    clearTelemetry()
    return invoke('start', { ...options })
  }, [clearTelemetry, invoke])

  const applyDigitalStimulusReply = useCallback((
    action: 'setDigitalStimulus' | 'resetDigitalStimulus',
    reply: ReplyMessage,
    requestConfigId: string,
  ): DigitalStimulusSnapshot => {
    const digitalStimulus = reply.data.digitalStimulus
    const currentSnapshot = snapshotRef.current
    if (digitalStimulus &&
        currentSnapshot.descriptor.configId === requestConfigId &&
        digitalStimulus.revision >= currentSnapshot.digitalStimulus.revision) {
      const nextSnapshot = { ...currentSnapshot, digitalStimulus }
      snapshotRef.current = nextSnapshot
      setSnapshot(nextSnapshot)
    }

    if (!reply.ok) {
      throw new Error(reply.message || reply.code || `${action} was rejected`)
    }
    if (digitalStimulus) return digitalStimulus

    const message = `${action} reply is missing data.digitalStimulus`
    setActionError(message)
    pushDiagnostic('error', `失败 · ${action}`, message, reply)
    throw new Error(message)
  }, [pushDiagnostic])

  const setDigitalStimulus = useCallback(async (
    switchId: string,
    active: boolean,
    expectedRevision: number,
  ) => {
    const requestConfigId = snapshotRef.current.descriptor.configId
    const reply = await invoke('setDigitalStimulus', {
      switchId,
      active,
      expectedRevision,
    })
    return applyDigitalStimulusReply('setDigitalStimulus', reply, requestConfigId)
  }, [applyDigitalStimulusReply, invoke])

  const resetDigitalStimulus = useCallback(async () => {
    const requestConfigId = snapshotRef.current.descriptor.configId
    const reply = await invoke('resetDigitalStimulus')
    return applyDigitalStimulusReply('resetDigitalStimulus', reply, requestConfigId)
  }, [applyDigitalStimulusReply, invoke])

  const value = useMemo<SessionContextValue>(() => ({
    wsUrl: HWTEST_WS_URL,
    connectionState,
    connectionDetail,
    snapshot,
    configCatalog,
    configCatalogReady,
    configCatalogError,
    testConfigs,
    testConfigsReady,
    serialPorts,
    selectedConfigId,
    busyAction,
    actionError,
    analysisResults,
    analysisResultLoading,
    analysisResultErrors,
    performanceNavigationIdentity,
    connect,
    invoke,
    refreshConfigCatalog,
    getConfigDocument,
    saveConfig,
    start,
    setDigitalStimulus,
    resetDigitalStimulus,
    fetchAnalysisResult,
  }), [
    actionError,
    analysisResultErrors,
    analysisResultLoading,
    analysisResults,
    busyAction,
    connect,
    connectionDetail,
    connectionState,
    configCatalog,
    configCatalogError,
    configCatalogReady,
    fetchAnalysisResult,
    getConfigDocument,
    invoke,
    performanceNavigationIdentity,
    refreshConfigCatalog,
    resetDigitalStimulus,
    saveConfig,
    selectedConfigId,
    serialPorts,
    setDigitalStimulus,
    snapshot,
    start,
    testConfigs,
    testConfigsReady,
  ])

  const telemetryValue = useMemo<TelemetryContextValue>(() => ({
    store: telemetryStore,
    diagnostics,
    clearTelemetry,
  }), [clearTelemetry, diagnostics, telemetryStore])

  return (
    <SessionContext.Provider value={value}>
      <TelemetryContext.Provider value={telemetryValue}>
        {children}
      </TelemetryContext.Provider>
    </SessionContext.Provider>
  )
}

export function useSession(): SessionContextValue {
  const context = useContext(SessionContext)
  if (!context) throw new Error('useSession must be used inside SessionProvider')
  return context
}

export function useTelemetry(): SessionTelemetryValue {
  const context = useContext(TelemetryContext)
  if (!context) throw new Error('useTelemetry must be used inside SessionProvider')
  const snapshot = useSyncExternalStore(
    context.store.subscribe,
    context.store.getSnapshot,
    context.store.getSnapshot,
  )
  return useMemo(() => ({
    latestSample: snapshot.latestSample,
    telemetry: context.store.buffer,
    fields: snapshot.fields,
    dataVersion: snapshot.version,
    telemetryStats: snapshot,
    diagnostics: context.diagnostics,
    clearTelemetry: context.clearTelemetry,
  }), [context, snapshot])
}
