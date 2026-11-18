import { defineConfig } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';

export default defineConfig({
  plugins: [viteSingleFile()],
  build: {
    target: 'es2022',
    minify: 'esbuild',
    cssMinify: 'esbuild',
    outDir: 'dist',
    emptyOutDir: true
  }
});
