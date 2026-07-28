import type {
  ActionName,
  ApplicationSample,
  ApplicationSnapshot,
  DigitalStimulusSnapshot,
  DigitalSwitchDescriptor,
  ReplyData,
  ReplyMessage,
  RunMode,
  ServerMessage,
  TestConfigCatalog,
  TestConfigOption,
  TestDescriptor,
  TestMeasurementDescriptor,
} from '../protocol'
import { EMPTY_DIGITAL_STIMULUS, EMPTY_TEST_DESCRIPTOR } from '../protocol'

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

function requiredDigitalLevel(parent: JsonObject, key: string): DigitalSwitchDescriptor['activeLevel'] {
  const value = requiredString(parent, key)
  if (value !== 'High' && value !== 'Low') {
    throw new Error(`Invalid protocol field: ${key}`)
  }
  return value
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
      supportedRunModes: modeValue as RunMode[],
      measurements,
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
    if (key !== 'digitalStimulus') data[key] = item
  }
  if (Object.prototype.hasOwnProperty.call(value, 'digitalStimulus')) {
    data.digitalStimulus = parseDigitalStimulus(requiredObject(value, 'digitalStimulus'))
  }
  return data
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
  return {
    ...(value as unknown as ApplicationSnapshot),
    dataSaveEnabled,
    dataFilePath,
    dataSaveError,
    descriptor: descriptorValue === undefined
      ? EMPTY_TEST_DESCRIPTOR
      : parseDescriptor(requiredObject(value, 'descriptor')),
    digitalStimulus,
  }
}

function parseSample(value: JsonObject): ApplicationSample {
  return {
    taskId: requiredString(value, 'taskId'),
    stepId: requiredString(value, 'stepId'),
    channelId: requiredString(value, 'channelId'),
    timestampUs: requiredNumber(value, 'timestampUs'),
    cycleIndex: requiredNumber(value, 'cycleIndex'),
    values: requiredObject(value, 'values'),
    tags: requiredObject(value, 'tags'),
  }
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
    return {
      v: 1,
      type: 'hello',
      server: requiredString(parsed, 'server'),
      protocolVersion: requiredNumber(parsed, 'protocolVersion'),
    }
  }
  if (parsed.type === 'snapshot') {
    return {
      v: 1,
      type: 'snapshot',
      seq: requiredNumber(parsed, 'seq'),
      snapshot: parseSnapshot(requiredObject(parsed, 'snapshot')),
    }
  }
  if (parsed.type === 'sample') {
    return {
      v: 1,
      type: 'sample',
      seq: requiredNumber(parsed, 'seq'),
      sample: parseSample(requiredObject(parsed, 'sample')),
    }
  }
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
  | { type: 'message'; message: ServerMessage }

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
        this.emit({ type: 'connection', state: 'connected' })
        resolve()
      }, { once: true })
      socket.addEventListener('message', (event) => this.handleMessage(String(event.data)))
      socket.addEventListener('close', () => {
        if (this.socket === socket) this.socket = null
        this.rejectPending(new Error('WebSocket connection closed'))
        this.emit({ type: 'connection', state: 'disconnected' })
      })
      socket.addEventListener('error', () => {
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
