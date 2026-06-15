// ---------------------------------------------------------------------------
// Local admin panel server + Supabase proxy.
// ---------------------------------------------------------------------------

const SUPABASE_URL = process.env.SUPABASE_URL || ''
const SECRET_KEY   = process.env.SB_SECRET_KEY || ''

// Headers the browser adds that mark a request as coming from a browser.
// Supabase's secret-key block keys off these, so we drop them before forwarding.
const BROWSER_HEADERS = [
  'origin', 'referer',
  'sec-fetch-site', 'sec-fetch-mode', 'sec-fetch-dest', 'sec-fetch-user',
  'sec-ch-ua', 'sec-ch-ua-mobile', 'sec-ch-ua-platform',
]

// Plain object (not defineConfig) so `npx vite` works without a local install.
export default {
  server: {
    port: 5173,
    proxy: {
      '/rest': {
        target: SUPABASE_URL,
        changeOrigin: true,
        configure(proxy) {
          proxy.on('proxyReq', (proxyReq) => {
            // Env key wins; otherwise relay the apikey the browser sent.
            if (SECRET_KEY) proxyReq.setHeader('apikey', SECRET_KEY)
            // Make this look like a server-side call.
            for (const h of BROWSER_HEADERS) proxyReq.removeHeader(h)
            proxyReq.setHeader('user-agent', 'opendojo-admin-proxy')
          })
        },
      },
    },
  },
}
