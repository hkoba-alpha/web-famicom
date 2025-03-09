interface AudioWorkletProcessor {
    readonly port: MessagePort;
    process(
        inputs: Float32Array[][],
        outputs: Float32Array[][],
        parameters: Record<string, Float32Array>
    ): boolean;
}

declare var AudioWorkletProcessor: {
    prototype: AudioWorkletProcessor;
    new(options?: AudioWorkletNodeOptions): AudioWorkletProcessor;
};

declare function registerProcessor(
    name: string,
    processorCtor: typeof AudioWorkletProcessor
): void;

class ApuAudioPlayer extends AudioWorkletProcessor {
    private buffer: Float32Array;
    private writeIndex: number;
    private readIndex: number;
    private availableSamples: number;
    private waitFlag = true;
    private waitSamples = 400;
    private silientValue = 0.0;

    public constructor() {
        super();
        this.buffer = new Float32Array(3000); // 最大 3000 サンプルを保持
        this.writeIndex = 0;
        this.readIndex = 0;
        this.availableSamples = 0;

        // 240Hzで呼び出される
        this.port.onmessage = (event) => {
            const samples = event.data as Uint8Array;
            if (this.waitFlag) {
                this.waitSamples = samples.length * 8;
            } else {
                this.port.postMessage({ avaiable: this.availableSamples });
            }
            for (let i = 0; i < samples.length; i++) {
                this.buffer[this.writeIndex] = samples[i] / 255.0;
                this.writeIndex = (this.writeIndex + 1) % this.buffer.length;
                this.availableSamples++;
            }
        };
    }
    process(inputs: Float32Array[][], outputs: Float32Array[][]) {
        const output = outputs[0][0];
        //console.log(output.length, this.availableSamples);
        for (let i = 0; i < output.length; i++) {
            if ((this.waitFlag && this.availableSamples >= this.waitSamples) || (!this.waitFlag && this.availableSamples > 0)) {
                if (this.availableSamples === output.length) {
                    console.log('buffer full: ' + this.availableSamples);
                }
                this.waitFlag = false;
                this.silientValue = output[i] = this.buffer[this.readIndex];
                this.readIndex = (this.readIndex + 1) % this.buffer.length;
                this.availableSamples--;
            } else {
                if (!this.waitFlag) {
                    console.log('buffer empty');
                }
                this.waitFlag = true;
                output[i] = this.silientValue; // バッファが空なら無音
            }
        }
        return true;
    }
}

registerProcessor('my-audio-processor', ApuAudioPlayer);