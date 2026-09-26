import os

import lit.formats

# Standalone lit config for the checker-error-recover feature tests, kept
# outside clang/test so they stay separate from the upstream tree. Run with:
#   build-ninja/bin/llvm-lit test/checker_recovery_test

config.name = "checker-recovery"
config.test_format = lit.formats.ShTest()
config.suffixes = [".cpp", ".c"]

here = os.path.dirname(os.path.abspath(__file__))
repo_root = os.path.dirname(os.path.dirname(here))

# Override with CUSTOM_ANALYSIS_BUILD_DIR=<build dir> to pick another build.
build_dir = os.environ.get("CUSTOM_ANALYSIS_BUILD_DIR")
if not build_dir:
    for candidate in ("build-ninja", "build-debug", "build-xcode", "build-vs-debug"):
        if os.path.exists(os.path.join(repo_root, candidate, "bin", "clang")):
            build_dir = os.path.join(repo_root, candidate)
            break

clang = os.path.join(build_dir, "bin", "clang")
config.environment["PATH"] = (
    os.path.join(build_dir, "bin") + os.pathsep + os.environ.get("PATH", "")
)

# Mirrors the upstream %clang_analyze_cc1 substitution
# (clang -cc1 -analyze -setup-static-analyzer, plus the builtin include setup).
resource_dir = (
    os.popen(clang + " -print-resource-dir").read().strip()
    if os.path.exists(clang)
    else ""
)
config.substitutions.append(
    (
        "%clang_analyze_cc1",
        clang
        + " -cc1 -internal-isystem "
        + resource_dir
        + " -nostdsysteminc -analyze -setup-static-analyzer",
    )
)
