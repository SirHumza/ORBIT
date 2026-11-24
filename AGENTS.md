# ORBIT contributor instructions

ORBIT is an OpenOrbis PS4 homebrew package browser and installer. Preserve FPKGi `DATA` JSON compatibility and the existing `/data/FPKGi` user data when changing internals.

Only ship package catalog entries when their authors or distributors authorize the package links. Do not add commercial game dumps, third-party scrape lists, accounts, tokens, or mandatory hosted services. Keep TLS verification, package identity/size checks, PKG magic validation, and failure-safe staging.

Before editing, inspect the relevant source, tests, and build workflow. Make one cohesive change, then run `make HOST_CXX=g++ host-tests`, catalog validation, and the OpenOrbis package build when the toolchain is available. Do not claim PS4 hardware compatibility without a recorded test on the stated firmware and model. Keep `PLAN.md` current, record upstream attribution and license obligations, and commit completed logical changes.
