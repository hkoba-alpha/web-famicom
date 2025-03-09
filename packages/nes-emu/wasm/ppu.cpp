#include <emscripten.h>
#include <cstring>
#include <functional>

// 1 scan = 341 PPU cycle,(3ppu = 1cpu)
// 262 line

// 事前スキャンライン
// -> 次のスキャンラインの最初の２つのタイルデータをシフトレジスタへ入れる
// 280-304の間、レンダリングが有効であればvscrollが再ロード
/*
                                         [BBBBBBBB] - Next tile's pattern data,
                                         [BBBBBBBB] - 2 bits per pixel
                                          ||||||||<----[Transfers every inc hori(v)]
                                          vvvvvvvv
      Serial-to-parallel - [AAAAAAAA] <- [BBBBBBBB] <- [1...] - Parallel-to-serial
         shift registers - [AAAAAAAA] <- [BBBBBBBB] <- [1...] - shift registers
                            vvvvvvvv
                            ||||||||   [Sprites 0..7]----+  [EXT in]----+
                            ||||||||                     |              |
[fine_x selects a bit]---->[  Mux   ]------------>[Priority mux]----->[Mux]---->[Pixel]
                            ||||||||                              |
                            ^^^^^^^^                              +------------>[EXT out]
      Serial-to-parallel - [PPPPPPPP] <- [P] - 1-bit latch
         shift registers - [PPPPPPPP] <- [P] - 1-bit latch
                                          ^
                                          |<--------[Transfers every inc hori(v)]
                                     [  Mux   ]<----[coarse_x bit 1 and coarse_y bit 1 select 2 bits]
                                      ||||||||
                                      ^^^^^^^^
                                     [PPPPPPPP] - Next tile's attributes data
*/

#define NMI_ENABLED 0x80
#define PPU_MASTER 0x40
#define SPRITE16 0x20
#define BG_PATTERN 0x10
#define SPRITE_PATTERN 0x08
#define INC_MODE 0x04

#define SPRITE_ENABLE 0x10
#define BG_ENABLE 0x08
#define SPRITE_CLIP 0x04
#define BG_CLIP 0x02
#define GRAY_SCALE 1

#define DISPLAY_ENABLE (SPRITE_ENABLE | BG_ENABLE)

#define PPU_CYCLES 341

struct _reg
{
    //
    unsigned int v : 15;
    unsigned int t : 15;
    unsigned int x : 3;
    unsigned int w : 1;
    unsigned int odd : 1;
    // ctrlを反映
    uint8_t inc;
    uint8_t spSize;
    uint16_t bgAddr;
    uint16_t spAddr;
    // フェッチしたパターン
    uint16_t bgPattern[2];
    // 属性
    uint8_t attr;
    // バッファ遅延
    uint8_t readBuf;
};
static _reg reg = {0, 0, 0, 0, 0, 1, 8, 0, 0, {0, 0}, 0};
static uint32_t colorPalette[] = {
    0xFF757575, 0xFF8F1B27, 0xFFAB0000, 0xFF9F0047, 0xFF77008F,
    0xFF1300AB, 0xFF0000A7, 0xFF0B007F, 0xFF002F43, 0xFF004700,
    0xFF005100, 0xFF173F00, 0xFF5F3F1B, 0xFF000000, 0xFF050505,
    0xFF050505, 0xFFBCBCBC, 0xFFEF7300, 0xFFEF3B23, 0xFFF30083,
    0xFFBF00BF, 0xFF5B00E7, 0xFF002BDB, 0xFF0F4FCB, 0xFF00738B,
    0xFF009700, 0xFF00AB00, 0xFF3B9300, 0xFF8B8300, 0xFF111111,
    0xFF090909, 0xFF090909, 0xFFFFFFFF, 0xFFFFBF3F, 0xFFFF975F,
    0xFFF78BA7, 0xFFFF7BF7, 0xFFB777FF, 0xFF6377FF, 0xFF3B9BFF,
    0xFF3FBFF3, 0xFF13D383, 0xFF4BDF4F, 0xFF98F858, 0xFFDBEB00,
    0xFF666666, 0xFF0D0D0D, 0xFF0D0D0D, 0xFFFFFFFF, 0xFFFFE7AB,
    0xFFFFD7C7, 0xFFFFCBD7, 0xFFFFC7FF, 0xFFDBC7FF, 0xFFB3BFFF,
    0xFFABDBFF, 0xFFA3E7FF, 0xFFA3FFE3, 0xFFBFF3AB, 0xFFCFFFB3,
    0xFFF3FF9F, 0xFFDDDDDD, 0xFF111111, 0xFF111111};

// TODO
static uint32_t grayPalette[] = {
    0xFF757575, 0xFF2B2B2B, 0xFF131313, 0xFF272727, 0xFF383838,
    0xFF353535, 0xFF313131, 0xFF272727, 0xFF2F2F2F, 0xFF292929,
    0xFF2F2F2F, 0xFF272727, 0xFF373737, 0xFF000000, 0xFF050505,
    0xFF050505, 0xFFBCBCBC, 0xFF5E5E5E, 0xFF484848, 0xFF424242,
    0xFF4E4E4E, 0xFF4F4F4F, 0xFF5A5A5A, 0xFF6C6C6C, 0xFF6D6D6D,
    0xFF585858, 0xFF646464, 0xFF5D5D5D, 0xFF5C5C5C, 0xFF111111,
    0xFF090909, 0xFF090909, 0xFFFFFFFF, 0xFFA0A0A0, 0xFF929292,
    0xFF9F9F9F, 0xFFAFAFAF, 0xFFA6A6A6, 0xFF9D9D9D, 0xFFADADAD,
    0xFFBFBFBF, 0xFFA5A5A5, 0xFFA3A3A3, 0xFFBDBDBD, 0xFFA2A2A2,
    0xFF656565, 0xFF0C0C0C, 0xFF0C0C0C, 0xFFFFFFFF, 0xFFD7D7D7,
    0xFFD6D6D6, 0xFFD4D4D4, 0xFFDEDEDE, 0xFFDADADA, 0xFFD0D0D0,
    0xFFE0E0E0, 0xFFE6E6E6, 0xFFECECEC, 0xFFD7D7D7, 0xFFE2E2E2,
    0xFFE0E0E0, 0xFFDDDDDD, 0xFF111111, 0xFF111111};

// 色の強調表示
static uint32_t emphasisColor[] = {
    0xFF8C8C8C, 0xFFAB202E, 0xFFCD0000, 0xFFBE0055, 0xFF8E00AB,
    0xFF1600CD, 0xFF0000C8, 0xFF0D0098, 0xFF003850, 0xFF005500,
    0xFF006100, 0xFF1B4B00, 0xFF724B20, 0xFF000000, 0xFF060606,
    0xFF060606, 0xFFE1E1E1, 0xFFFF8A00, 0xFFFF462A, 0xFFFF009D,
    0xFFE500E5, 0xFF6D00FF, 0xFF0033FF, 0xFF125EF3, 0xFF008AA6,
    0xFF00B500, 0xFF00CD00, 0xFF46B000, 0xFFA69D00, 0xFF141414,
    0xFF0A0A0A, 0xFF0A0A0A, 0xFFFFFFFF, 0xFFFFE54B, 0xFFFFB572,
    0xFFFFA6C8, 0xFFFF93FF, 0xFFDB8EFF, 0xFF768EFF, 0xFF46BAFF,
    0xFF4BE5FF, 0xFF16FD9D, 0xFF5AFF5E, 0xFFB6FF69, 0xFFFFFF00,
    0xFF7A7A7A, 0xFF0F0F0F, 0xFF0F0F0F, 0xFFFFFFFF, 0xFFFFFFCD,
    0xFFFFFFEE, 0xFFFFF3FF, 0xFFFFEEFF, 0xFFFFEEFF, 0xFFD6E5FF,
    0xFFCDFFFF, 0xFFC3FFFF, 0xFFC3FFFF, 0xFFE5FFCD, 0xFFF8FFD6,
    0xFFFFFFBE, 0xFFFFFFFF, 0xFF141414, 0xFF141414};

/**
 * NameTableへのインデックス参照
 */
static int nameIndex[4] = {0, 0, 1, 1};

struct _state
{
    uint8_t ctrl2000;
    uint8_t ctrl2001;
    uint8_t state; // 0x80: vblank, 0x40: hit, 0x20: overflow(うまく動かないらしい)
};
static _state state;

static uint16_t lineBuf[256]; // 0x100: 色あり, 0x200: スプライト前, 0x400: Sprite0
static uint32_t screen[256 * 240];

static uint8_t pattern[0x2000];
static uint8_t nameTable[4][0x400];
static uint8_t palette[0x20];

struct _sprite
{
    uint8_t mem[256];
    uint8_t addr;
};
static _sprite sprite;

struct _cycle
{
    int ppuCycle;
    int notifyPpuCycle;
};
static _cycle cycle;

static std::function<void(int)> hBlankCallback;
static std::function<void()> vBlankCallback;
static std::function<void(int)> cpuCallback;

static void notifyCpuCycle()
{
    if (cycle.ppuCycle > cycle.notifyPpuCycle)
    {
        int cpuCycle = (cycle.ppuCycle - cycle.notifyPpuCycle + 2) / 3;
        cycle.notifyPpuCycle += cpuCycle * 3;
        if (cpuCallback)
        {
            cpuCallback(cpuCycle);
        }
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE void writeSprite(int addr, int value)
{
    sprite.mem[addr & 255] = value;
}

extern "C" EMSCRIPTEN_KEEPALIVE void writeVram(int addr, int value)
{
    if (addr < 0x2000)
    {
        pattern[addr & 0x1fff] = value;
    }
    else if (addr < 0x3f00)
    {
        nameTable[nameIndex[(addr >> 10) & 3]][addr & 0x3ff] = value;
    }
    else if (addr & 3)
    {
        palette[addr & 0x1f] = value & 0x3f;
    }
    else
    {
        palette[addr & 0x0f] = value & 0x3f;
        palette[0x10 | (addr & 0x0f)] = value & 0x3f;
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE int readVram(int addr)
{
    if (addr < 0x2000)
    {
        return pattern[addr];
    }
    else if (addr < 0x3f00)
    {
        return nameTable[nameIndex[(addr >> 10) & 3]][addr & 0x3ff];
    }
    else
    {
        return palette[addr & 0x1f];
    }
}
extern "C" EMSCRIPTEN_KEEPALIVE void writeMem(int addr, int val)
{
    switch (addr & 7)
    {
    case 0:
        state.ctrl2000 = val;
        if (val & INC_MODE)
        {
            reg.inc = 32;
        }
        else
        {
            reg.inc = 1;
        }
        if (val & SPRITE16)
        {
            reg.spSize = 16;
        }
        else
        {
            reg.spSize = 8;
        }
        if (val & BG_PATTERN)
        {
            reg.bgAddr = 0x1000;
        }
        else
        {
            reg.bgAddr = 0;
        }
        if (val & SPRITE_PATTERN)
        {
            reg.spAddr = 0x1000;
        }
        else
        {
            reg.spAddr = 0;
        }
        break;
    case 1:
        state.ctrl2001 = val;
        break;
    case 3:
        sprite.addr = val;
        break;
    case 4:
        sprite.mem[sprite.addr++] = val;
        break;
    case 5:
        if (reg.w)
        {
            reg.t = (reg.t & 0x0c1f) | ((val & 7) << 12) | ((val & 0xf8) << 2);
            reg.w = 0;
        }
        else
        {
            reg.x = val & 7;
            reg.t = (reg.t & 0x7fe0) | (val >> 3);
            reg.w = 1;
        }
        break;
    case 6:
        if (reg.w)
        {
            reg.t = (reg.t & 0x7f00) | val;
            reg.w = 0;
            reg.v = reg.t;
        }
        else
        {
            reg.t = (reg.t & 0xff) | ((val & 0x3f) << 8);
            reg.w = 1;
        }
        break;
    case 7:
        // EM_ASM({ console.log("writePPU:#" + $0.toString(16) + ", " + $1.toString(16)); }, reg.v, val);
        writeVram(reg.v, val);
        reg.v += reg.inc;
        break;

    default:
        break;
    }
}
static void fetchTile()
{
    reg.bgPattern[0] <<= 8;
    reg.bgPattern[1] <<= 8;
    reg.attr <<= 2;
    if (!(state.ctrl2001 & BG_ENABLE))
    {
        return;
    }
    // name table
    uint16_t addr = 0x2000 | (reg.v & 0x0fff);
    int tile = readVram(addr);
    // パターン
    addr = reg.bgAddr | (tile << 4) | (reg.v >> 12);
    reg.bgPattern[0] |= pattern[addr];
    reg.bgPattern[1] |= pattern[addr | 8];
    // 属性
    int at = readVram(0x23c0 | (reg.v & 0x0c00) | ((reg.v >> 4) & 0x38) | ((reg.v >> 2) & 7));
    // EM_ASM({ console.log("Tile:" + $0.toString(16) + " addr=" + $1.toString(16)); }, reg.v, 0x23c0 | (reg.v & 0x0c00) | ((reg.v >> 4) & 0x38) | ((reg.v >> 2) & 7));
    if (reg.v & 2)
    {
        at >>= 2;
    }
    if (reg.v & 0x40)
    {
        at >>= 4;
    }
    // at >>= 2;
    reg.attr |= (at & 3);
    // horizontal scroll
    if ((reg.v & 0x1f) == 31)
    {
        reg.v &= ~0x1f;
        reg.v ^= 0x400;
    }
    else
    {
        reg.v++;
    }
}

static void fetchSprite(int y)
{
    for (int x = 0; x < 256; x++)
    {
        lineBuf[x] = palette[0];
    }
    if (!(state.ctrl2001 & SPRITE_ENABLE))
    {
        return;
    }
    int count = 0;
    for (int i = 0; i < 64; i++)
    {
        int sy = sprite.mem[i * 4];
        int dy = (y - sy) & 255;
        if (dy >= reg.spSize)
        {
            continue;
        }
        count++;
        if (count > 8)
        {
            // Overflow
            state.state |= 0x20;
            break;
        }
        int sx = sprite.mem[i * 4 + 3];
        int tile = sprite.mem[i * 4 + 1];
        int attr = sprite.mem[i * 4 + 2];
        if (attr & 0x80)
        {
            // Y座標反転
            dy = reg.spSize - 1 - dy;
        }
        int addr;
        if (reg.spSize == 16)
        {
            addr = (tile & 1) << 12;
            if (dy < 8)
            {
                tile &= 0xfe;
            }
            else
            {
                tile |= 1;
                dy &= 7;
            }
        }
        else
        {
            addr = reg.spAddr;
        }
        addr |= (tile << 4) | dy;
        uint8_t pattern0 = pattern[addr];
        uint8_t pattern1 = pattern[addr | 8];
        for (int dx = 0; dx < 8; dx++)
        {
            int px = (sx + dx) & 255;
            if ((lineBuf[px] & 0x100) || (px < 8 && !(state.ctrl2001 & SPRITE_CLIP)))
            {
                continue;
            }
            int bit;
            if (attr & 0x40)
            {
                // X座標反転
                bit = 0x01 << dx;
            }
            else
            {
                bit = 0x80 >> dx;
            }
            int pix = ((pattern0 & bit) ? 1 : 0) | ((pattern1 & bit) ? 2 : 0);
            if (pix > 0)
            {
                lineBuf[px] = palette[((attr & 3) << 2) | pix | 0x10] | ((attr & 0x20) ? 0x100 : 0x300);
                if (i == 0)
                {
                    // Sprite0
                    lineBuf[px] |= 0x400;
                }
            }
        }
    }
}

extern "C" EMSCRIPTEN_KEEPALIVE uint32_t *renderScreen()
{
    // EM_ASM({ console.log("RenderStart", $0.toString(16), $1.toString(16)); }, reg.t, reg.v);
    reg.odd = !reg.odd;
    if (reg.odd && (state.ctrl2001 & BG_ENABLE))
    {
        // サイクルスキップ
        cycle.notifyPpuCycle++;
    }
    // pre render line(261)から始める
    cycle.ppuCycle = 279;
    // clear vblank,sprite0,overflow
    state.state = 0;
    notifyCpuCycle();
    // vert(v)=vert(t)
    if (state.ctrl2001 & DISPLAY_ENABLE)
    {
        reg.v = (reg.t & ~0xc00) | ((state.ctrl2000 & 3) << 10);
        // EM_ASM({ console.log("$2000=" + $0.toString(16) + " $2001=" + $1.toString(16) + " t=" + $2.toString(16) + " v=" + $3.toString(16) + " sp0.y=" + $4); }, state.ctrl2000, state.ctrl2001, reg.t, reg.v, sprite.mem[0]);
    }
    cycle.ppuCycle = 320;
    notifyCpuCycle();
    fetchTile();
    cycle.ppuCycle = 328;
    notifyCpuCycle();
    fetchTile();
    cycle.ppuCycle = 341;
    // ここから
    for (int y = 1; y <= 240; y++)
    {
        // BG部分
        cycle.ppuCycle = y * PPU_CYCLES + 1;
        for (int x = 0; x < 32; x++)
        {
            notifyCpuCycle();
            // ピクセル描画とタイルフェッチ
            if (state.ctrl2001 & BG_ENABLE)
            {
                if (x > 0 || (state.ctrl2001 & BG_CLIP))
                {
                    int bit = 0x8000 >> reg.x;
                    for (int dx = 0; dx < 8; dx++)
                    {
                        int px = (x << 3) | dx;
                        int pix = ((reg.bgPattern[0] & bit) ? 1 : 0) | ((reg.bgPattern[1] & bit) ? 2 : 0);
                        if (pix > 0)
                        {
                            if (lineBuf[px] & 0x400)
                            {
                                // Sprite0 hit
                                state.state |= 0x40;
                            }
                            if (!(lineBuf[px] & 0x200))
                            {
                                lineBuf[px] = palette[((reg.attr << ((bit & 0xff) ? 2 : 0)) & 0xc) | pix] | 0x100;
                            }
                        }
                        bit >>= 1;
                    }
                }
            }
            if (x < 31)
            {
                fetchTile();
            }
            cycle.ppuCycle += 8;
        }
        cycle.ppuCycle = y * PPU_CYCLES + 255;
        notifyCpuCycle();
        if (state.ctrl2001 & DISPLAY_ENABLE)
        {
            // vwcroll, horizontal reset
            if ((reg.v & 0x7000) != 0x7000)
            {
                // １ドット移動
                reg.v += 0x1000;
            }
            else if ((reg.v & 0x3e0) == 0x3a0)
            {
                // ページ切り替え
                reg.v &= ~0x73e0;
                reg.v ^= 0x800;
            }
            else
            {
                // 次の行
                reg.v &= ~0x7000;
                reg.v += 0x20;
            }
            reg.v = (reg.v & ~0x41f) | ((state.ctrl2000 & 1) << 10) | (reg.t & 0x1f);
        }
        // ピクセル反映
        int py = (y - 1) << 8;
        uint32_t emphasisFlag = 0;
        if (state.ctrl2001 & 0x80)
        {
            // 赤強調
            emphasisFlag = 0x0000ff;
        }
        if (state.ctrl2001 & 0x40)
        {
            // 緑強調
            emphasisFlag |= 0x00ff00;
        }
        if (state.ctrl2001 & 0x20)
        {
            // 青強調
            emphasisFlag |= 0xff0000;
        }
        for (int x = 0; x < 256; x++)
        {
            int col = lineBuf[x] & 0x3f;
            if (state.ctrl2001 & GRAY_SCALE)
            {
                screen[py | x] = grayPalette[col];
            }
            else if (emphasisFlag)
            {
                screen[py | x] = (colorPalette[col] & ~emphasisFlag) | (emphasisColor[col] & emphasisFlag);
            }
            else
            {
                screen[py | x] = colorPalette[col];
            }
        }
        // sprite
        fetchSprite(y - 1);
        // hBlank
        if (hBlankCallback)
        {
            hBlankCallback(y - 1);
        }
        cycle.ppuCycle = y * PPU_CYCLES + 320;
        notifyCpuCycle();
        fetchTile();
        cycle.ppuCycle = y * PPU_CYCLES + 328;
        fetchTile();
    }
    cycle.ppuCycle = 242 * PPU_CYCLES + 1;
    notifyCpuCycle();
    // VBlank
    state.state |= 0x80;
    // ここで、CPU命令を1つ実行したい
    cycle.ppuCycle += 3;
    notifyCpuCycle();
    // 2002を先に読み込まれると、NMIがキャンセルされるらしい
    if (vBlankCallback && (state.state & 0x80) && (state.ctrl2000 & NMI_ENABLED))
    {
        vBlankCallback();
    }
    cycle.ppuCycle = 262 * PPU_CYCLES;
    notifyCpuCycle();
    // 戻す
    cycle.notifyPpuCycle -= 262 * PPU_CYCLES;
    cycle.ppuCycle = 0;
    return screen;
}

extern "C" EMSCRIPTEN_KEEPALIVE int readMem(int addr)
{
    switch (addr & 7)
    {
    case 2:
    {
        int val = state.state;
        state.state &= 0x7f;
        reg.w = 0;
        return val;
    }
    case 7:
    {
        // バッファ遅延
        int ret = reg.readBuf;
        reg.readBuf = readVram(reg.v);
        if (addr >= 0x3f00)
        {
            // パレットは即時応答
            ret = reg.readBuf;
        }
        reg.v += reg.inc;
        return ret;
    }
    default:
        break;
    }
    return 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE void setHblankCallback(void (*callback)(int))
{
    hBlankCallback = callback;
}

extern "C" EMSCRIPTEN_KEEPALIVE void setVblankCallback(void (*callback)())
{
    vBlankCallback = callback;
}
extern "C" EMSCRIPTEN_KEEPALIVE void setCpuCallback(void (*callback)(int))
{
    cpuCallback = callback;
}
extern "C" EMSCRIPTEN_KEEPALIVE void reset()
{
    std::memset(&reg, 0, sizeof(reg));
    std::memset(&state, 0, sizeof(state));
    writeMem(0x2000, 0);
    writeMem(0x2001, 0);
}
extern "C" EMSCRIPTEN_KEEPALIVE void powerOff()
{
    std::memset(pattern, 0, sizeof(pattern));
    std::memset(nameTable, 0, sizeof(nameTable));
    std::memset(palette, 0, sizeof(palette));
    std::memset(&sprite, 0, sizeof(sprite));
    reset();
}
/**
 * ミラーモード
 * 0: 1画面 lower bank
 * 1: 1画面 upper bank
 * 2: 垂直ミラー
 * 3: 水平ミラー
 * 4: ４画面
 */
extern "C" EMSCRIPTEN_KEEPALIVE void setMirrorMode(int mode)
{
    if (mode == 2)
    {
        // 垂直ミラー
        nameIndex[0] = 0;
        nameIndex[1] = 1;
        nameIndex[2] = 0;
        nameIndex[3] = 1;
    }
    else if (mode == 3)
    {
        // 水平ミラー
        nameIndex[0] = 0;
        nameIndex[1] = 0;
        nameIndex[2] = 1;
        nameIndex[3] = 1;
    }
    else if (mode == 4)
    {
        nameIndex[0] = 0;
        nameIndex[1] = 1;
        nameIndex[2] = 2;
        nameIndex[3] = 3;
    }
    else
    {
        int num = mode & 1;
        nameIndex[0] = num;
        nameIndex[1] = num;
        nameIndex[2] = num;
        nameIndex[3] = num;
    }
}