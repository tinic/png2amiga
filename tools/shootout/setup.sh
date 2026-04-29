#!/usr/bin/env bash
# Set up the third-party converters used by the HAM/SHAM shootout.
# Idempotent — safe to re-run; will skip any tool already in vendor/.
#
# Tools fetched / built:
#   - ham_convert (Solo761) — Java GUI/CLI, supports HAM6/SHAM6 + FS dither.
#       Distributed as a .zip with bundled .jar; no source.
#   - abc (arnaud-carre) — native C++ converter with Metal compute on macOS,
#       supports HAM6/SHAM6 + Floyd. Built from source via the project Makefile.
#
# Why no amigagfxmangle / DPaint.js / AGAConv: see tools/shootout/README.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENDOR="$SCRIPT_DIR/vendor"
mkdir -p "$VENDOR"

# --- ham_convert -----------------------------------------------------------
HAM_CONVERT_VER="1.10.4_beta_22-03-2025"
HAM_CONVERT_ZIP="ham_convert_${HAM_CONVERT_VER}.zip"
HAM_CONVERT_URL="http://mrsebe2.bplaced.net/ham_convert/${HAM_CONVERT_ZIP}"
HAM_CONVERT_DIR="$VENDOR/ham_convert"

if [ ! -f "$HAM_CONVERT_DIR/ham_convert.jar" ]; then
  echo "==> Setting up ham_convert..."
  rm -rf "$HAM_CONVERT_DIR"
  mkdir -p "$HAM_CONVERT_DIR"
  zip=""
  # Prefer a locally-staged copy in ~/Downloads (the upstream blog URLs
  # at mrsebe2.bplaced.net 404'd as of 2026-04, even for versions still
  # listed on the blog index page).
  for cand in \
      "$HOME/Downloads/ham_convert.zip" \
      "$HOME/Downloads/${HAM_CONVERT_ZIP}"; do
    if [ -f "$cand" ]; then
      zip="$cand"
      echo "    using local copy: $zip"
      break
    fi
  done
  if [ -z "$zip" ]; then
    echo "    fetching ${HAM_CONVERT_VER}..."
    if curl -fsSL -o "$HAM_CONVERT_DIR/$HAM_CONVERT_ZIP" "$HAM_CONVERT_URL"; then
      zip="$HAM_CONVERT_DIR/$HAM_CONVERT_ZIP"
    else
      echo "ERROR: ham_convert URL failed (and no copy in ~/Downloads/)." >&2
      echo "       Drop ham_convert.zip into ~/Downloads/ and re-run." >&2
      echo "       Blog: http://mrsebe.bplaced.net/blog/wordpress/?page_id=374" >&2
      exit 1
    fi
  fi
  cd "$HAM_CONVERT_DIR"
  unzip -q "$zip"
  # The zip nests under a versioned dir; flatten the .jar + lib/ to the top.
  jar=$(find . -maxdepth 3 -iname 'ham_convert*.jar' -not -ipath '*lib/*' | head -1)
  if [ -z "$jar" ]; then
    echo "ERROR: no ham_convert*.jar found in $zip" >&2
    exit 1
  fi
  cp "$jar" ham_convert.jar
  # The companion _lib/ directory must sit next to the .jar for the
  # MANIFEST classpath to resolve.
  libdir=$(find . -maxdepth 3 -type d -iname 'ham_convert*_lib' | head -1)
  if [ -n "$libdir" ]; then
    libname=$(basename "$libdir")
    if [ ! -d "$HAM_CONVERT_DIR/$libname" ]; then
      cp -R "$libdir" "$HAM_CONVERT_DIR/$libname"
    fi
  fi
  cd "$SCRIPT_DIR"
  echo "    ham_convert.jar -> $HAM_CONVERT_DIR/ham_convert.jar"
else
  echo "==> ham_convert already present, skipping"
fi

# --- abc (arnaud-carre) ----------------------------------------------------
ABC_DIR="$VENDOR/abc"
if [ ! -x "$ABC_DIR/abc2" ]; then
  echo "==> Cloning + building abc..."
  if [ ! -d "$ABC_DIR" ]; then
    git clone --depth 1 https://github.com/arnaud-carre/abc "$ABC_DIR"
  fi
  cd "$ABC_DIR"
  make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
  cd "$SCRIPT_DIR"
  echo "    abc2 -> $ABC_DIR/abc2"
else
  echo "==> abc2 already built, skipping"
fi

# --- ssimulacra2 (Cloudinary, perceptual quality metric) -------------------
# Built once from the upstream submodule at third_party/ssimulacra2.
# Used by run.sh as a *measurement* metric alongside sRGB-PSNR — the
# numbers correlate to subjective quality much better than PSNR. Build
# requires libhwy / lcms2 / libjpeg / libpng (brew/apt-installed) and
# the upstream pulls libjxl + highway directly from its src/lib tree.
S2_DIR="$VENDOR/ssimulacra2"
S2_BIN="$S2_DIR/ssimulacra2"
S2_SRC="$SCRIPT_DIR/../../third_party/ssimulacra2"
if [ ! -x "$S2_BIN" ]; then
  if [ ! -d "$S2_SRC" ] || [ ! -f "$S2_SRC/build_ssimulacra" ]; then
    echo "WARNING: third_party/ssimulacra2 submodule missing. " \
         "Run 'git submodule update --init' to fetch it." >&2
  else
    echo "==> Building ssimulacra2 (first-time, can take a few minutes)..."
    mkdir -p "$S2_DIR"
    # Build out-of-tree so the upstream source stays clean.
    if command -v ninja >/dev/null 2>&1; then
      cmake -S "$S2_SRC/src" -B "$S2_DIR/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release > "$S2_DIR/cmake.log" 2>&1
      cmake --build "$S2_DIR/build" --target ssimulacra2 -j \
        > "$S2_DIR/build.log" 2>&1 || {
          echo "ERROR: ssimulacra2 build failed. See $S2_DIR/build.log." >&2
          echo "    Common fix: brew install highway lcms2 jpeg-turbo libpng" >&2
          exit 1
        }
    else
      cmake -S "$S2_SRC/src" -B "$S2_DIR/build" \
        -DCMAKE_BUILD_TYPE=Release > "$S2_DIR/cmake.log" 2>&1
      cmake --build "$S2_DIR/build" --target ssimulacra2 -j \
        > "$S2_DIR/build.log" 2>&1 || {
          echo "ERROR: ssimulacra2 build failed. See $S2_DIR/build.log." >&2
          echo "    Common fix: brew install highway lcms2 jpeg-turbo libpng" >&2
          exit 1
        }
    fi
    cp "$S2_DIR/build/tools/ssimulacra2" "$S2_BIN" 2>/dev/null \
      || cp "$S2_DIR/build/ssimulacra2" "$S2_BIN" 2>/dev/null \
      || { echo "ERROR: built binary not found in $S2_DIR/build" >&2; exit 1; }
    echo "    ssimulacra2 -> $S2_BIN"
  fi
else
  echo "==> ssimulacra2 already built, skipping"
fi

# --- Sanity ----------------------------------------------------------------
JAVA="${JAVA:-/opt/homebrew/opt/openjdk/bin/java}"
if [ ! -x "$JAVA" ]; then
  JAVA=$(command -v java || true)
fi
if [ -z "$JAVA" ] || [ ! -x "$JAVA" ]; then
  echo "WARNING: java not found. ham_convert won't run." >&2
fi

echo
echo "Setup complete. Run ./run.sh to do the shootout."
