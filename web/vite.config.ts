import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import { fileURLToPath, URL } from 'node:url'
import fs from 'fs'

// SSL certificate paths (in web/certs directory)
const SSL_KEY_PATH = './certs/key.pem'
const SSL_CERT_PATH = './certs/certificate.pem'

// Check if SSL certificates exist
let httpsConfig = false
try {
  if (fs.existsSync(SSL_KEY_PATH) && fs.existsSync(SSL_CERT_PATH)) {
    httpsConfig = {
      key: fs.readFileSync(SSL_KEY_PATH),
      cert: fs.readFileSync(SSL_CERT_PATH)
    }
    console.log('✓ SSL certificates found, HTTPS enabled')
  } else {
    console.log('⚠ SSL certificates not found, using HTTP')
  }
} catch (e) {
  console.log('⚠ Could not read SSL certificates, using HTTP')
}

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [vue()],

  resolve: {
    alias: {
      '@': fileURLToPath(new URL('./src', import.meta.url))
    }
  },

  server: {
    port: 3000,
    https: httpsConfig,
    host: 'localhost',

    // CORS headers for File System API
    headers: {
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cross-Origin-Opener-Policy': 'same-origin'
    }
  },

  // Ensure WASM files are served with correct MIME type
  assetsInclude: ['**/*.wasm']
})