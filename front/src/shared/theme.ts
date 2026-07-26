export type Theme = 'dark' | 'light'

export const THEME_STORAGE_KEY = 'hwtest.theme.v1'

export interface ChartPalette {
  readonly axis: string
  readonly grid: string
  readonly ticks: string
  readonly series: readonly string[]
}

const CHART_PALETTES: Record<Theme, ChartPalette> = {
  dark: {
    axis: '#77827f',
    grid: '#1a2425',
    ticks: '#2a3637',
    series: [
      '#b6f34a',
      '#45d6d0',
      '#f5b84b',
      '#d18cff',
      '#ff6b6b',
      '#7ca8ff',
      '#f0f3e8',
    ],
  },
  light: {
    axis: '#66766d',
    grid: '#dce6e0',
    ticks: '#becbc3',
    series: [
      '#3f7d14',
      '#007875',
      '#a56000',
      '#77549d',
      '#b53c45',
      '#335fa8',
      '#30443a',
    ],
  },
}

export function normalizeTheme(value: unknown): Theme {
  return value === 'light' ? 'light' : 'dark'
}

export function readTheme(): Theme {
  try {
    return normalizeTheme(window.localStorage.getItem(THEME_STORAGE_KEY))
  } catch {
    return 'dark'
  }
}

export function persistTheme(theme: Theme) {
  try {
    window.localStorage.setItem(THEME_STORAGE_KEY, theme)
  } catch {
    // Storage can be unavailable in privacy-restricted browser contexts.
  }
}

export function applyTheme(theme: Theme) {
  document.documentElement.dataset.theme = theme
}

export function getChartPalette(theme: Theme): ChartPalette {
  return CHART_PALETTES[theme]
}
