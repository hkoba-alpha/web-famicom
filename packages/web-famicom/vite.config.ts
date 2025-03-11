import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { viteStaticCopy } from 'vite-plugin-static-copy'

// https://vite.dev/config/
export default defineConfig({
  base: '/public/web-famicom/',
  plugins: [react(),
    viteStaticCopy({
      targets: [
        { src: 'node_modules/nes-emu/dist/assets/*.js', dest: 'assets' },
      ]
    }),
  ],
})
