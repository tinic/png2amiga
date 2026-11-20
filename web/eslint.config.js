import js from '@eslint/js'
import vue from 'eslint-plugin-vue'
import vueA11y from 'eslint-plugin-vuejs-accessibility'
import importX from 'eslint-plugin-import-x'
import security from 'eslint-plugin-security'
import promise from 'eslint-plugin-promise'
import unicorn from 'eslint-plugin-unicorn'
import sonarjs from 'eslint-plugin-sonarjs'
import tseslint from 'typescript-eslint'
import { createTypeScriptImportResolver } from 'eslint-import-resolver-typescript'
import globals from 'globals'

export default [
  {
    ignores: [
      'dist/**',
      '../service/**',
      '../build-wasm/**',
      'node_modules/**',
      'playwright-report/**',
      'test-results/**',
    ],
  },
  js.configs.recommended,
  ...tseslint.configs.strictTypeChecked,
  // Stylistic type-aware rules paired with strict — adds consistent-type-imports,
  // prefer-nullish-coalescing, prefer-optional-chain, prefer-readonly,
  // consistent-indexed-object-style, etc. Standard 2026 pairing.
  ...tseslint.configs.stylisticTypeChecked,
  ...vue.configs['flat/recommended'],
  // a11y rules for Vue templates: alt-less images, click-without-keyboard,
  // missing form labels, role/aria contradictions. Standard for any
  // production-facing Vue app.
  ...vueA11y.configs['flat/recommended'],
  importX.flatConfigs.recommended,
  security.configs.recommended,
  promise.configs['flat/recommended'],
  unicorn.configs['flat/recommended'],
  sonarjs.configs.recommended,
  {
    // The eslint config itself uses plugin default exports that also have
    // named exports; silence the import-x complaint about that pattern.
    files: ['eslint.config.js'],
    rules: {
      'import-x/no-named-as-default': 'off',
      'import-x/no-named-as-default-member': 'off',
    },
  },
  {
    files: ['**/*.{ts,mts,cts}'],
    languageOptions: {
      parser: tseslint.parser,
      parserOptions: {
        ecmaVersion: 'latest',
        sourceType: 'module',
        projectService: true,
        tsconfigRootDir: import.meta.dirname,
      },
    },
  },
  {
    files: ['**/*.vue'],
    languageOptions: {
      parserOptions: {
        parser: tseslint.parser,
        extraFileExtensions: ['.vue'],
        projectService: true,
        tsconfigRootDir: import.meta.dirname,
      },
    },
  },
  {
    // Type-checked rules require a tsconfig project; config files and the
    // standalone scripts/ helpers aren't part of the app's project.
    files: ['eslint.config.js', 'vite.config.ts', 'vitest.config.ts', 'playwright.config.ts', 'scripts/**/*.mjs'],
    ...tseslint.configs.disableTypeChecked,
  },
  {
    files: ['**/*.{js,mjs,cjs,ts,mts,cts,vue}'],
    languageOptions: {
      ecmaVersion: 'latest',
      sourceType: 'module',
      globals: {
        ...globals.browser,
        ...globals.worker,
        ...globals.node,
        __APP_VERSION__: 'readonly',
      },
    },
    settings: {
      'import-x/resolver-next': [
        // typescript-aware resolver: handles tsconfig paths, .ts extensions,
        // and the convention of importing `'foo.js'` to mean a `.ts` source.
        createTypeScriptImportResolver({
          alwaysTryTypes: true,
          project: './tsconfig.json',
        }),
      ],
    },
    rules: {
      // core
      'no-unused-vars': 'off',
      '@typescript-eslint/no-unused-vars': ['error', {
        args: 'after-used',
        argsIgnorePattern: '^_',
        varsIgnorePattern: '^_',
        caughtErrorsIgnorePattern: '^_',
      }],
      // TypeScript already verifies undefined references; the core rule
      // doesn't know about ambient DOM types, so it false-positives on
      // BlobPart, FileList, etc. inside `as` casts.
      'no-undef': 'off',
      'no-var': 'error',
      'prefer-const': 'error',
      'eqeqeq': ['error', 'always', { null: 'ignore' }],
      'no-implicit-coercion': 'error',
      'no-throw-literal': 'error',
      'no-return-await': 'error',
      'no-param-reassign': ['error', { props: false }],

      // Block escape hatches: `as any`, @ts-ignore, eval, new Function.
      // Each requires an explicit per-line eslint-disable to allow.
      'no-eval': 'error',
      'no-new-func': 'error',
      '@typescript-eslint/no-explicit-any': 'error',
      '@typescript-eslint/ban-ts-comment': ['error', {
        'ts-expect-error': 'allow-with-description',
        'ts-ignore': true,           // banned outright; use ts-expect-error
        'ts-nocheck': true,
        'ts-check': false,
      }],
      'no-restricted-syntax': ['error',
        {
          // `as any`
          selector: 'TSAsExpression > TSAnyKeyword',
          message: '`as any` is banned. Cast through `unknown` and explain why if absolutely necessary.',
        },
        {
          // `as unknown as X` — escape hatch that defeats the as-any ban.
          // Acceptable only at well-defined trust boundaries (worker
          // postMessage, third-party untyped APIs); use an explicit
          // // eslint-disable-next-line + rationale at those sites.
          selector: 'TSAsExpression > TSAsExpression > TSUnknownKeyword',
          message: 'Chained `as unknown as X` is banned. Add a runtime narrow (typeof / instanceof / "in") or define a proper type at the boundary.',
        },
        {
          // Non-null assertion (`x!`) — a quieter cousin of `as any`. Use
          // an explicit narrow (`if (!x) throw …`) so the asserter's
          // contract is checked at runtime.
          selector: 'TSNonNullExpression',
          message: 'Non-null assertion (`!`) is banned. Use an explicit narrow / runtime guard so the contract is checked.',
        },
      ],
      // Surface "known-broken, ship anyway" debt comments as warnings.
      'no-warning-comments': ['warn', {
        terms: ['fixme', 'xxx'],
        location: 'anywhere',
      }],
      // Goal: cyclomatic and cognitive complexity of 8. Refactor functions
      // that exceed it into smaller named helpers; do NOT raise the cap.
      'complexity': ['error', 8],
      'max-params': ['error', 5],
      'sonarjs/cognitive-complexity': ['error', 8],
      'no-console': ['warn', { allow: ['warn', 'error'] }],
      'curly': ['error', 'multi-line'],

      // vue
      'vue/multi-word-component-names': 'off',
      'vue/html-self-closing': 'off',
      'vue/max-attributes-per-line': 'off',
      'vue/singleline-html-element-content-newline': 'off',
      'vue/multiline-html-element-content-newline': 'off',
      'vue/html-indent': 'off',
      'vue/html-closing-bracket-newline': 'off',
      'vue/attributes-order': 'off',
      'vue/first-attribute-linebreak': 'off',
      'vue/attribute-hyphenation': 'off', // PrimeVue uses camelCase props
      // Lock down HTML injection: no v-html anywhere. Only XSS gadget left
      // is PrimeVue's `v-tooltip="{ escape: false }"` (used in Converter.vue
      // for the raw-format tooltip), which already coerces every
      // interpolation to a number — see the SAFETY comment by rawTooltipHtml.
      'vue/no-v-html': 'error',
      // Catch the "destructure a ref" footgun: `const { value } = myRef`
      // gives you a value, not reactivity, and silently breaks subsequent
      // updates.
      // (renamed from vue/no-ref-object-destructure, removed in plugin v10)
      'vue/no-ref-object-reactivity-loss': 'error',
      // Keep <script setup> macros in a consistent order so PRs don't churn
      // on stylistic ordering: defineOptions → defineProps → defineEmits →
      // defineSlots (the eslint-plugin-vue default).
      'vue/define-macros-order': 'error',
      // Vue 3.5+: use `useTemplateRef('foo')` instead of the legacy
      // `ref(null)` + matching template `ref="foo"`. The new form is
      // type-safe and detaches automatically.
      'vue/prefer-use-template-ref': 'error',
      // Vue 3.3+: declare component name / inheritAttrs / etc. via
      // `defineOptions({ name: 'X' })` inside <script setup> instead of
      // a separate non-setup <script> block. Removes a class of dual-
      // <script> footguns where the two blocks see different scopes.
      'vue/prefer-define-options': 'error',
      // Modern Vue 3: composition API only. The Options API still works
      // but composition + <script setup> is the standard going forward
      // and mixing the two in one codebase is a recipe for confusion.
      'vue/component-api-style': ['error', ['script-setup', 'composition']],
      // Type-based defineProps / defineEmits — the TS-friendly form
      // (`defineProps<{ foo: string }>()`) instead of the runtime
      // declaration (`defineProps({ foo: String })`). Plays well with
      // strictTypeChecked.
      'vue/define-props-declaration': ['error', 'type-based'],
      'vue/define-emits-declaration': ['error', 'type-based'],
      // Catch <SomeComponent /> in templates that doesn't resolve to a
      // registered component or import — without this, typos render
      // silently as a "missing custom element" warning at runtime.
      'vue/no-undef-components': ['error', {
        // PrimeVue auto-registers a few directives + Suspense / Teleport
        // are framework-builtins. Add to ignore as we discover more.
        ignorePatterns: [],
      }],
      // Flag template refs that are declared but never used.
      'vue/no-unused-refs': 'error',

      // a11y: PrimeVue components (Slider, Select, InputNumber, …) wrap
      // their own focusable form control internally and emit proper
      // aria-* associations, but the static template lint can't see
      // through the wrapper — these two rules false-positive on every
      // <label> + <PrimeComponent> pairing in our UI. Keep the
      // substantive interaction rules (click-events-have-key-events,
      // no-static-element-interactions, alt-text, etc.) on.
      'vuejs-accessibility/label-has-for': 'off',
      'vuejs-accessibility/form-control-has-label': 'off',

      // Block specific imports that look fine but bypass package entry
      // contracts (e.g. importing vue/dist/* skips the package's
      // declared exports field; primevue/*/dist/* same). Add new
      // patterns here when a reviewer spots a footgun import path.
      'no-restricted-imports': ['error', {
        patterns: [
          {
            group: ['vue/dist/*', 'primevue/*/dist/*', '@vue/runtime-*'],
            message: 'Import from the package entry (e.g. `vue`, `primevue/<component>`) — deep dist paths bypass exports/types and may break across upgrades.',
          },
          {
            group: ['../../node_modules/*', '../../../*'],
            message: 'Imports must stay inside src/ (or use the @wasm alias for the WASM build output). Going up 3+ directories implies a missing alias.',
          },
        ],
      }],

      // import-x — resolver doesn't grok @wasm alias / vite-built worker URLs
      'import-x/no-unresolved': ['error', {
        ignore: ['^@wasm/', String.raw`\?worker$`, String.raw`\?worker&`],
      }],
      'import-x/order': ['warn', {
        'newlines-between': 'always',
        groups: ['builtin', 'external', 'internal', 'parent', 'sibling', 'index'],
      }],

      // unicorn — relax the noisiest stylistic ones; keep the substantive checks
      'unicorn/prevent-abbreviations': 'off',
      // v72 split prevent-abbreviations into these two. Same call as above:
      // they want opts→options, el→element, ctx→context, buf→buffer and
      // isX/hasX prefixes on every boolean — 216 renames across the app for
      // no correctness gain.
      'unicorn/name-replacements': 'off',
      'unicorn/consistent-boolean-name': 'off',
      // Module-level mutable state is deliberate here: the debounce/spinner
      // timers, the worker's Module singleton, and Converter.vue's generation
      // counters all have to outlive the functions that set them.
      'unicorn/no-top-level-assignment-in-function': 'off',
      // `self` is the idiomatic global inside a Web Worker — it reads as
      // "this worker" where globalThis reads as "whatever scope this is".
      'unicorn/prefer-global-this': 'off',
      // Shape-of-the-code opinions, and the autofix for the first one emits
      // tab-indented semicolon lines into our 2-space semicolon-free files.
      'unicorn/prefer-early-return': 'off',
      'unicorn/prefer-minimal-ternary': 'off',
      'unicorn/prefer-simple-condition-first': 'off',
      'unicorn/no-declarations-before-early-exit': 'off',
      'unicorn/consistent-conditional-object-spread': 'off',
      // Every .sort() here is over ASCII identifiers or filenames, where the
      // default code-unit comparator is exactly what we want — the rule's
      // suggested localeCompare would *change* the ordering.
      'unicorn/require-array-sort-compare': 'off',
      // Canvas preview draws once per encode, not in an animation loop, so
      // caching a Path2D would trade clarity for nothing.
      'unicorn/prefer-path2d': 'off',
      'unicorn/filename-case': 'off',
      'unicorn/no-null': 'off',
      'unicorn/prefer-module': 'off',
      'unicorn/prefer-top-level-await': 'off',
      'unicorn/prefer-query-selector': 'off',
      'unicorn/numeric-separators-style': 'off',
      'unicorn/no-array-reduce': 'off',
      'unicorn/no-array-for-each': 'off',
      'unicorn/no-await-expression-member': 'off',
      'unicorn/explicit-length-check': 'off',
      'unicorn/consistent-function-scoping': 'off',
      'unicorn/prefer-export-from': 'off',
      'unicorn/no-immediate-mutation': 'off',  // we build arrays conditionally with push()
      'unicorn/prefer-single-call': 'off',     // we have multi-line conditional push patterns
      'unicorn/switch-case-braces': ['error', 'always'],

      // security — turn off the false-positive-prone object-injection check;
      // we use bracket access on small fixed-shape objects all over the place.
      'security/detect-object-injection': 'off',
      'security/detect-non-literal-regexp': 'off',
    },
  },
  {
    // Build / config / helper scripts. Last in the chain so this overrides
    // the global rule block above. They're short, stand-alone, and not
    // user-facing — relax the noisier rules.
    files: ['scripts/**/*.mjs'],
    rules: {
      'no-console': 'off',
      'security/detect-non-literal-fs-filename': 'off',
      'unicorn/no-array-sort': 'off',
      // Drift checks parse build inputs we control; ReDoS not a real risk.
      'sonarjs/slow-regex': 'off',
    },
  },
  {
    // Type-info-aware rules. Pinned explicitly (rather than relying on
    // recommendedTypeChecked alone) so a future preset bump can't silently
    // downgrade them. Restricted to .ts / .vue because they need the
    // tsconfig project; the scripts/*.mjs files have type checking
    // disabled via disableTypeChecked above.
    files: ['**/*.{ts,mts,cts,vue}'],
    // Config files live outside the app tsconfig project — exclude them
    // here too so type-checked rules don't try to fetch parser services.
    ignores: ['vite.config.ts', 'vitest.config.ts', 'playwright.config.ts'],
    rules: {
      // Every Promise must be awaited, .then'd, .catch'd, or explicitly
      // `void`'d. Catches the common "kicked off async work, didn't wait,
      // unhandled rejection later" footgun.
      '@typescript-eslint/no-floating-promises': 'error',
      // No Promise where a void return is expected (event handlers,
      // conditionals, setTimeout). Catches `setTimeout(async () => …)`
      // and `if (asyncFn())` mistakes.
      '@typescript-eslint/no-misused-promises': 'error',
      // strictTypeChecked turns this on with no implicit-conversion
      // allowances. This is graphics code: `${width}x${height}` and
      // similar number/bool template interpolations are everywhere and
      // safe (JS coerces consistently). Loosen to allow number/boolean
      // but keep the catch on object/array (where you'd see
      // [object Object] or "true,false,2"-style accidents).
      '@typescript-eslint/restrict-template-expressions': ['error', {
        allowNumber: true,
        allowBoolean: true,
      }],
    },
  },
]
