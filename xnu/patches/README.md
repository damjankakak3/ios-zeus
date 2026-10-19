# zeus xnu patches

Applied in name order to `xnu-12377.121.6` by `.github/workflows/xnu.yml`, before
blacktop's own patches. Strict: a patch that does not apply fails the build.

Name them `NNNN-short-name.patch` (paths `a/...` `b/...` relative to the xnu root).
Ideas and code from ios-sweet's `xnu/patches` (xnu-8796, branch `main`) are
re-done here for xnu-12377; note the sweet patch number in each header.
