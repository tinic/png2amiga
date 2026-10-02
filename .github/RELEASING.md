# Releases

1. Bump the version in `CMakeLists.txt` and commit the release's source changes.
2. Run `./tools/build-web.sh`. This builds both WASM variants, runs the web
   validation pipeline, generates gzip/Brotli assets, and records the build manifest.
3. Run `cd web && npm run test:e2e` against the resulting production bundle.
4. Commit and push `service/html/`, including `build-manifest.json`.
5. Run `python3 tools/web-assets-manifest.py check`, then tag and push the release.

The release workflow rejects missing, stale, or modified web assets before
configuring the native build. Any encoder, web source, build recipe, version, or
dependency change requires another web build and asset commit before tagging.
