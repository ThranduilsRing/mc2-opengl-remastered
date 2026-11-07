#!/usr/bin/env sh
# GL census: GL is gone from the engine. Fail if a gl*() call or a GL header include appears in our C/C++ (3rdparty excluded).
# Comment text ('//' tails, block-comment lines) is ignored: history comments may still name GL calls.
set -euf
SRC="-- *.cpp *.h *.hpp *.c *.inc :!3rdparty"
INC='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](GL/|GLES|glad|glew|SDL_opengl|SDL2/SDL_opengl|imgui_impl_opengl)'
CALL='\bgl[A-Z][A-Za-z0-9]*[[:space:]]*\('
FILES=$(git ls-files $SRC | wc -l)
[ "$FILES" -ge 500 ] || { echo "gl-census: corpus floor: only $FILES C/C++ files tracked"; exit 1; }
HITS=$(git grep -nE "$INC" $SRC || true)
CODE=$(git grep -nE "$CALL" $SRC | sed -E 's/"([^"\\]|\\.)*"//g; s#(^[^:]+:[0-9]+:[^/]*)//.*#\1#' \
       | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(/\*|\*)' | grep -E "$CALL" || true)
HITS="$HITS$CODE"
[ -z "$HITS" ] || { echo "gl-census: GL call/include in owned code:"; echo "$HITS" | head -20; exit 1; }
echo "gl-census: OK ($FILES files, 0 gl*() calls, 0 GL includes)"
