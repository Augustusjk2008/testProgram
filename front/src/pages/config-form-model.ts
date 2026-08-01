import type { ConfigValue } from './config-draft'

function pathTokens(path: string): string[] {
  if (path === '') return []
  if (!path.startsWith('/')) throw new Error(`配置字段路径必须是 JSON Pointer：${path}`)
  return path.slice(1).split('/').map((token) =>
    token.replaceAll('~1', '/').replaceAll('~0', '~'))
}

function arrayIndex(token: string, length: number): number {
  if (!/^\d+$/.test(token)) throw new Error(`配置数组索引无效：${token}`)
  const index = Number(token)
  if (!Number.isSafeInteger(index) || index < 0 || index >= length) {
    throw new Error(`配置数组索引越界：${token}`)
  }
  return index
}

function replaceAtPath(current: unknown, tokens: string[], offset: number, nextValue: unknown): unknown {
  if (offset === tokens.length) return nextValue
  const token = tokens[offset]!
  if (Array.isArray(current)) {
    const index = arrayIndex(token, current.length)
    const next = [...current]
    next[index] = replaceAtPath(current[index], tokens, offset + 1, nextValue)
    return next
  }
  if (typeof current === 'object' && current !== null) {
    const record = current as Record<string, unknown>
    const exists = Object.prototype.hasOwnProperty.call(record, token)
    const nextToken = tokens[offset + 1]
    if (!exists && nextToken !== undefined && /^\d+$/.test(nextToken)) {
      throw new Error(`不能为缺失字段推断配置数组：${token}`)
    }
    return {
      ...record,
      [token]: replaceAtPath(exists ? record[token] : {}, tokens, offset + 1, nextValue),
    }
  }
  throw new Error(`配置字段路径不能穿过标量值：${token}`)
}

export function configValueAtPath(value: unknown, path: string): unknown {
  return pathTokens(path).reduce<unknown>((current, token) => {
    if (Array.isArray(current)) return current[arrayIndex(token, current.length)]
    if (typeof current === 'object' && current !== null) {
      return (current as Record<string, unknown>)[token]
    }
    return undefined
  }, value)
}

export function updateConfigValueAtPath(value: ConfigValue, path: string, nextValue: unknown): ConfigValue {
  const updated = replaceAtPath(value, pathTokens(path), 0, nextValue)
  if (typeof updated !== 'object' || updated === null || Array.isArray(updated)) {
    throw new Error('配置文档根节点必须保持为对象')
  }
  return updated as ConfigValue
}

export function appendConfigArrayItem(
  value: ConfigValue,
  path: string,
  item: unknown,
): ConfigValue {
  const current = configValueAtPath(value, path)
  if (current === undefined) return updateConfigValueAtPath(value, path, [item])
  if (!Array.isArray(current)) throw new Error(`配置字段不是列表：${path}`)
  return updateConfigValueAtPath(value, path, [...current, item])
}

export function removeConfigArrayItem(
  value: ConfigValue,
  path: string,
  index: number,
): ConfigValue {
  const current = configValueAtPath(value, path)
  if (!Array.isArray(current)) throw new Error(`配置字段不是列表：${path}`)
  arrayIndex(String(index), current.length)
  return updateConfigValueAtPath(value, path, current.filter((_, itemIndex) => itemIndex !== index))
}
