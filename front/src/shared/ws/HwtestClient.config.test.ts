import { afterEach, describe, expect, it, vi } from 'vitest'

import { HwtestClient, parseConfigCatalog } from './HwtestClient'

class FakeWebSocket {
  static readonly OPEN = 1
  static instances: FakeWebSocket[] = []

  readyState = 0
  readonly sent: string[] = []
  private readonly listeners = new Map<string, Array<(event: { data?: unknown }) => void>>()

  constructor(readonly url: string) {
    FakeWebSocket.instances.push(this)
  }

  addEventListener(
    type: string,
    listener: (event: { data?: unknown }) => void,
    _options?: unknown,
  ): void {
    const listeners = this.listeners.get(type) ?? []
    listeners.push(listener)
    this.listeners.set(type, listeners)
  }

  send(text: string): void {
    this.sent.push(text)
  }

  close(): void {
    this.readyState = 3
    this.emit('close')
  }

  emit(type: string, event: { data?: unknown } = {}): void {
    this.listeners.get(type)?.forEach((listener) => listener(event))
  }
}

afterEach(() => {
  FakeWebSocket.instances = []
  vi.unstubAllGlobals()
})

describe('HwtestClient configuration API', () => {
  it('rejects numeric configuration revisions because saves require SHA-256 strings', () => {
    expect(() => parseConfigCatalog({ revision: 42, items: [] }))
      .toThrow('Invalid protocol field: revision')
  })

  it('requests the configuration catalog and returns its typed entries', async () => {
    vi.stubGlobal('WebSocket', FakeWebSocket)
    const client = new HwtestClient('ws://127.0.0.1:18765/ws')
    const connected = client.connect()
    const socket = FakeWebSocket.instances[0]!
    socket.readyState = FakeWebSocket.OPEN
    socket.emit('open')
    await connected

    const getConfigCatalog = (client as unknown as {
      getConfigCatalog?: () => Promise<unknown>
    }).getConfigCatalog
    expect(getConfigCatalog).toBeTypeOf('function')
    if (!getConfigCatalog) return

    const pending = getConfigCatalog.call(client)
    const request = JSON.parse(socket.sent[0]!)
    expect(request).toMatchObject({
      v: 1,
      type: 'request',
      action: 'configCatalog',
      params: {},
    })

    socket.emit('message', {
      data: JSON.stringify({
        v: 1,
        type: 'reply',
        id: request.id,
        ok: true,
        code: '',
        message: '',
        data: {
          revision: 'catalog-r7',
          items: [
            {
              documentId: 'timer-jitter.testcfg.json',
              configId: 'mbddf.timer_jitter',
              title: '定时器抖动',
              enabled: true,
              order: 4,
              valid: true,
              message: '',
            },
          ],
        },
      }),
    })

    await expect(pending).resolves.toEqual({
      revision: 'catalog-r7',
      items: [
        {
          documentId: 'timer-jitter.testcfg.json',
          configId: 'mbddf.timer_jitter',
          title: '定时器抖动',
          enabled: true,
          order: 4,
          valid: true,
          message: '',
        },
      ],
    })
  })

  it('loads a document and saves its edited value with the expected revision', async () => {
    vi.stubGlobal('WebSocket', FakeWebSocket)
    const client = new HwtestClient('ws://127.0.0.1:18765/ws')
    const connected = client.connect()
    const socket = FakeWebSocket.instances[0]!
    socket.readyState = FakeWebSocket.OPEN
    socket.emit('open')
    await connected

    const typedClient = client as unknown as {
      getConfigDocument?: (documentId: string) => Promise<unknown>
      saveConfig?: (request: {
        documentId: string
        expectedRevision: string
        value: Record<string, unknown>
      }) => Promise<unknown>
    }
    expect(typedClient.getConfigDocument).toBeTypeOf('function')
    expect(typedClient.saveConfig).toBeTypeOf('function')
    if (!typedClient.getConfigDocument || !typedClient.saveConfig) return

    const documentPending = typedClient.getConfigDocument('timer-jitter.testcfg.json')
    const documentRequest = JSON.parse(socket.sent[0]!)
    expect(documentRequest).toMatchObject({
      v: 1,
      type: 'request',
      action: 'configDocument',
      params: { documentId: 'timer-jitter.testcfg.json' },
    })
    socket.emit('message', {
      data: JSON.stringify({
        v: 1,
        type: 'reply',
        id: documentRequest.id,
        ok: true,
        code: '',
        message: '',
        data: {
          documentId: 'timer-jitter.testcfg.json',
          kind: 'testcfg',
          revision: 'r11',
          value: {
            reportFields: { title: '定时器抖动' },
            steps: [{ parameters: { sample_count: 250 } }],
            executionConfig: { periodUs: 250 },
          },
          schema: { title: { type: 'string' } },
        },
      }),
    })

    await expect(documentPending).resolves.toMatchObject({
      documentId: 'timer-jitter.testcfg.json',
      kind: 'testcfg',
      revision: 'r11',
      value: { steps: [{ parameters: { sample_count: 250 } }] },
    })

    const savePending = typedClient.saveConfig({
      documentId: 'timer-jitter.testcfg.json',
      expectedRevision: 'r11',
      value: {
        reportFields: { title: '更新后的抖动测试' },
        steps: [{ parameters: { sample_count: 500 } }],
        executionConfig: { periodUs: 250 },
      },
    })
    const saveRequest = JSON.parse(socket.sent[1]!)
    expect(saveRequest).toMatchObject({
      v: 1,
      type: 'request',
      action: 'saveConfig',
      params: {
        documentId: 'timer-jitter.testcfg.json',
        expectedRevision: 'r11',
        value: {
          reportFields: { title: '更新后的抖动测试' },
          steps: [{ parameters: { sample_count: 500 } }],
          executionConfig: { periodUs: 250 },
        },
      },
    })
    socket.emit('message', {
      data: JSON.stringify({
        v: 1,
        type: 'reply',
        id: saveRequest.id,
        ok: true,
        code: '',
        message: '',
        data: {
          documentId: 'timer-jitter.testcfg.json',
          kind: 'testcfg',
          revision: 'r12',
          value: {
            reportFields: { title: '更新后的抖动测试' },
            steps: [{ parameters: { sample_count: 500 } }],
            executionConfig: { periodUs: 250 },
          },
        },
      }),
    })

    await expect(savePending).resolves.toMatchObject({
      documentId: 'timer-jitter.testcfg.json',
      revision: 'r12',
      value: { reportFields: { title: '更新后的抖动测试' } },
    })
  })

  it('preserves save conflict code and message for the configuration page', async () => {
    vi.stubGlobal('WebSocket', FakeWebSocket)
    const client = new HwtestClient('ws://127.0.0.1:18765/ws')
    const connected = client.connect()
    const socket = FakeWebSocket.instances[0]!
    socket.readyState = FakeWebSocket.OPEN
    socket.emit('open')
    await connected

    const saveConfig = (client as unknown as {
      saveConfig?: (request: {
        documentId: string
        expectedRevision: string
        value: Record<string, unknown>
      }) => Promise<unknown>
    }).saveConfig
    expect(saveConfig).toBeTypeOf('function')
    if (!saveConfig) return

    const pending = saveConfig.call(client, {
      documentId: 'mbddf-station',
      expectedRevision: 'r8',
      value: { serial: { baudRate: 115200 } },
    })
    const request = JSON.parse(socket.sent[0]!)
    socket.emit('message', {
      data: JSON.stringify({
        v: 1,
        type: 'reply',
        id: request.id,
        ok: false,
        code: 'config_conflict',
        message: '配置已被其他用户更新',
        data: {},
      }),
    })

    await expect(pending).rejects.toMatchObject({
      code: 'config_conflict',
      message: '配置已被其他用户更新',
    })
  })
})
