import '@fontsource-variable/geist'
import { createRoot } from 'react-dom/client'

import { App } from './app/App'
import { ThemeProvider } from './app/ThemeProvider'
import { SessionProvider } from './features/session/SessionProvider'
import { applyTheme, readTheme } from './shared/theme'
import './index.css'

const root = document.getElementById('root')
if (!root) throw new Error('Missing #root element')

applyTheme(readTheme())

createRoot(root).render(
  <ThemeProvider>
    <SessionProvider>
      <App />
    </SessionProvider>
  </ThemeProvider>,
)
