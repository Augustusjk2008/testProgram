import type {
  ActionName,
  ApplicationSample,
  ApplicationSnapshot,
  ReplyMessage,
  ServerMessage,
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
      snapshot: requiredObject(parsed, 'snapshot') as unknown as ApplicationSnapshot,
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
      data: requiredObject(parsed, 'data'),
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
      this.emit({
        type: 'connection',
        state: 'error',
        detail: error instanceof Error ? error.message : String(error),
      })
    }
  }

  private rejectPending(error: Error): void {
    this.pending.forEach(({ reject }) => reject(error))
    this.pending.clear()
  }

  private emit(event: ClientEvent): void {
    this.listeners.forEach((listener) => listener(event))
  }
}
