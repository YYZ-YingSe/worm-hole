#!/usr/bin/env bash
set -euo pipefail

scripts/nightly/nightly_cmake_test_driver.sh label fuzz
