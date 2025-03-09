export class FamPPU {
    // コールバック関数の参照を保持するための変数
    private vblankCallback = 0;
    private hblankCallback = 0;
    private cpuCallback = 0;

    // シングルトンインスタンスを保持するための変数
    private static instance: FamPPU;

    // コンストラクタは外部から呼び出せないようにする
    private constructor(private readonly module: any) {
    }

    // PPUのインスタンスを取得するためのメソッド
    public static async getPPU(): Promise<FamPPU> {
        if (!this.instance) {
            const wasmModule = await import('./wasm/ppu.js');
            const module = await wasmModule.default();
            this.instance = new FamPPU(module);
        }
        return this.instance;
    }

    // VBlankコールバックを設定するメソッド
    public setVblankCallback(callback?: () => void) {
        if (this.vblankCallback) {
            this.module.removeFunction(this.vblankCallback);
        }
        if (callback) {
            this.vblankCallback = this.module.addFunction(callback, 'v');
            this.module._setVblankCallback(this.vblankCallback);
        } else {
            this.vblankCallback = 0;
            this.module._setVblankCallback(0);
        }
    }

    // HBlankコールバックを設定するメソッド
    public setHblankCallback(callback?: (y: number) => void) {
        if (this.hblankCallback) {
            this.module.removeFunction(this.hblankCallback);
        }
        if (callback) {
            this.hblankCallback = this.module.addFunction(callback, 'vi');
            this.module._setHblankCallback(this.hblankCallback);
        } else {
            this.hblankCallback = 0;
            this.module._setHblankCallback(0);
        }
    }

    // CPUコールバックを設定するメソッド
    public setCpuCallback(callback?: (cycle: number) => void) {
        if (this.cpuCallback) {
            this.module.removeFunction(this.cpuCallback);
        }
        if (callback) {
            this.cpuCallback = this.module.addFunction(callback, 'vi');
            this.module._setCpuCallback(this.cpuCallback);
        } else {
            this.cpuCallback = 0;
            this.module._setCpuCallback(0);
        }
    }

    /**
     * 画面をレンダリングする
     * @param clip 上下8ドットずつをクリップするかどうか
     * @returns レンダリングされた画面のピクセルデータ
     */
    public renderScreen(clip = false): Uint8ClampedArray {
        const ret = this.module._renderScreen();
        if (clip) {
            return new Uint8ClampedArray(this.module.HEAPU32.buffer, ret + 256 * 8 * 4, 256 * 224 * 4);
        } else {
            return new Uint8ClampedArray(this.module.HEAPU32.buffer, ret, 256 * 240 * 4);
        }
    }

    // メモリにデータを書き込むメソッド
    public writeMem(addr: number, data: number) {
        //console.log("VRAM:" + addr.toString(16) + "=" + data.toString(16));
        this.module._writeMem(addr & 0x2007, data);
    }

    // メモリからデータを読み込むメソッド
    public readMem(addr: number): number {
        return this.module._readMem(addr & 0x2007);
    }

    // VRAMにデータを書き込むメソッド
    public writeVram(addr: number, data: number) {
        this.module._writeVram(addr, data);
    }

    // VRAMからデータを読み込むメソッド
    public readVram(addr: number): number {
        return this.module._readVram(addr);
    }

    // スプライトにデータを書き込むメソッド
    public writeSprite(addr: number, data: number) {
        this.module._writeSprite(addr, data);
    }

    /**
     * ミラーモードを設定するメソッド
     * @param mode 0: 1画面 lower, 1: １画面 upper, 2:垂直ミラー, 3:水平ミラー, 4:4画面
     */
    public setMirrorMode(mode: number) {
        this.module._setMirrorMode(mode);
    }
    public reset(): void {
        this.module._reset();
    }
    public powerOff(): void {
        this.module._powerOff();
    }
}
