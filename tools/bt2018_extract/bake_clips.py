"""Compatibility import; the native decoder lives with the BT2018 importer."""
if __name__ == '__main__':
    import runpy
    from pathlib import Path
    runpy.run_path(str(Path(__file__).resolve().parents[1] / 'bt2018_import' / 'bake_clips.py'), run_name='__main__')
else:
    from tools.bt2018_import.bake_clips import (  # noqa: F401
        UNITYPY_VERSION, RATE, WIDTH, streamed_keys, eval_streamed, sample_clip,
        path_names, decode_clip, native_clips, chassis_clips, add_to_glb,
    )
