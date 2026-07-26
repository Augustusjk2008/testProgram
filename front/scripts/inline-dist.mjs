import { readFile, rm, writeFile } from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const scriptDirectory = path.dirname(fileURLToPath(import.meta.url))
const distDirectory = path.resolve(scriptDirectory, '..', 'dist')
const indexPath = path.join(distDirectory, 'index.html')
const assetsDirectory = path.join(distDirectory, 'assets')

const MIME_TYPES = {
  '.css': 'text/css',
  '.js': 'text/javascript',
  '.woff2': 'font/woff2',
}

function assetPath(reference) {
  const relative = decodeURIComponent(reference).replace(/^\/+/, '')
  if (!relative.startsWith('assets/')) {
    throw new Error(`Unexpected build asset reference: ${reference}`)
  }
  const resolved = path.resolve(distDirectory, relative)
  if (!resolved.startsWith(`${distDirectory}${path.sep}`)) {
    throw new Error(`Build asset escapes dist directory: ${reference}`)
  }
  return resolved
}

function mimeType(filePath) {
  return MIME_TYPES[path.extname(filePath).toLowerCase()] ?? 'application/octet-stream'
}

async function readAsset(reference) {
  const filePath = assetPath(reference)
  return {
    bytes: await readFile(filePath),
    type: mimeType(filePath),
  }
}

async function replaceAsync(source, pattern, replacer) {
  const matches = [...source.matchAll(pattern)]
  if (matches.length === 0) return source

  let output = ''
  let cursor = 0
  for (const match of matches) {
    output += source.slice(cursor, match.index)
    output += await replacer(match)
    cursor = match.index + match[0].length
  }
  return output + source.slice(cursor)
}

async function inlineCss(css) {
  return replaceAsync(
    css,
    /url\(\s*(['"]?)(\/?assets\/[^'"\)]+)\1\s*\)/g,
    async (match) => {
      const { bytes, type } = await readAsset(match[2])
      return `url(data:${type};base64,${bytes.toString('base64')})`
    },
  )
}

let html = await readFile(indexPath, 'utf8')

html = html.replace(/<link\b[^>]*rel=["']modulepreload["'][^>]*>/gi, '')
html = await replaceAsync(
  html,
  /<link\b[^>]*rel=["']stylesheet["'][^>]*>/gi,
  async (match) => {
    const href = match[0].match(/\bhref=["']([^"']+)["']/i)?.[1]
    if (!href) throw new Error(`Stylesheet link has no href: ${match[0]}`)
    const { bytes } = await readAsset(href)
    const css = await inlineCss(bytes.toString('utf8'))
    return `<style>${css.replace(/<\/style/gi, '<\\/style')}</style>`
  },
)
html = await replaceAsync(
  html,
  /<script\b[^>]*\bsrc=["']([^"']+)["'][^>]*><\/script>/gi,
  async (match) => {
    const { bytes } = await readAsset(match[1])
    const type = match[0].match(/\btype=["']([^"']+)["']/i)?.[1]
    const typeAttribute = type ? ` type="${type}"` : ''
    return `<script${typeAttribute}>${bytes.toString('utf8').replace(/<\/script/gi, '<\\/script')}</script>`
  },
)

if (/<(?:script|link)\b[^>]+(?:src|href)=/i.test(html) || /url\(\s*\/?assets\//i.test(html)) {
  throw new Error('Single-file build still contains external asset references')
}

await writeFile(indexPath, html, 'utf8')
await rm(assetsDirectory, { recursive: true, force: true })
console.log(`Wrote standalone frontend: ${indexPath}`)
