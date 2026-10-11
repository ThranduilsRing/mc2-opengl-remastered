"""printf_log_tagged - B5: printf_log over the `[TAG vN]` lines whose tag no Python parser or checker mentions."""
import printf_log as _pl

PATH_AWARE = True


def rewrite(text: str, path: str = "") -> str:
    return _pl.convert(text, path, classes=("unconsumed",))[0]
