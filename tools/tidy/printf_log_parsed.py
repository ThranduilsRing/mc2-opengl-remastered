"""printf_log_parsed - B6: printf_log over the tags the smoke harness parses; MC2_PRINTF_LOG_TAG=NAME limits a run to one tag."""
import printf_log as _pl

PATH_AWARE = True


def rewrite(text: str, path: str = "") -> str:
    return _pl.convert(text, path, classes=("consumed",))[0]
