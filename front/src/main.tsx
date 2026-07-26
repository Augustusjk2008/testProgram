import '@fontsource-variable/geist'
import { createRoot } from 'react-dom/client'

import { App } from './app/App'
import { SessionProvider } from './features/session/SessionProvider'
import './index.css'

const root = document.getElementById('root')
if (!root) throw new Error('Missing #root element')

createRoot(root).render(
  <SessionProvider>
    <App />
  </SessionProvider>,
)
