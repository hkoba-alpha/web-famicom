import { IFamCanvas, IFamPad, IFamSound, Mapper, NesFile, PadButton } from "./NesFile";

export class PPUCanvas implements IFamCanvas {
    private context: CanvasRenderingContext2D;
    private image: ImageData;
    private clip: boolean;
    private adjust?: () => void;

    // コンストラクタでCanvasのコンテキストを取得
    public constructor(private element: HTMLCanvasElement) {
        this.clip = element.height * 256 / element.width < 230;
        this.context = element.getContext('2d')!;
        const height = this.clip ? 224 : 240;
        this.image = this.context.createImageData(256, height);
        if (element.width >= 512) {
            // 拡大する
            const scale = element.width / 256;
            this.adjust = () => {
                this.context.drawImage(
                    this.context.canvas, // ソース
                    0, 0, 256, height, // 元のサイズ
                    0, 0, Math.floor(256 * scale), Math.floor(height * scale)  // 2倍に拡大
                );
            };
        }
    }
    powerOff(): void {
        this.context.clearRect(0, 0, this.element.width, this.element.height);
    }

    render(image: Uint8ClampedArray): void {
        this.image.data.set(image);
        this.context.putImageData(this.image, 0, 0);
        if (this.adjust) {
            this.adjust();
        }
    }

    isClip(): boolean {
        return this.clip;
    }
}

export class PPUCanvasWebGL implements IFamCanvas {
    private gl: WebGLRenderingContext;
    private program: WebGLProgram;
    private texture: WebGLTexture;
    private buffer: WebGLBuffer;
    private vertexArray: Float32Array;
    private clip: boolean;
    private height: number;

    constructor(element: HTMLCanvasElement) {
        this.clip = element.height * 256 / element.width < 230;
        this.height = this.clip ? 224 : 240;
        this.gl = element.getContext('webgl')!;
        this.gl.viewport(0, 0, 512, this.height * 2);

        this.program = this.createShaderProgram();
        this.texture = this.createTexture();
        this.buffer = this.createBuffer();
    }
    powerOff(): void {
        this.gl.clearColor(0, 0, 0, 0);
        this.gl.clear(this.gl.COLOR_BUFFER_BIT);
    }

    private createShaderProgram(): WebGLProgram {
        const vsSource = `
            attribute vec2 a_position;
            varying vec2 v_texCoord;
            void main() {
                v_texCoord = a_position * 0.5 + 0.5;
                gl_Position = vec4(a_position, 0.0, 1.0);
            }
        `;

        const fsSource = `
            precision mediump float;
            varying vec2 v_texCoord;
            uniform sampler2D u_texture;
            void main() {
                vec2 texCoord = vec2(v_texCoord.x, 1.0 - v_texCoord.y); // Y座標を反転
                vec4 color = texture2D(u_texture, texCoord);
                if (mod(gl_FragCoord.y, 2.0) < 1.0) {
                    color.rgb *= 0.75;
                }
                gl_FragColor = color;
            }
        `;

        const program = this.gl.createProgram()!;
        this.gl.attachShader(program, this.compileShader(this.gl.VERTEX_SHADER, vsSource));
        this.gl.attachShader(program, this.compileShader(this.gl.FRAGMENT_SHADER, fsSource));
        this.gl.linkProgram(program);
        return program;
    }

    private compileShader(type: number, source: string): WebGLShader {
        const shader = this.gl.createShader(type)!;
        this.gl.shaderSource(shader, source);
        this.gl.compileShader(shader);
        return shader;
    }

    private createTexture(): WebGLTexture {
        const texture = this.gl.createTexture()!;
        this.gl.bindTexture(this.gl.TEXTURE_2D, texture);
        this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_MIN_FILTER, this.gl.NEAREST);
        this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_MAG_FILTER, this.gl.NEAREST);
        this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_WRAP_S, this.gl.CLAMP_TO_EDGE);
        this.gl.texParameteri(this.gl.TEXTURE_2D, this.gl.TEXTURE_WRAP_T, this.gl.CLAMP_TO_EDGE);
        return texture;
    }

    private createBuffer(): WebGLBuffer {
        const buffer = this.gl.createBuffer()!;
        this.gl.bindBuffer(this.gl.ARRAY_BUFFER, buffer);
        this.vertexArray = new Float32Array([
            -1, -1, 1, -1, -1, 1,
            -1, 1, 1, -1, 1, 1
        ]);
        this.gl.bufferData(this.gl.ARRAY_BUFFER, this.vertexArray, this.gl.STATIC_DRAW);
        return buffer;
    }

    render(image: Uint8ClampedArray): void {
        this.gl.bindTexture(this.gl.TEXTURE_2D, this.texture);
        this.gl.texImage2D(this.gl.TEXTURE_2D, 0, this.gl.RGBA, 256, this.height, 0, this.gl.RGBA, this.gl.UNSIGNED_BYTE, image);

        this.gl.useProgram(this.program);
        this.gl.bindBuffer(this.gl.ARRAY_BUFFER, this.buffer);
        const position = this.gl.getAttribLocation(this.program, 'a_position');
        this.gl.enableVertexAttribArray(position);
        this.gl.vertexAttribPointer(position, 2, this.gl.FLOAT, false, 0, 0);

        this.gl.drawArrays(this.gl.TRIANGLES, 0, 6);
    }

    isClip(): boolean {
        return this.clip;
    }
}


export const player1Buttons: { [button: number]: string[]; } = {
    [PadButton.A]: ['KeyX'],
    [PadButton.B]: ['KeyZ'],
    [PadButton.START]: ['Enter'],
    [PadButton.SELECT]: ['MetaLeft'],
    [PadButton.UP]: ['ArrowUp'],
    [PadButton.LEFT]: ['ArrowLeft'],
    [PadButton.DOWN]: ['ArrowDown'],
    [PadButton.RIGHT]: ['ArrowRight'],
};

export class KeyboardPad implements IFamPad {
    private static pushKeyMap: { [key: string]: boolean; };

    private static isPressed(...keys: string[]): boolean {
        if (!this.pushKeyMap) {
            this.pushKeyMap = {};
            const onKeyDown = (e: KeyboardEvent) => {
                this.pushKeyMap[e.code] = true;
            };
            const onKeyUp = (e: KeyboardEvent) => {
                delete this.pushKeyMap[e.code];
            };
            window.addEventListener("keydown", onKeyDown);
            window.addEventListener("keyup", onKeyUp);
        }
        for (let key of keys) {
            if (key in this.pushKeyMap) {
                return true;
            }
        }
        return false;
    }

    private buttonMap: { [button: number]: string[]; } = {};
    public constructor(buttons: { [button: number]: string[]; } = player1Buttons) {
        Object.assign(this.buttonMap, buttons);
    }
    getButton(button: PadButton): boolean {
        return KeyboardPad.isPressed(...(this.buttonMap[button] || []));
    }
}

export class WorkerSound implements IFamSound {
    private static instance: WorkerSound;

    public static async getSound(): Promise<WorkerSound> {
        if (!this.instance) {
            const context = new (window.AudioContext || (window as any).webkitAudioContext)({
                sampleRate: 44100
            });
            await context.audioWorklet.addModule('assets/apu-player.js');
            const apuNode = new AudioWorkletNode(context, 'my-audio-processor');
            apuNode.connect(context.destination);
            this.instance = new WorkerSound(apuNode);
        }
        return this.instance;
    }

    samples: number = 184;

    private constructor(private apuNode: AudioWorkletNode) {
        this.apuNode.port.onmessage = (event) => {
            const { avaiable } = event.data;
            // 183.75なので、183-185の間をいったりきたりする
            if (avaiable > 183 * 10) {
                console.log("Sample Down");
                this.samples = 183;
            } else if (avaiable < 184 * 4) {
                console.log("Sample Up");
                this.samples = 184;
            }
        }
    }

    play(data: Uint8Array): void {
        this.apuNode.port.postMessage(data);
    }
}

export class Mapper0 extends Mapper {
    private constructor(nesFile: NesFile) {
        super(nesFile);
    }

    @Mapper.entry(0)
    public static create(nes: NesFile): Mapper {
        return new Mapper0(nes);
    }
    protected writeRom(addr: number, data: number): void {
        // Mapper0ではROMへの書き込みは無視されます
    }
    protected initRom(): void {
        this.setChrBank(0, 0);
        this.setPrgBank(1, this.getPrgBankCount() - 1);
        if (this.nesFile.prgBankList.length > 1) {
            this.setPrgBank(0, 0);
        }
    }
}
export class Mapper1 extends Mapper {
    private count: number;
    private data: number;
    /**
     * 0-1: ミラーリング(0=)
     * 2: PRGバンク固定: 0-8000切り替え, 1-c000切り替え
     * 3: PRG単位: 0-32k, 1-16k
     * 4: CHR単位: 0-8k, 1-4k
     */
    private mode: number = 0;

    private pageInfo = {
        lowPage: 0,
        highPage: 0,
        swapPage: 0,
        size512: false
    };

    @Mapper.entry(1)
    public static create(nes: NesFile): Mapper {
        return new Mapper1(nes);
    }

    private constructor(nesFile: NesFile) {
        super(nesFile);
        this.count = 0;
        this.data = 0;
    }

    protected initRom(): void {
        this.count = 0;
        this.data = 0;
        this.setChrBankSize(0x1000);
        // 16k=on, low=on
        this.mode = 0xc;
        this.pageInfo.size512 = this.getPrgBankCount() > 16;
        this.pageInfo.lowPage = -1;
        this.pageInfo.highPage = -1;
        this.selectPrgPage(0, (this.getPrgBankCount() - 1) & 0xf, 0);
        this.setChrBank(0, 0);
        this.setChrBank(1, 1);
    }
    protected writeRom(addr: number, data: number): void {
        //console.log("MapperWrite: " + addr.toString(16) + "=" + data.toString(16));
        if ((data & 0x80) > 0) {
            this.count = this.data = 0;
            this.mode |= 0xc;   // PRG-ROM bank mode = 3 にする
            return;
        }
        this.data |= ((data & 1) << this.count);
        this.count++;
        if (this.count < 5) {
            return;
        }
        let reg = this.data;
        this.data = 0;
        this.count = 0;
        if (addr < 0xa000) {
            // 設定
            this.mode = reg;
            this.ppu?.setMirrorMode(reg & 3);
            //console.log("Change Mode:" + this.mode.toString(16));
        } else if (addr < 0xc000) {
            if (this.pageInfo.size512) {
                this.selectPrgPage(this.pageInfo.lowPage, this.pageInfo.highPage, reg & 0x10);
            }
            // CHR bank0
            if (this.mode & 0x10) {
                // 4k
                this.setChrBank(0, reg);
                //this.selectChrBank(0, reg, 1);
            } else {
                // 8k
                this.setChrBank(0, reg & ~1).setChrBank(1, reg | 1);
                //this.selectChrBank(0, reg);
            }
        } else if (addr < 0xe000) {
            // CHR bank0
            if (this.mode & 0x10) {
                // 4k
                this.setChrBank(1, reg);
                //this.selectChrBank(1, reg, 1);
            } else {
                // 8k
                //this.setChrBank(0, reg >> 1, 8);
            }
        } else {
            // prg
            //console.log("Prg:" + reg);
            if (this.mode & 0x8) {
                // 16k
                if (this.mode & 4) {
                    // last fix, 8000 change
                    this.selectPrgPage(reg & 0xf, (this.getPrgBankCount() - 1) & 0xf, this.pageInfo.swapPage);
                } else {
                    // first fix, c000 change
                    this.selectPrgPage(0, reg & 0xf, this.pageInfo.swapPage);
                }
            } else {
                // 32k TODO
                this.selectPrgPage(reg & 0xe, (reg & 0xe) | 1, this.pageInfo.swapPage);
            }
        }
    }
    private selectPrgPage(low: number, high: number, swap: number): void {
        if (low != this.pageInfo.lowPage || swap != this.pageInfo.swapPage) {
            this.pageInfo.lowPage = low;
            this.setPrgBank(0, swap | low);
            //console.log("Change Low:" + low + " swap:" + swap);
        }
        if (high != this.pageInfo.highPage || swap != this.pageInfo.swapPage) {
            this.pageInfo.highPage = high;
            this.setPrgBank(1, swap | high);
            //console.log("Change High:" + high + " swap:" + swap);
        }
        this.pageInfo.swapPage = swap;
    }
}

class Mapper2 extends Mapper {
    @Mapper.entry(2)
    public static create(nes: NesFile): Mapper {
        return new Mapper2(nes);
    }

    private constructor(nesFile: NesFile) {
        super(nesFile);
    }

    protected initRom(): void {
        this.setPrgBank(0, 0);
        this.setPrgBank(1, this.nesFile.prgBankList.length - 1);
        this.setChrBank(0, 0);
    }
    protected writeRom(addr: number, data: number): void {
        //console.log("Change Bank:" + data);
        this.setPrgBank(0, data);
    }
}
class MapperMMC3 extends Mapper {
    private selectMode = 0;
    private pageInfo = {
        evenPage: 0,
        oddPage: 1
    };

    @Mapper.entry(4)
    public static create(nes: NesFile): Mapper {
        return new MapperMMC3(nes);
    }

    private constructor(nesFile: NesFile) {
        super(nesFile);
    }

    protected initRom(): void {
        this.pageInfo.evenPage = 0;
        this.pageInfo.oddPage = 1;
        this.setPrgBankSize(0x2000).setChrBankSize(0x400);
        this.setPrgBank(0, this.getPrgBankCount() - 2);
        this.setPrgBank(1, this.pageInfo.oddPage);
        this.setPrgBank(2, this.pageInfo.evenPage);
        this.setPrgBank(3, this.getPrgBankCount() - 1);
    }
    private selectChrBank(bank: number, index: number, size: number): void {
        const vram = bank * 0x400;
        const chr = this.nesFile.prgBankList[index >> 4];
        const from = (index & 15) * 0x400;
        for (let i = 0; i < size * 0x400; i++) {
            this.ppu!.writeVram(vram + i, chr[from + i]);
        }
    }
    protected writeRom(addr: number, data: number): void {
        if (addr < 0xa000) {
            if (addr & 1) {
                // odd bank
                let bank = 0;
                let chr = true;
                let size = 2;
                const mode = this.selectMode & 7;
                if (mode === 6) {
                    this.pageInfo.evenPage = data;
                    if (this.selectMode & 0x40) {
                        this.setPrgBank(2, data);
                    } else {
                        this.setPrgBank(0, data);
                    }
                } else if (mode === 7) {
                    this.pageInfo.oddPage = data;
                    this.setPrgBank(1, data);
                } else {
                    // R0*2, R1*2, R2, R3, R4, R5
                    // R2, R3, R4, R5, R0*2, R1*2
                    let size = 2;
                    let bank = 0;
                    if (mode < 2) {
                        bank = mode * 2;
                    } else {
                        size = 1;
                        bank = mode + 2;
                    }
                    if (this.selectMode & 0x80) {
                        bank ^= 4;
                    }
                    for (let i = 0; i < size; i++) {
                        this.setChrBank(bank + i, data + i);
                    }
                }
            } else {
                // even
                this.selectMode = data;
                if (data & 0x40) {
                    this.setPrgBank(0, this.getPrgBankCount() - 2);
                    this.setPrgBank(2, this.pageInfo.evenPage);
                } else {
                    this.setPrgBank(0, this.pageInfo.evenPage);
                    this.setPrgBank(2, this.getPrgBankCount() - 2);
                }
            }
        }
    }
}

class MMC1java extends Mapper {
    private count = 0;
    private data = 0;
    private chr4k = false;
    private prgLow = false;
    private prg16k = false;
    private prgLowPage = 0;
    private prgHighPage = 0;
    private swapBase = 0;
    private size512 = false;
    private constructor(nesFile: NesFile) {
        super(nesFile);
    }
    protected initRom(): void {
        this.count = 0;
        this.data = 0;
        this.prgLowPage = this.prgHighPage = -1;
        this.prgLow = this.prg16k = true;
        this.chr4k = false;
        this.setPrg(0, (this.getBankSize() - 1) & 0x0f, 0);
        this.size512 = this.getBankSize() > 16
    }
    getBankSize(): number {
        return this.nesFile.prgBankList.length;
    }

    private setPrg(low: number, high: number, swap: number): void {
        //console.log("16k=" + this.prg16k + ", area=" + this.prgLow);
        //console.log("MAP:" + low + ", " + high + ", swap=" + swap);
        if (low != this.prgLowPage || swap != this.swapBase) {
            this.prgLowPage = low;
            this.selectPage(0, (swap | low) * 2);
            this.selectPage(1, (swap | low) * 2 + 1);
        }
        if (high != this.prgHighPage || swap != this.swapBase) {
            this.prgHighPage = high;
            this.selectPage(2, (swap | high) * 2);
            this.selectPage(3, (swap | high) * 2 + 1);
        }
        this.swapBase = swap;
    }
    public selectPage(page: number, romPage: number) {
        const offset = (romPage & 1) > 0 ? 0x2000 : 0;
        const bank = this.nesFile.prgBankList[(romPage >> 1) % this.nesFile.prgBankList.length];
        this.prgBankMap[page] = bank.subarray(offset, offset + 0x2000);
        return this;
    }
    //@Mapper.entry(1)
    public static create(nes: NesFile): Mapper {
        return new MMC1java(nes);
    }
    protected writeRom(addr: number, val: number): void {

        //System.out.printf("MAP:%04X=%02X\n", addr, val);
        if ((val & 0x80) > 0) {
            this.count = this.data = 0;
            return;
        }
        this.data |= ((val & 1) << this.count);
        this.count++;
        if (this.count < 5) {
            return;
        }
        const reg = this.data;
        this.data = 0;
        this.count = 0;
        if (addr < 0xa000) {
            // 設定
            this.chr4k = (reg & 0x10) > 0;
            this.prgLow = (reg & 4) > 0;
            this.prg16k = (reg & 8) > 0;
            // TODO
            this.ppu?.setMirrorMode(reg & 3);
        } else if (addr < 0xc000) {
            if (this.size512) {
                this.setPrg(this.prgLowPage, this.prgHighPage, reg & 0x10);
            }
            // chr low
            let page = reg & 0xf;
            if ((reg & 0x10) > 0) {
                page += this.nesFile.chrBankList.length;
            }
            if (this.chr4k) {
                // 4k
                this.setChrBank(page, page);
            } else {
                // 8k
                this.setChrBank(0, page);
                this.setChrBank(1, page + 1);
            }
        } else if (addr < 0xe000) {
            // chr high
            let page = reg & 0xf;
            if ((reg & 0x10) > 0) {
                page += this.nesFile.chrBankList.length;
            }
            if (this.chr4k) {
                // 4k
                this.setChrBank(1, page);
            }
        } else {
            // prg
            if (this.prg16k) {
                // 16k
                if (this.prgLow) {
                    this.setPrg(reg & 0xf, (this.getBankSize() - 1) & 0xf, this.swapBase);
                } else {
                    this.setPrg(0, reg & 0xf, this.swapBase);
                }
            } else {
                // 32k
                this.setPrg(reg & 0xe, (reg & 0xe) | 1, this.swapBase);
            }
        }
    }
}