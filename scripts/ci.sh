#!/usr/bin/env bash
# ハードウェアなしで回る CI (要件 §12.3, §11)。TSan を常設 (§12.2)。
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build-ci -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSPEAR_WITH_UHD=OFF
ninja -C build-ci
ctest --test-dir build-ci --output-on-failure -j4   # core / dsp / apps / std_t98(実録音 golden は無ければ skip)
timeout 60 ./build-ci/tools/spear-headless --source synthetic --seconds 6 --record build-ci/ci_rec
timeout 60 ./build-ci/tools/spear-headless --source file:build-ci/ci_rec --seconds 3
cmake -S . -B build-ci-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSPEAR_TSAN=ON -DSPEAR_WITH_UHD=OFF
ninja -C build-ci-tsan
TSAN_OPTIONS="halt_on_error=1" ./build-ci-tsan/tests/spear_tests
TSAN_OPTIONS="halt_on_error=1" ./build-ci-tsan/apps/std_t98/tests/spear_std_t98_tests
echo "CI OK"
