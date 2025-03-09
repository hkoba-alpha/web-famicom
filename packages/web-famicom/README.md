# WebAssemblyを使ったファミコンエミュレータ

pnpmをインストールしてください。

## エミュレータ本体

emscriptenをインストールする必要があります。

https://github.com/emscripten-core/emsdk

以下のパスで環境が使えるようにしてください。

```
~/emsdk/emsdk_env.sh
```

### ビルド方法

nes-emuのビルド

```
cd packages/nes-emu
pnpm run build
```

web-famicomのビルド

```
cd packages/web-famicom
pnpm run build
```