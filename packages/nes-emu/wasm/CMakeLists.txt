cmake_minimum_required(VERSION 3.13)

# プロジェクト設定
project(NesEmu)

# Emscripten のツールチェーンを使用
set(CMAKE_CXX_COMPILER "em++")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 出力ファイル設定
set(CMAKE_EXECUTABLE_SUFFIX ".js")
set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../src/wasm")

# 共通の最適化フラグ (-O3 を追加)
set(OPTIMIZATION_FLAGS "-O3")

# 共通のリンクフラグ
set(COMMON_LINK_FLAGS "--no-entry -s ALLOW_TABLE_GROWTH=1 -s WASM=1 -s MODULARIZE=1 -sSINGLE_FILE=1 -s EXPORT_ES6=1 -s ENVIRONMENT=web -s EXPORTED_RUNTIME_METHODS=['addFunction','removeFunction']")

# 共通のコンパイルオプション
set(COMMON_COMPILE_OPTIONS "-sUSE_ES6_IMPORT_META=0")

# 関数でビルド設定をまとめる
function(add_embind_target target_name)
    add_executable(${target_name} ${target_name}.cpp)
    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${OUTPUT_DIR}
        LINK_FLAGS "${COMMON_LINK_FLAGS}"
    )
    target_compile_options(${target_name} PRIVATE "${OPTIMIZATION_FLAGS}" "${COMMON_COMPILE_OPTIONS}")
endfunction()

# ターゲット追加
add_embind_target(cpu)
add_embind_target(ppu)
add_embind_target(apu)
