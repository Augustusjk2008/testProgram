import type {
  ActionName,
  AnalysisChannel,
  AnalysisChannelStatus,
  AnalysisChannelSummary,
  AnalysisMetric,
  AnalysisResult,
  AnalysisSnapshot,
  AnalysisState,
  BodeProjection,
  ApplicationSample,
  ApplicationSnapshot,
  DigitalStimulusSnapshot,
  DigitalSwitchDescriptor,
  HelloMessage,
  ReplyData,
  ReplyMessage,
  RunMode,
  SampleBatchMessage,
  ServerMessage,
  TestConfigCatalog,
  TestConfigOption,
  TestDescriptor,
  TestMeasurementDescriptor,
  TestRunParameterDescriptor,
} from '../protocol'
import {
  EMPTY_ANALYSIS,
  EMPTY_DIGITAL_STIMULUS,
  EMPTY_TEST_DESCRIPTOR,
} from '../protocol'

type JsonObject = Record<string, unknown>

function isObject(value: unknown): value is JsonObject {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}

function requiredObject(parent: JsonObject, key: string): JsonObject {
  const value = parent[key]
  if (!isObject(value)) {
    throw new Error(`Invalid protocol field: ${key}`)
  }
  return value
}

function requiredString(parent: JsonObject, key: string): string {
  const value = parent[key]
  if (typeof value !== 'string') {
    throw new Error(`Invalid protocol field: ${key}`)
  }
  return value
}

function requiredNumber(parent: JsonObject, key: string): number {
  const value = parent[key]
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new Error(`Invalid protocol field: ${key}`)
  }
  return value
}

function optionalSafeNonNegativeInteger(parent: JsonObject, key: string): number | undefined {
  if (!Object.prototype.hasOwnProperty.call(parent, key)) return undefined
  return requiredSafeInteger(parent, key)
}

function requiredSafeInteger(
  parent: JsonObject,
  key: string,
  minimum = 0,
  maximum = Number.MAX_SAFE_INTEGER,
): number {
  const value = parent[key]
  if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < minimum || value > maximum) {
    throw new Error(`Invalid protocol field: ${key}`)
  }
  return value
}

function requiredBoolean(parent: JsonObject, key: string): boolean {
  const value = parent[key]
  if (typeof value !== 'boolean') {
    throw new Error(`Invalid protocol field: ${key}`)
  }
  return value
}

function requiredNonEmptyString(parent: JsonObject, key: string): string {
  const value = requiredString(parent, key)
  if (!value.trim()) {
    throw new Error(`Invalid protocol field: ${key}`)
  }
  return value
}

function requiredArray(parent: JsonObject, key: string): unknown[] {
  const value = parent[key]
  if (!Array.isArray(value)) {
    throw new Error(`Invalid protocol field: ${key}`)
  }
  return value
}

function requiredNullableFiniteNumber(value: unknown, field: string): number | null {
  if (value === null) return null
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new Error(`Invalid protocol field: ${field}`)
  }
  return value
}

function requiredDigitalLevel(parent: JsonObject, key: string): DigitalSwitchDescriptor['activeLevel'] {
  const value = requiredString(parent, key)
  if (value !== 'High' && value !== 'Low') {
    throw new Error(`Invalid protocol field: ${key}`)
  }
  return value
}

function parseRunParameterScalar(value: unknown, field: string): string | number | boolean {
  if (typeof value === 'string' || typeof value === 'boolean') return value
  if (typeof value === 'number' && Number.isFinite(value)) return value
  throw new Error(field)
}

function runParameterMap(value: unknown, field: string): Record<string, unknown> {
  if (!isObject(value)) throw new Error(field)
  const result: Record<string, unknown> = {}
  for (const [key, item] of Object.entries(value)) {
    result[key] = parseRunParameterScalar(item, field)
  }
  return result
}

function emptyAnalysis() : AnalysisSnapshot {
  return { ...EMPTY_ANALYSIS, channelSummaries: [], sourceSummary: {} }
}

function parsePostRunAnalysisCapability(value: JsonObject): TestDescriptor['postRunAnalysis'] {
  try {
    return {
      supported: requiredBoolean(value, 'supported'),
      analyzerId: requiredString(value, 'analyzerId'),
      schemaVersion: requiredString(value, 'schemaVersion'),
    }
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error)
    throw new Error(`Invalid protocol postRunAnalysis: ${detail}`)
  }
}

function parseAnalysisState(value: unknown): AnalysisState {
  if (value === 'none' || value === 'capturing' || value === 'queued' ||
      value === 'validating' || value === 'preprocessing' || value === 'calculating' ||
      value === 'persisting' || value === 'completed' || value === 'partial' ||
      value === 'unavailable' || value === 'failed' || value === 'cancelled') {
    return value
  }
  throw new Error('Invalid protocol field: analysis.state')
}

function parseAnalysisChannelStatus(value: unknown): AnalysisChannelStatus {
  if (value === 'not_applicable' || value === 'completed' ||
      value === 'partial' || value === 'unavailable') {
    return value
  }
  throw new Error('Invalid protocol field: analysis.channel.status')
}

function parseAnalysisMetric(value: JsonObject): AnalysisMetric {
  return {
    key: requiredNonEmptyString(value, 'key'),
    label: requiredString(value, 'label'),
    unit: requiredString(value, 'unit'),
    status: requiredNonEmptyString(value, 'status'),
    value: requiredNullableFiniteNumber(value.value, 'metric.value'),
    detail: requiredString(value, 'detail'),
  }
}

function parseAnalysisMetrics(value: unknown, field: string): AnalysisMetric[] {
  if (!Array.isArray(value)) throw new Error(`Invalid protocol field: ${field}`)
  return value.map((item) => {
    if (!isObject(item)) throw new Error(`Invalid protocol field: ${field}`)
    return parseAnalysisMetric(item)
  })
}

function parseAnalysisChannelSummary(value: JsonObject): AnalysisChannelSummary {
  const channel = requiredSafeInteger(value, 'channel', 0, 3) as AnalysisChannel
  const warnings = requiredArray(value, 'warnings').map((warning) => {
    if (typeof warning !== 'string') throw new Error('Invalid protocol field: analysis.warnings')
    return warning
  })
  const omittedWarningCount = Object.prototype.hasOwnProperty.call(value, 'omittedWarningCount')
    ? requiredSafeInteger(value, 'omittedWarningCount')
    : undefined
  return {
    channel,
    enabled: requiredBoolean(value, 'enabled'),
    status: parseAnalysisChannelStatus(value.status),
    warnings,
    ...(omittedWarningCount === undefined ? {} : { omittedWarningCount }),
    commonMetrics: parseAnalysisMetrics(value.commonMetrics, 'analysis.commonMetrics'),
    waveformMetrics: parseAnalysisMetrics(value.waveformMetrics, 'analysis.waveformMetrics'),
    bodeAvailable: requiredBoolean(value, 'bodeAvailable'),
    bodePointCount: requiredSafeInteger(value, 'bodePointCount'),
    reasonCode: requiredString(value, 'reasonCode'),
    message: requiredString(value, 'message'),
  }
}

function parseAnalysisSnapshot(value: JsonObject): AnalysisSnapshot {
  try {
    const channelSummaries = requiredArray(value, 'channelSummaries').map((item) => {
      if (!isObject(item)) throw new Error('Invalid protocol field: analysis.channelSummaries')
      return parseAnalysisChannelSummary(item)
    })
    const channels = new Set<number>()
    for (const summary of channelSummaries) {
      if (channels.has(summary.channel)) {
        throw new Error('Invalid protocol field: analysis.channelSummaries')
      }
      channels.add(summary.channel)
    }
    return {
      supported: requiredBoolean(value, 'supported'),
      analyzerId: requiredString(value, 'analyzerId'),
      schemaVersion: requiredString(value, 'schemaVersion'),
      taskId: requiredString(value, 'taskId'),
      analysisGeneration: requiredSafeInteger(value, 'analysisGeneration'),
      state: parseAnalysisState(value.state),
      progress: requiredNumber(value, 'progress'),
      stage: requiredString(value, 'stage'),
      message: requiredString(value, 'message'),
      reasonCode: requiredString(value, 'reasonCode'),
      resultFilePath: requiredString(value, 'resultFilePath'),
      diagnosticInputFilePath: requiredString(value, 'diagnosticInputFilePath'),
      sourceSummary: requiredObject(value, 'sourceSummary'),
      channelSummaries,
    }
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error)
    throw new Error(`Invalid protocol analysis: ${detail}`)
  }
}

function parseBodeProjection(value: JsonObject): BodeProjection {
  const frequencyHz = requiredArray(value, 'frequencyHz').map((item) => {
    if (typeof item !== 'number' || !Number.isFinite(item) || item <= 0) {
      throw new Error('Invalid protocol field: bode.frequencyHz')
    }
    return item
  })
  const magnitudeDb = requiredArray(value, 'magnitudeDb')
    .map((item) => requiredNullableFiniteNumber(item, 'bode.magnitudeDb'))
  const phaseDeg = requiredArray(value, 'phaseDeg')
    .map((item) => requiredNullableFiniteNumber(item, 'bode.phaseDeg'))
  const pointStatus = requiredArray(value, 'pointStatus').map((item) => {
    if (typeof item !== 'string' || !item) throw new Error('Invalid protocol field: bode.pointStatus')
    return item
  })
  const length = frequencyHz.length
  if (magnitudeDb.length !== length || phaseDeg.length !== length || pointStatus.length !== length) {
    throw new Error('Invalid protocol bode: arrays must have equal lengths')
  }
  return { frequencyHz, magnitudeDb, phaseDeg, pointStatus }
}

function parseAnalysisResult(value: JsonObject): AnalysisResult {
  try {
    return {
      channelSummary: parseAnalysisChannelSummary(requiredObject(value, 'channelSummary')),
      bode: parseBodeProjection(requiredObject(value, 'bode')),
    }
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error)
    throw new Error(`Invalid protocol analysisResult: ${detail}`)
  }
}

function parseDescriptor(value: JsonObject): TestDescriptor {
  try {
    const modeValue = value.supportedRunModes
    if (!Array.isArray(modeValue) || !modeValue.every((mode) => (
      mode === 'single' || mode === 'pc_periodic' || mode === 'device_stream'
    ))) {
      throw new Error('supportedRunModes')
    }
    const measurementValue = value.measurements
    if (!Array.isArray(measurementValue)) throw new Error('measurements')
    const measurements: TestMeasurementDescriptor[] = measurementValue.map((item) => {
      if (!isObject(item)) throw new Error('measurement')
      return {
        id: requiredString(item, 'id'),
        label: requiredString(item, 'label'),
        unit: requiredString(item, 'unit'),
        primary: requiredBoolean(item, 'primary'),
      }
    })
    const schemaVersion = Object.prototype.hasOwnProperty.call(value, 'runParameterSchemaVersion')
      ? requiredString(value, 'runParameterSchemaVersion')
      : ''
    const runParametersValue = value.runParameters ?? []
    if (!Array.isArray(runParametersValue)) throw new Error('runParameters')
    const parameterIds = new Set<string>()
    const runParameters: TestRunParameterDescriptor[] = runParametersValue.map((item) => {
      if (!isObject(item)) throw new Error('runParameter')
      const id = requiredNonEmptyString(item, 'id')
      if (parameterIds.has(id)) throw new Error('duplicate run parameter')
      parameterIds.add(id)
      const kind = requiredString(item, 'kind')
      if (kind !== 'integer' && kind !== 'number' && kind !== 'boolean' && kind !== 'choice') {
        throw new Error('run parameter kind')
      }
      const choicesValue = item.choices
      if (!Array.isArray(choicesValue)) throw new Error('run parameter choices')
      const choices = choicesValue.map((choice) => {
        if (!isObject(choice)) throw new Error('run parameter choice')
        return {
          value: parseRunParameterScalar(choice.value, 'run parameter choice value'),
          label: requiredString(choice, 'label'),
        }
      })
      const minimum = item.minimum === undefined
        ? undefined
        : requiredNumber(item, 'minimum')
      const maximum = item.maximum === undefined
        ? undefined
        : requiredNumber(item, 'maximum')
      const persistValues = item.persistValues === undefined
        ? undefined
        : requiredBoolean(item, 'persistValues')
      let visibleWhen
      if (item.visibleWhen !== undefined) {
        if (!isObject(item.visibleWhen)) throw new Error('run parameter visibility')
        visibleWhen = {
          parameter: requiredNonEmptyString(item.visibleWhen, 'parameter'),
          equals: parseRunParameterScalar(item.visibleWhen.equals, 'run parameter visibility value'),
        }
      }
      return {
        id,
        label: requiredString(item, 'label'),
        description: requiredString(item, 'description'),
        kind,
        unit: requiredString(item, 'unit'),
        required: requiredBoolean(item, 'required'),
        minimum,
        maximum,
        minimumExclusive: requiredBoolean(item, 'minimumExclusive'),
        maximumExclusive: requiredBoolean(item, 'maximumExclusive'),
        choices,
        visibleWhen,
        ...(persistValues === undefined ? {} : { persistValues }),
      }
    })
    const runParameterDefaults = Object.prototype.hasOwnProperty.call(value, 'runParameterDefaults')
      ? runParameterMap(value.runParameterDefaults, 'runParameterDefaults')
      : {}
    for (const key of Object.keys(runParameterDefaults)) {
      if (!parameterIds.has(key)) throw new Error('unknown run parameter default')
    }
    return {
      configId: requiredString(value, 'configId'),
      productModel: requiredString(value, 'productModel'),
      productName: requiredString(value, 'productName'),
      configVersion: requiredString(value, 'configVersion'),
      stepId: requiredString(value, 'stepId'),
      testItemId: requiredString(value, 'testItemId'),
      algorithmId: requiredString(value, 'algorithmId'),
      title: requiredString(value, 'title'),
      description: requiredString(value, 'description'),
      stoppable: Object.prototype.hasOwnProperty.call(value, 'stoppable')
        ? requiredBoolean(value, 'stoppable')
        : true,
      supportedRunModes: modeValue as RunMode[],
      measurements,
      runParameterSchemaVersion: schemaVersion,
      runParameters,
      runParameterDefaults,
      postRunAnalysis: Object.prototype.hasOwnProperty.call(value, 'postRunAnalysis')
        ? parsePostRunAnalysisCapability(requiredObject(value, 'postRunAnalysis'))
        : { supported: false, analyzerId: '', schemaVersion: '' },
    }
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error)
    throw new Error(`Invalid protocol descriptor: ${detail}`)
  }
}

export function parseTestConfigCatalog(value: JsonObject): TestConfigCatalog {
  const configValue = value.configs
  if (!Array.isArray(configValue)) {
    throw new Error('Invalid protocol field: configs')
  }
  const configs: TestConfigOption[] = configValue.map((item) => {
    if (!isObject(item)) throw new Error('Invalid protocol field: config')
    return {
      configId: requiredString(item, 'configId'),
      title: requiredString(item, 'title'),
      description: requiredString(item, 'description'),
      algorithmId: requiredString(item, 'algorithmId'),
    }
  })
  return {
    selectedConfigId: requiredString(value, 'selectedConfigId'),
    configs,
  }
}

function emptyDigitalStimulus(): DigitalStimulusSnapshot {
  return { ...EMPTY_DIGITAL_STIMULUS, switches: [] }
}

function parseDigitalStimulus(value: JsonObject): DigitalStimulusSnapshot {
  try {
    const switchesValue = value.switches
    if (!Array.isArray(switchesValue) || switchesValue.length > 16) {
      throw new Error('switches')
    }

    const switchIds = new Set<string>()
    const dutBits = new Set<number>()
    const switches: DigitalSwitchDescriptor[] = switchesValue.map((item) => {
      if (!isObject(item)) throw new Error('switch')
      const switchId = requiredNonEmptyString(item, 'switchId')
      const dutBit = requiredSafeInteger(item, 'dutBit', 0, 15)
      const label = requiredNonEmptyString(item, 'label')
      const activeLevel = requiredDigitalLevel(item, 'activeLevel')
      if (switchIds.has(switchId) || dutBits.has(dutBit)) {
        throw new Error('duplicate switchId or dutBit')
      }
      switchIds.add(switchId)
      dutBits.add(dutBit)
      return { switchId, dutBit, label, activeLevel }
    })

    return {
      available: requiredBoolean(value, 'available'),
      configured: requiredBoolean(value, 'configured'),
      switches,
      appliedMask: requiredSafeInteger(value, 'appliedMask', 0, 0xFFFF),
      revision: requiredSafeInteger(value, 'revision'),
      lastWriteTimestampUs: requiredSafeInteger(value, 'lastWriteTimestampUs'),
      settlingMs: requiredSafeInteger(value, 'settlingMs', 0, 60_000),
      errorCode: requiredString(value, 'errorCode'),
      message: requiredString(value, 'message'),
    }
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error)
    throw new Error(`Invalid protocol digitalStimulus: ${detail}`)
  }
}

function parseReplyData(value: JsonObject): ReplyData {
  const data: ReplyData = {}
  for (const [key, item] of Object.entries(value)) {
    if (key !== 'digitalStimulus' && key !== 'analysisResult') data[key] = item
  }
  if (Object.prototype.hasOwnProperty.call(value, 'digitalStimulus')) {
    data.digitalStimulus = parseDigitalStimulus(requiredObject(value, 'digitalStimulus'))
  }
  if (Object.prototype.hasOwnProperty.call(value, 'analysisResult')) {
    data.analysisResult = parseAnalysisResult(requiredObject(value, 'analysisResult'))
  }
  return data
}

function parseTelemetryBatchCapability(
  value: JsonObject,
): NonNullable<HelloMessage['capabilities']>['telemetryBatch'] {
  if (requiredSafeInteger(value, 'version', 1, 1) !== 1) {
    throw new Error('Invalid protocol field: capabilities.telemetryBatch.version')
  }
  return {
    version: 1,
    maxSamples: requiredSafeInteger(value, 'maxSamples', 1, 64),
    maxBytes: requiredSafeInteger(value, 'maxBytes', 1),
    maxLatencyMs: requiredSafeInteger(value, 'maxLatencyMs'),
    snapshotIntervalMs: requiredSafeInteger(value, 'snapshotIntervalMs'),
  }
}

function parseHelloCapabilities(
  value: JsonObject,
): HelloMessage['capabilities'] | undefined {
  if (!Object.prototype.hasOwnProperty.call(value, 'capabilities')) return undefined
  const capabilities = requiredObject(value, 'capabilities')
  if (!Object.prototype.hasOwnProperty.call(capabilities, 'telemetryBatch')) return {}
  return {
    telemetryBatch: parseTelemetryBatchCapability(
      requiredObject(capabilities, 'telemetryBatch'),
    ),
  }
}

function parseSnapshot(value: JsonObject): ApplicationSnapshot {
  const descriptorValue = value.descriptor
  const digitalStimulus = Object.prototype.hasOwnProperty.call(value, 'digitalStimulus')
    ? parseDigitalStimulus(requiredObject(value, 'digitalStimulus'))
    : emptyDigitalStimulus()
  const dataSaveEnabled = Object.prototype.hasOwnProperty.call(value, 'dataSaveEnabled')
    ? requiredBoolean(value, 'dataSaveEnabled')
    : false
  const dataFilePath = Object.prototype.hasOwnProperty.call(value, 'dataFilePath')
    ? requiredString(value, 'dataFilePath')
    : ''
  const dataSaveError = Object.prototype.hasOwnProperty.call(value, 'dataSaveError')
    ? requiredString(value, 'dataSaveError')
    : ''
  const effectiveRunParameters = Object.prototype.hasOwnProperty.call(value, 'effectiveRunParameters')
    ? runParameterMap(value.effectiveRunParameters, 'effectiveRunParameters')
    : {}
  const analysis = Object.prototype.hasOwnProperty.call(value, 'analysis')
    ? parseAnalysisSnapshot(requiredObject(value, 'analysis'))
    : emptyAnalysis()
  return {
    ...(value as unknown as ApplicationSnapshot),
    dataSaveEnabled,
    dataFilePath,
    dataSaveError,
    effectiveRunParameters,
    descriptor: descriptorValue === undefined
      ? EMPTY_TEST_DESCRIPTOR
      : parseDescriptor(requiredObject(value, 'descriptor')),
    digitalStimulus,
    analysis,
  }
}

function parseSample(value: JsonObject): ApplicationSample {
  const streamElapsedUs = optionalSafeNonNegativeInteger(value, 'streamElapsedUs')
  return {
    taskId: requiredString(value, 'taskId'),
    stepId: requiredString(value, 'stepId'),
    channelId: requiredString(value, 'channelId'),
    timestampUs: requiredSafeInteger(value, 'timestampUs'),
    cycleIndex: requiredSafeInteger(value, 'cycleIndex'),
    values: requiredObject(value, 'values'),
    tags: requiredObject(value, 'tags'),
    ...(streamElapsedUs === undefined ? {} : { streamElapsedUs }),
  }
}

function parseSampleBatch(value: JsonObject): SampleBatchMessage {
  const firstSeq = requiredSafeInteger(value, 'firstSeq')
  const lastSeq = requiredSafeInteger(value, 'lastSeq')
  const sampleValues = requiredArray(value, 'samples')
  if (sampleValues.length < 1 || sampleValues.length > 64) {
    throw new Error('Invalid protocol sampleBatch: samples')
  }
  const expectedLast = firstSeq + sampleValues.length - 1
  if (!Number.isSafeInteger(expectedLast) || lastSeq !== expectedLast) {
    throw new Error('Invalid protocol sampleBatch: firstSeq/lastSeq')
  }
  const samples = sampleValues.map((item) => {
    if (!isObject(item)) throw new Error('Invalid protocol sampleBatch: samples')
    return parseSample(item)
  })
  const taskId = samples[0]?.taskId
  if (!taskId || samples.some((sample) => sample.taskId !== taskId)) {
    throw new Error('Invalid protocol sampleBatch: taskId')
  }
  return { v: 1, type: 'sampleBatch', firstSeq, lastSeq, samples }
}

export function supportsTelemetryBatch(hello: HelloMessage): boolean {
  return hello.capabilities?.telemetryBatch?.version === 1
}

export function parseServerMessage(text: string): ServerMessage {
  let parsed: unknown
  try {
    parsed = JSON.parse(text)
  } catch {
    throw new Error('Invalid protocol JSON')
  }
  if (!isObject(parsed) || parsed.v !== 1 || typeof parsed.type !== 'string') {
    throw new Error('Unsupported protocol envelope')
  }

  if (parsed.type === 'hello') {
    const capabilities = parseHelloCapabilities(parsed)
    return {
      v: 1,
      type: 'hello',
      server: requiredString(parsed, 'server'),
      protocolVersion: requiredNumber(parsed, 'protocolVersion'),
      ...(capabilities === undefined ? {} : { capabilities }),
    }
  }
  if (parsed.type === 'snapshot') {
    return {
      v: 1,
      type: 'snapshot',
      seq: requiredSafeInteger(parsed, 'seq'),
      snapshot: parseSnapshot(requiredObject(parsed, 'snapshot')),
    }
  }
  if (parsed.type === 'sample') {
    return {
      v: 1,
      type: 'sample',
      seq: requiredSafeInteger(parsed, 'seq'),
      sample: parseSample(requiredObject(parsed, 'sample')),
    }
  }
  if (parsed.type === 'sampleBatch') return parseSampleBatch(parsed)
  if (parsed.type === 'reply') {
    if (typeof parsed.ok !== 'boolean') {
      throw new Error('Invalid protocol field: ok')
    }
    return {
      v: 1,
      type: 'reply',
      id: requiredString(parsed, 'id'),
      ok: parsed.ok,
      code: requiredString(parsed, 'code'),
      message: requiredString(parsed, 'message'),
      data: parseReplyData(requiredObject(parsed, 'data')),
    }
  }
  throw new Error(`Unsupported protocol message type: ${parsed.type}`)
}

export function makeRequest(
  id: string,
  action: ActionName,
  params: JsonObject = {},
): string {
  return JSON.stringify({ v: 1, type: 'request', id, action, params })
}

export type ConnectionState =
  | 'disconnected'
  | 'connecting'
  | 'connected'
  | 'reconnecting'
  | 'error'

export type ClientEvent =
  | { type: 'connection'; state: ConnectionState; detail?: string }
  | { type: 'message'; message: Exclude<ServerMessage, { type: 'sample' } | { type: 'sampleBatch' }> }
  | {
    type: 'samples'
    firstSeq: number
    lastSeq: number
    samples: ApplicationSample[]
  }

type Listener = (event: ClientEvent) => void

function replyIdFromMalformedMessage(text: string): string | null {
  try {
    const parsed: unknown = JSON.parse(text)
    if (isObject(parsed) && parsed.v === 1 && parsed.type === 'reply' && typeof parsed.id === 'string') {
      return parsed.id
    }
  } catch {
    // The original parser reports malformed JSON through the connection event.
  }
  return null
}

export class HwtestClient {
  private socket: WebSocket | null = null
  private listeners = new Set<Listener>()
  private pending = new Map<string, {
    resolve: (reply: ReplyMessage) => void
    reject: (error: Error) => void
  }>()
  private requestSequence = 0

  constructor(private readonly url: string) {}

  subscribe(listener: Listener): () => void {
    this.listeners.add(listener)
    return () => this.listeners.delete(listener)
  }

  async connect(reconnecting = false): Promise<void> {
    if (this.socket?.readyState === WebSocket.OPEN) return
    this.emit({
      type: 'connection',
      state: reconnecting ? 'reconnecting' : 'connecting',
    })

    await new Promise<void>((resolve, reject) => {
      const socket = new WebSocket(this.url)
      this.socket = socket
      socket.addEventListener('open', () => {
        if (this.socket === socket) {
          this.emit({ type: 'connection', state: 'connected' })
        }
        resolve()
      }, { once: true })
      socket.addEventListener('message', (event) => {
        if (this.socket === socket) this.handleMessage(String(event.data))
      })
      socket.addEventListener('close', () => {
        if (this.socket !== socket) return
        this.socket = null
        this.rejectPending(new Error('WebSocket connection closed'))
        this.emit({ type: 'connection', state: 'disconnected' })
      })
      socket.addEventListener('error', () => {
        if (this.socket !== socket) return
        const error = new Error(`Cannot connect to ${this.url}`)
        this.emit({ type: 'connection', state: 'error', detail: error.message })
        reject(error)
      }, { once: true })
    })
  }

  request(action: ActionName, params: JsonObject = {}): Promise<ReplyMessage> {
    if (this.socket?.readyState !== WebSocket.OPEN) {
      return Promise.reject(new Error('WebSocket is not connected'))
    }
    const id = `${action}-${Date.now()}-${++this.requestSequence}`
    return new Promise<ReplyMessage>((resolve, reject) => {
      this.pending.set(id, { resolve, reject })
      this.socket?.send(makeRequest(id, action, params))
    })
  }

  close(): void {
    this.socket?.close(1000, 'client close')
  }

  private handleMessage(text: string): void {
    try {
      const message = parseServerMessage(text)
      if (message.type === 'reply') {
        const pending = this.pending.get(message.id)
        if (pending) {
          this.pending.delete(message.id)
          pending.resolve(message)
        }
      }
      if (message.type === 'sample') {
        this.emit({
          type: 'samples',
          firstSeq: message.seq,
          lastSeq: message.seq,
          samples: [message.sample],
        })
        return
      }
      if (message.type === 'sampleBatch') {
        this.emit({
          type: 'samples',
          firstSeq: message.firstSeq,
          lastSeq: message.lastSeq,
          samples: message.samples,
        })
        return
      }
      this.emit({ type: 'message', message })
    } catch (error) {
      const protocolError = error instanceof Error ? error : new Error(String(error))
      const replyId = replyIdFromMalformedMessage(text)
      if (replyId) this.rejectPendingReply(replyId, protocolError)
      this.emit({
        type: 'connection',
        state: 'error',
        detail: protocolError.message,
      })
    }
  }

  private rejectPendingReply(id: string, error: Error): void {
    const pending = this.pending.get(id)
    if (!pending) return
    this.pending.delete(id)
    pending.reject(error)
  }

  private rejectPending(error: Error): void {
    this.pending.forEach(({ reject }) => reject(error))
    this.pending.clear()
  }

  private emit(event: ClientEvent): void {
    this.listeners.forEach((listener) => listener(event))
  }
}
