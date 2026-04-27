import js from '@eslint/js'
import vue from 'eslint-plugin-vue'
import importX from 'eslint-plugin-import-x'
import security from 'eslint-plugin-security'
import promise from 'eslint-plugin-promise'
import unicorn from 'eslint-plugin-unicorn'
import sonarjs from 'eslint-plugin-sonarjs'
import globals from 'globals'

export default [
  {
    ignores: [
      'dist/**',
      '../service/**',
      '../build-wasm/**',
      'node_modules/**',
    ],
  },
  js.configs.recommended,
  ...vue.configs['flat/recommended'],
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
    files: ['**/*.{js,mjs,cjs,vue}'],
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
      'import-x/resolver': {
        node: { extensions: ['.js', '.mjs', '.vue'] },
      },
    },
    rules: {
      // core
      'no-unused-vars': ['error', {
        args: 'after-used',
        argsIgnorePattern: '^_',
        varsIgnorePattern: '^_',
        caughtErrorsIgnorePattern: '^_',
      }],
      'no-undef': 'error',
      'no-var': 'error',
      'prefer-const': 'error',
      'eqeqeq': ['error', 'always', { null: 'ignore' }],
      'no-implicit-coercion': 'error',
      'no-throw-literal': 'error',
      'no-return-await': 'error',
      'no-param-reassign': ['error', { props: false }],
      // Existing baseline for Converter.vue's larger handlers — tighten over time.
      'complexity': ['error', 30],
      'max-params': ['error', 5],
      'sonarjs/cognitive-complexity': ['error', 30],
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
]
