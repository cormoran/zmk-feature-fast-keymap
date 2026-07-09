import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

// https://vite.dev/config/
export default defineConfig({
  base: process.env.VITE_BASE ?? "/zmk-fast-keymap/",
  plugins: [react()],
  resolve: {
    alias: {
      // Bundle the fast-keymap package straight from its TS source (not a
      // prebuilt `dist`) so `vite build`/`vite dev` never need the package
      // built first -- avoids a build-order dependency between the
      // workspace package and the app.
      "zmk-feature-fast-keymap": path.resolve(
        __dirname,
        "packages/fast-keymap/src/index.ts"
      ),
    },
  },
});
