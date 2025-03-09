#include <emscripten.h>
#include <functional>

static std::function<void(int)> irqCallback;
// DMCのメモリ読み込み
static std::function<int(int)> dmcCallback;

#define MODE_5STEP 0x80
#define IRQ_DISABLE 0x40

#define DMC_IRQ 0x80
#define FRAME_IRQ 0x40
#define DMC_ACTIVE 0x10
#define TRIANGLE_ACTIVE 0x08
#define NOISE_ACTIVE 0x04
#define SQUARE2_ACTIVE 0x02
#define SQUARE1_ACTIVE 0x01

#define FRAME_CYCLE 7457

struct _reg
{
    int stepMode;
    int frameCounter;
    bool irqDisable;
    uint8_t state;
    int squareTimer[2];
    int triangleTimer;
};
static struct _reg reg = {4, 20, false, 0, {0, 0}, 0};
static int lengthIndexData[] = {0x0a, 0xfe, 0x14, 0x02, 0x28,
                                0x04, 0x50, 0x06, 0xa0, 0x08, 0x3c, 0x0a, 0x0e, 0x0c, 0x1a, 0x0e,
                                0x0c, 0x10, 0x18, 0x12, 0x30, 0x14, 0x60, 0x16, 0xc0, 0x18, 0x48,
                                0x1a, 0x10, 0x1c, 0x20, 0x1e};

// 1step=183 or 184 or 200までのサンプル数の返却
static uint8_t sampleResult[256];

// 音声合成用
static uint8_t pulseMixValue[31];
static uint16_t tndMixValue[3 * 15 + 2 * 15 + 127 + 1];

extern "C" EMSCRIPTEN_KEEPALIVE void setVolume(int volumeMax)
{
    EM_ASM({ console.log("APU Set Volume: " + $0); }, volumeMax);
    for (int i = 1; i < sizeof(pulseMixValue); i++)
    {
        pulseMixValue[i] = (uint8_t)(volumeMax * 95.88 / ((8128.8 / i) + 100));
    }
    for (int i = 1; i < sizeof(tndMixValue); i++)
    {
        tndMixValue[i] = (uint16_t)(volumeMax * 163.67 / (24329.8 / i + 100));
    }
}

#define SQUARE_SHIFT 4
#define TRIANGLE_SHIFT 5
#define NOISE_SHIFT 1

struct SweepData
{
    bool enabled;
    uint8_t period;
    uint16_t count;
    uint16_t value;
    bool upFlag;
};
struct EnvelopeData
{
    bool enabled;
    uint8_t period;
    uint8_t count;
};
/**
 * 矩形波音声データ出力情報
 * 183サンプル数で7457サイクル進む
 */
struct SquareOutput
{
    uint64_t timerCycle;
    uint64_t currentCycle;
    uint8_t *waveValue;
    uint8_t volume;
};
static uint8_t squareWaveValue[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},
    {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0},
    {1, 0, 0, 1, 1, 1, 1, 1}};

struct TriangleOutput
{
    uint64_t timerCycle;
    uint64_t currentCycle;
};
// 本来は 15,14...0,0,...14,15 だが、無音を 0 にするため逆にする
static uint8_t triangleWaveValue[] = {0, 1, 2, 3, 4, 5, 6, 7,
                                      8, 9, 10, 11, 12, 13, 14, 15,
                                      15, 14, 13, 12, 11, 10, 9, 8,
                                      7, 6, 5, 4, 3, 2, 1, 0};

struct NoiseOutput
{
    uint64_t timerCycle;
    uint64_t currentCycle;
    uint8_t volume;
};

class SquareSound
{
    bool enableFlag;
    bool loopFlag;
    bool updateFlag;
    uint8_t dutyValue;
    uint8_t volumeValue;
    uint8_t lengthCounter;
    uint16_t timerCount;

    SweepData sweepData;
    SweepData nextSweep;
    EnvelopeData envData;
    EnvelopeData nextEnv;

    SquareOutput output;

public:
    static SquareSound square[2];

    void powerOff()
    {
        enableFlag = false;
        loopFlag = false;
        updateFlag = true;
        dutyValue = 0;
        volumeValue = 0;
        lengthCounter = 0;
        timerCount = 0;
        sweepData.enabled = nextSweep.enabled = false;
        envData.enabled = nextEnv.enabled = false;
    }
    void reset()
    {
        // unchanged
    }
    /**
     * 240Hzの1ステップの音声データを出力する
     */
    void doOutput(uint8_t *sample, int size)
    {
        int delta = FRAME_CYCLE / size;
        bool keyOn = lengthCounter > 0;
        if (updateFlag)
        {
            updateFlag = false;
            if (keyOn && timerCount > 0 && output.timerCycle > 0)
            {
                output.currentCycle = (output.currentCycle * (timerCount << SQUARE_SHIFT)) / output.timerCycle;
            }
            else
            {
                output.currentCycle = 0;
            }
            output.timerCycle = timerCount << SQUARE_SHIFT;
        }
        while (size > 0)
        {
            if (output.currentCycle < output.timerCycle)
            {
                int idx = output.currentCycle * 8 / output.timerCycle;
                *sample = output.waveValue[idx] * output.volume;
                size--;
                sample++;
                output.currentCycle += delta;
            }
            else
            {
                // 新たな波形の開始
                if (output.timerCycle > 0)
                {
                    output.currentCycle -= output.timerCycle;
                }
                if (keyOn && timerCount > 0 && volumeValue > 0)
                {
                    output.timerCycle = timerCount << SQUARE_SHIFT;
                    output.volume = volumeValue;
                    output.waveValue = squareWaveValue[dutyValue];
                }
                else
                {
                    output.timerCycle = output.currentCycle = 0;
                    std::memset(sample, 0, size);
                    break;
                }
            }
        }
    }
    void setVolumeEnvelope(int val)
    {
        if (val & 0x10)
        {
            // エンベロープ無効
            volumeValue = val & 15;
            envData.enabled = nextEnv.enabled = false;
        }
        else
        {
            envData.period = envData.count = val & 15;
            envData.enabled = true;
        }
        dutyValue = (val >> 6) & 3;
        loopFlag = (val & 0x20) > 0;
    }
    void setSweep(int val)
    {
        if ((val & 0x80) && (val & 7) > 0)
        {
            nextSweep.enabled = true;
            nextSweep.upFlag = (val & 8) > 0;
            nextSweep.period = nextSweep.count = ((val >> 4) & 7) + 1;
            nextSweep.value = val & 7;
        }
        else
        {
            sweepData.enabled = nextSweep.enabled = false;
        }
    }
    void setTimer(int lengthIndex, int count)
    {
        if (!enableFlag)
        {
            return;
        }
        updateFlag = true;
        if (nextEnv.enabled)
        {
            envData = nextEnv;
            nextEnv.enabled = false;
        }
        if (nextSweep.enabled)
        {
            sweepData = nextSweep;
            nextSweep.enabled = false;
        }
        if (envData.enabled)
        {
            envData.count = envData.period;
            volumeValue = 15;
        }
        if (sweepData.enabled)
        {
            sweepData.count = sweepData.period;
        }
        if (count < 7 || count > 0x7fe)
        {
            // 無効
            lengthCounter = 0;
        }
        else
        {
            lengthCounter = lengthIndexData[lengthIndex];
            timerCount = count + 1;
        }
    }
    bool isPlaying()
    {
        return lengthCounter > 0;
    }
    void setEnabled(bool flag)
    {
        enableFlag = flag;
        if (!flag)
        {
            lengthCounter = 0;
        }
    }
    void stepCounter()
    {
        if (sweepData.enabled)
        {
            // TODO step
            if (lengthCounter > 0)
            {
                sweepData.count--;
                if (sweepData.count == 0)
                {
                    int tm = timerCount;
                    if (sweepData.upFlag)
                    {
                        tm -= (tm >> sweepData.value);
                        // ２番目だけさらに引く
                        if (this == &square[1])
                        {
                            tm--;
                        }
                    }
                    else
                    {
                        tm += (tm >> sweepData.value);
                    }
                    updateFlag = true;
                    if (tm < 8 || tm > 0x7ff)
                    {
                        // 無効化する
                        lengthCounter = 0;
                    }
                    else
                    {
                        timerCount = tm;
                    }
                    sweepData.count = sweepData.period;
                }
            }
        }
        if (!loopFlag && lengthCounter > 0)
        {
            lengthCounter--;
        }
    }
    void stepEnvelope()
    {
        if (envData.enabled)
        {
            // step
            envData.count--;
            if (envData.count == 0)
            {
                if (volumeValue > 0)
                {
                    volumeValue--;
                }
                else if (loopFlag)
                {
                    volumeValue = 15;
                }
                envData.count = envData.period;
            }
        }
    }
};
SquareSound SquareSound::square[2];

class TriangleSound
{
    bool enableFlag;
    bool loopFlag;
    bool updateFlag;
    uint8_t lengthCounter;
    uint8_t lineCounter;
    uint8_t lineCounterData;
    uint16_t timerCount;
    TriangleOutput output;

public:
    TriangleSound()
    {
        // ここでミキサーを初期化する
        setVolume(255);
        output.currentCycle = 0;
        output.timerCycle = 0;
    }
    void powerOff()
    {
        output.currentCycle = 0;
        output.timerCycle = 0;
        enableFlag = false;
        loopFlag = false;
        updateFlag = false;
        lengthCounter = 0;
        lineCounter = 0;
        lineCounterData = 0;
        timerCount = 0;
    }
    void reset()
    {
        // unchanged
    }
    /**
     * 240Hzの1ステップの音声データを出力する
     */
    void doOutput(uint8_t *sample, int size)
    {
        int delta = FRAME_CYCLE / size;
        bool keyOn = lengthCounter > 0 && lineCounter > 0;
        if (updateFlag)
        {
            updateFlag = false;
            if (keyOn && timerCount > 0 && output.timerCycle > 0)
            {
                // 同じ割合を残したまま変更する
                output.currentCycle = (output.currentCycle * (timerCount << TRIANGLE_SHIFT)) / output.timerCycle;
            }
            else
            {
                output.currentCycle = 0;
            }
            output.timerCycle = timerCount << TRIANGLE_SHIFT;
        }
        while (size > 0)
        {
            if (output.currentCycle < output.timerCycle)
            {
                int idx = output.currentCycle * 32 / output.timerCycle;
                if (keyOn)
                {
                    *sample = triangleWaveValue[idx];
                    size--;
                    sample++;
                    output.currentCycle += delta;
                }
                else
                {
                    std::memset(sample, triangleWaveValue[idx], size);
                    break;
                }
            }
            else
            {
                // 新たな波形の開始
                if (output.timerCycle > 0)
                {
                    output.currentCycle -= output.timerCycle;
                }
                if (keyOn && timerCount > 0)
                {
                    output.timerCycle = timerCount << TRIANGLE_SHIFT;
                }
                else
                {
                    output.timerCycle = output.currentCycle = 0;
                    std::memset(sample, 0, size);
                    break;
                }
            }
        }
    }
    void setLinear(int val)
    {
        loopFlag = (val & 0x80) > 0;
        lineCounter = lineCounterData = val & 0x7f;
    }
    void setTimer(int lengthIndex, int count)
    {
        if (!enableFlag)
        {
            return;
        }
        updateFlag = true;
        lineCounter = lineCounterData;
        lengthCounter = lengthIndexData[lengthIndex];
        timerCount = count + 1;
    }
    bool isPlaying()
    {
        return lengthCounter > 0 && lineCounter > 0;
    }
    void setEnabled(bool flag)
    {
        enableFlag = flag;
        if (!flag)
        {
            lengthCounter = 0;
        }
    }
    void stepCounter()
    {
        if (!loopFlag && lengthCounter > 0)
        {
            lengthCounter--;
        }
    }
};
static TriangleSound triangle;

static uint16_t noiseTimerIndex[] = {4, 8, 16, 32, 64, 96, 128, 160, 202,
                                     254, 380, 508, 762, 1016, 2034, 4068};
class NoiseSound
{
    bool enableFlag;
    bool loopFlag;
    bool updateFlag;
    bool shortFlag;
    uint8_t volumeValue;
    uint16_t lengthCounter;
    uint16_t shiftRegister;
    uint16_t timerCount;

    EnvelopeData envData;
    EnvelopeData nextEnv;

    NoiseOutput output;

public:
    NoiseSound()
    {
        shiftRegister = 1;
    }

    void powerOff()
    {
        enableFlag = false;
        loopFlag = false;
        updateFlag = false;
        shortFlag = false;
        volumeValue = 0;
        lengthCounter = 0;
        shiftRegister = 1;
        timerCount = 0;
        envData.enabled = nextEnv.enabled = false;
    }
    void reset()
    {
        // unchanged
    }
    /**
     * 240Hzの1ステップの音声データを出力する
     */
    void doOutput(uint8_t *sample, int size)
    {
        int delta = FRAME_CYCLE / size;
        bool keyOn = lengthCounter > 0;
        if (updateFlag)
        {
            updateFlag = false;
            if (keyOn && timerCount > 0 && output.timerCycle > 0)
            {
                output.currentCycle = (output.currentCycle * (timerCount << NOISE_SHIFT)) / output.timerCycle;
            }
            else
            {
                output.currentCycle = 0;
            }
            output.timerCycle = timerCount << NOISE_SHIFT;
        }
        while (size > 0)
        {
            if (keyOn && timerCount > 0)
            {
                // 音を鳴らす
                if (output.currentCycle < output.timerCycle)
                {
                    int len = (output.timerCycle - output.currentCycle + delta - 1) / delta;
                    if (len > size)
                    {
                        len = size;
                    }
                    std::memset(sample, output.volume, len);
                    sample += len;
                    size -= len;
                    output.currentCycle += len * delta;
                }
                else
                {
                    output.currentCycle -= output.timerCycle;
                    output.volume = getShiftBit() * volumeValue;
                }
            }
            else
            {
                output.timerCycle = output.currentCycle = 0;
                std::memset(sample, output.volume, size);
                break;
            }
        }
    }
    void setVolumeEnvelope(int val)
    {
        if (val & 0x10)
        {
            // Volume
            volumeValue = val & 15;
            envData.enabled = nextEnv.enabled = false;
        }
        else
        {
            // Envelope
            volumeValue = 0;
            nextEnv.enabled = true;
            nextEnv.count = nextEnv.period = val & 15;
        }
        loopFlag = (val & 0x20) > 0;
    }
    void setRandomMode(int val)
    {
        if (!enableFlag)
        {
            return;
        }
        shortFlag = (val & 80) > 0;
        timerCount = noiseTimerIndex[val & 15];
        updateFlag = true;
    }
    void setLength(int val)
    {
        if (nextEnv.enabled)
        {
            envData = nextEnv;
            nextEnv.enabled = false;
        }
        if (envData.enabled)
        {
            envData.count = envData.period;
            volumeValue = 15;
        }
        lengthCounter = lengthIndexData[(val >> 3) & 0x1f];
        shiftRegister = 0x4000;
    }
    void setEnabled(bool flag)
    {
        enableFlag = flag;
        if (!flag)
        {
            lengthCounter = 0;
        }
    }
    bool isPlaying()
    {
        return lengthCounter > 0;
    }
    void stepCounter()
    {
        if (!loopFlag && lengthCounter > 0)
        {
            lengthCounter--;
        }
    }
    void stepEnvelope()
    {
        if (envData.enabled)
        {
            envData.count--;
            if (envData.count == 0)
            {
                if (volumeValue > 0)
                {
                    volumeValue--;
                }
                else if (loopFlag)
                {
                    volumeValue = 15;
                }
                envData.count = envData.period;
            }
        }
    }
    int getShiftBit()
    {
        int ret = (shiftRegister & 1) ^ 1;
        if (shortFlag)
        {
            shiftRegister = (shiftRegister << 1) | (((shiftRegister >> 14) & 1) ^ ((shiftRegister >> 8) & 1));
        }
        else
        {
            shiftRegister = (shiftRegister << 1) | (((shiftRegister >> 14) & 1) ^ ((shiftRegister >> 13) & 1));
        }
        return ret;
    }
};
static NoiseSound noise;

// 未実装
static uint16_t periodIndexData[] = {
    0x1ac, 0x17c, 0x154, 0x140, 0x11e, 0x0fe, 0x0e2, 0x0d6,
    0x0be, 0x0a0, 0x08e, 0x080, 0x06a, 0x054, 0x048, 0x036};

class DeltaSound
{
    bool enableFlag;
    bool irqFlag;
    bool loopFlag;
    uint8_t periodIndex;
    uint8_t deltaValue;
    uint8_t sampleBuffer;
    uint16_t sampleSize;
    uint16_t sampleAddr;
    uint16_t nextAddr;
    uint16_t restSize;
    uint16_t counter;
    uint16_t shiftRegister;
    // 240Hzの間に書き込まれたdelta値を覚える
    // 正確ではないが、ここに書き込まれた数で均等分配する
    uint8_t deltaBuffer[100];
    uint8_t bufferIndex;

public:
    DeltaSound()
    {
    }
    void powerOff()
    {
        irqFlag = false;
        loopFlag = false;
        counter = 0;
        sampleBuffer = 0;
        shiftRegister = 0;
        nextAddr = 0;
        restSize = 0;
        bufferIndex = 0;
    }
    void reset()
    {
    }
    bool isPlaying()
    {
        return sampleSize > 0;
    }
    void setEnabled(bool flag)
    {
        enableFlag = flag;
        if (!flag)
        {
            shiftRegister = 0;
            counter = 0;
        }
    }
    void setMode(int val)
    {
        irqFlag = (val & 0x80) > 0;
        loopFlag = (val & 0x40) > 0;
        // 周波数
        periodIndex = val & 15;
        if (!irqFlag)
        {
            // クリア
            if (irqCallback)
            {
                irqCallback(0);
            }
        }
    }
    void setDelta(int val)
    {
        if (bufferIndex < sizeof(deltaBuffer))
        {
            deltaBuffer[bufferIndex++] = val & 127;
        }
        else
        {
            // オーバーしたものは捨てる
            deltaBuffer[bufferIndex - 1] = val & 127;
        }
    }
    void setAddress(int val)
    {
        sampleAddr = val * 64;
        nextAddr = sampleAddr;
    }
    void setSize(int val)
    {
        sampleSize = val * 16 + 1;
        restSize = 0;
    }
    void doOutput(uint8_t *sample, int size)
    {
        int delta = FRAME_CYCLE / size;
        if (!enableFlag)
        {
            std::memset(sample, deltaValue, size);
            return;
        }
        if (bufferIndex > 0)
        {
            // 新たに書き込みがされた
            int sz = size / bufferIndex;
            counter = 0;
            for (int i = 0; i < bufferIndex - 1; i++)
            {
                deltaValue = deltaBuffer[i];
                std::memset(sample, deltaValue, sz);
                sample += sz;
                size -= sz;
            }
            deltaValue = deltaBuffer[bufferIndex - 1];
            bufferIndex = 0;
        }
        if (sampleSize == 0)
        {
            // 終了
            std::memset(sample, deltaValue, size);
            return;
        }
        while (size > 0)
        {
            if (counter > delta)
            {
                counter -= delta;
                *sample++ = deltaValue;
                size--;
                continue;
            }
            counter += periodIndexData[periodIndex] - delta;
            // 新たにチェック
            if (!(shiftRegister & 0xff00))
            {
                if (restSize == 0)
                {
                    // 終了した
                    if (loopFlag)
                    {
                        restSize = sampleSize;
                        nextAddr = sampleAddr;
                    }
                    else
                    {
                        sampleSize = 0;
                        if (irqFlag && irqCallback)
                        {
                            irqCallback(1);
                        }
                    }
                }
                // 新たに読み取りが必要
                if (dmcCallback)
                {
                    shiftRegister = 0xff00 | dmcCallback(0xc000 | nextAddr);
                }
                else
                {
                    shiftRegister = 0xff00;
                }
                nextAddr++;
                restSize--;
            }
            if (shiftRegister & 1)
            {
                if (deltaValue > 1)
                {
                    deltaValue -= 2;
                }
            }
            else if (deltaValue < 126)
            {
                deltaValue += 2;
            }
            shiftRegister >>= 1;
        }
    }
};
static DeltaSound dmc;

// IRQリクエスト
extern "C" EMSCRIPTEN_KEEPALIVE void setIrqCallback(void (*callback)(int))
{
    irqCallback = callback;
}

// DMCリーダのメモリ読み込み呼び出し
extern "C" EMSCRIPTEN_KEEPALIVE void setDmcCallback(int (*callback)(int))
{
    dmcCallback = callback;
}

extern "C" EMSCRIPTEN_KEEPALIVE uint8_t *step(int samples)
{
    static uint8_t squareBuf[2][200];
    static uint8_t triangleBuf[200];
    static uint8_t noiseBuf[200];
    static uint8_t dmcBuf[200];

    reg.frameCounter--;
    if (reg.frameCounter < 0)
    {
        reg.frameCounter = 19;
    }
    int ix = reg.frameCounter % reg.stepMode;
    if (reg.stepMode == 5)
    {
        // 5step
        if (ix != 1)
        {
            SquareSound::square[0].stepEnvelope();
            SquareSound::square[1].stepEnvelope();
            noise.stepEnvelope();
        }
        if (ix == 0 || ix == 3)
        {
            SquareSound::square[0].stepCounter();
            SquareSound::square[1].stepCounter();
            triangle.stepCounter();
            noise.stepCounter();
        }
    }
    else
    {
        // 4 step
        SquareSound::square[0].stepEnvelope();
        SquareSound::square[1].stepEnvelope();
        noise.stepEnvelope();
        if (ix == 0 || ix == 2)
        {
            SquareSound::square[0].stepCounter();
            SquareSound::square[1].stepCounter();
            triangle.stepCounter();
            noise.stepCounter();
        }
    }
    // state更新
    if (SquareSound::square[0].isPlaying())
    {
        reg.state |= 1;
    }
    else
    {
        reg.state &= ~1;
    }
    if (SquareSound::square[1].isPlaying())
    {
        reg.state |= 2;
    }
    else
    {
        reg.state &= ~2;
    }
    if (triangle.isPlaying())
    {
        reg.state |= 4;
    }
    else
    {
        reg.state &= ~4;
    }
    if (noise.isPlaying())
    {
        reg.state |= 8;
    }
    else
    {
        reg.state &= ~8;
    }
    if (dmc.isPlaying())
    {
        reg.state |= 16;
    }
    else
    {
        reg.state &= ~16;
    }
    if (!reg.irqDisable && (reg.frameCounter & 3) == 0 && !(reg.state & FRAME_IRQ))
    {
        reg.state |= FRAME_IRQ;
        if (irqCallback)
        {
            irqCallback(1);
        }
    }
    if (samples > 100)
    {
        // 音声の出力
        SquareSound::square[0].doOutput(squareBuf[0], samples);
        SquareSound::square[1].doOutput(squareBuf[1], samples);
        triangle.doOutput(triangleBuf, samples);
        noise.doOutput(noiseBuf, samples);
        dmc.doOutput(dmcBuf, samples);
        for (int i = 0; i < samples; i++)
        {
            sampleResult[i] = std::min(255, pulseMixValue[squareBuf[0][i] + squareBuf[1][i]] + tndMixValue[triangleBuf[i] * 3 + noiseBuf[i] * 2 + dmcBuf[i]]);
            // sampleResult[i] = 0.00752 * (squareBuf[0][i] + squareBuf[1][i]) + 0.00851 * triangleBuf[i] + 0.00494 * noiseBuf[i] + 0.00335 * dmcBuf[i];
        }
    }
    return sampleResult;
}

extern "C" EMSCRIPTEN_KEEPALIVE void writeMem(int addr, int val)
{
    if (addr < 0x4008)
    {
        // 矩形波
        int ix = (addr >> 2) & 1;
        switch (addr & 3)
        {
        case 0:
            SquareSound::square[ix].setVolumeEnvelope(val);
            break;
        case 1:
            SquareSound::square[ix].setSweep(val);
            break;
        case 2:
            reg.squareTimer[ix] = ((reg.squareTimer[ix] & 0x700) | val);
            break;
        case 3:
            reg.squareTimer[ix] = ((reg.squareTimer[ix] & 0xff) | ((val & 7) << 8));
            SquareSound::square[ix].setTimer((val >> 3) & 0x1f, reg.squareTimer[ix]);
            break;
        default:
            break;
        }
    }
    else if (addr < 0x400c)
    {
        // 三角波
        switch (addr & 3)
        {
        case 0:
            triangle.setLinear(val);
            break;
        case 2:
            reg.triangleTimer = ((reg.triangleTimer & 0x700) | val);
            break;
        case 3:
            reg.triangleTimer = ((reg.triangleTimer & 0xff) | ((val & 7) << 8));
            triangle.setTimer((val >> 3) & 0x1f, reg.triangleTimer);
            break;
        default:
            break;
        }
    }
    else if (addr < 0x4010)
    {
        // Noise
        switch (addr & 3)
        {
        case 0:
            noise.setVolumeEnvelope(val);
            break;
        case 2:
            noise.setRandomMode(val);
            break;
        case 3:
            noise.setLength(val);
            break;
        default:
            break;
        }
    }
    else if (addr < 0x4014)
    {
        switch (addr & 3)
        {
        case 0:
            dmc.setMode(val);
            break;
        case 1:
            dmc.setDelta(val);
            break;
        case 2:
            dmc.setAddress(val);
            break;
        case 3:
            dmc.setSize(val);
            break;
        default:
            break;
        }
    }
    else if (addr == 0x4015)
    {
        // 音声チャネル制御
        SquareSound::square[0].setEnabled((val & 1) > 0);
        SquareSound::square[1].setEnabled((val & 2) > 0);
        triangle.setEnabled((val & 4) > 0);
        noise.setEnabled((val & 8) > 0);
        dmc.setEnabled((val & 16) > 0);
    }
    else if (addr == 0x4017)
    {
        if (val & MODE_5STEP)
        {
            // 5 step
            reg.stepMode = 5;
            reg.irqDisable = true;
        }
        else
        {
            // 4 step
            reg.stepMode = 4;
            reg.irqDisable = (val & IRQ_DISABLE) > 0;
        }
        if (reg.irqDisable)
        {
            reg.state &= ~FRAME_IRQ;
            if (irqCallback)
            {
                irqCallback(0);
            }
        }
    }
}
extern "C" EMSCRIPTEN_KEEPALIVE int readMem(int addr)
{
    if (addr == 0x4015)
    {
        int ret = reg.state;
        reg.state &= ~FRAME_IRQ;
        if (irqCallback)
        {
            // 割り込み待ちはクリアしない
            // irqCallback(0);
        }
        return ret;
    }
    return 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE void reset()
{
    EM_ASM({
        console.log("reset");
    });
}

extern "C" EMSCRIPTEN_KEEPALIVE void powerOff()
{
    EM_ASM({
        console.log("powerOff");
    });
    SquareSound::square[0].powerOff();
    SquareSound::square[1].powerOff();
    triangle.powerOff();
    noise.powerOff();
    reg.stepMode = 4;
    reg.frameCounter = 20;
    reg.irqDisable = false;
    reg.state = 0;
}