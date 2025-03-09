#include <emscripten.h>
#include <functional>

// APUへ通知するステップ数
#define APU_STEP_COUNT 7457

#define FLAG_NEGATIVE 0x80
#define FLAG_OVERFLOW 0x40
// NMI/IRQ=0, BRK/PHP 1
#define FLAG_BREAK 0x10
#define FLAG_DECIMAL 0x08
#define FLAG_INTERRUPT 0x04
#define FLAG_ZERO 0x02
#define FLAG_CARRY 0x01

static std::function<void(int)> apuStepCallback;
static std::function<void(int, int)> memWriteCallback;
static std::function<int(int)> memReadCallback;
static std::function<void(int, int, int, int, int, int, int)> debugCallback;
static int debugCycle;

struct _reg
{
    unsigned int a : 8;
    unsigned int x : 8;
    unsigned int y : 8;
    unsigned int s : 8;
    // NV1B DIZC(0x20=いつも1)
    unsigned int p : 8;
    // 起動時はメモリ 0xfffc の値
    unsigned int pc : 16;
    // いろいろなフラグ
    unsigned int irqRequest : 1;
    unsigned int nmiRequest : 1;
    // 遅延のIRQセット
    char nextIrq;
};
static _reg reg;

struct _cycle
{
    int cpuCycle;
    int notifyCpuCycle;
};
static _cycle cycle;

struct _context
{
    int addr;
    int cycle;
};
static _context context;

// 起動時フラグ
static bool powerOn = true;

// デバッグ出力
static bool debugFlag = false;
static bool faultFlag = false;

static void notifyApuStep()
{
    if (cycle.notifyCpuCycle >= APU_STEP_COUNT)
    {
        if (apuStepCallback)
        {
            apuStepCallback(cycle.notifyCpuCycle / APU_STEP_COUNT);
        }
        cycle.notifyCpuCycle %= APU_STEP_COUNT;
    }
}

static void writeMem(int addr, int val)
{
    notifyApuStep();
    if (memWriteCallback)
    {
        memWriteCallback(addr, val);
    }
}
static int readMem(int addr)
{
    notifyApuStep();
    if (memReadCallback)
    {
        return memReadCallback(addr);
    }
    return 0;
}

static void context_set(uint8_t val)
{
    if (context.addr < 0)
    {
        reg.a = val;
    }
    else
    {
        writeMem(context.addr, val);
    }
}
static uint8_t context_get()
{
    if (context.addr < 0)
    {
        return reg.a;
    }
    else
    {
        return readMem(context.addr);
    }
}

/**
 * フラグを設定する
 * @param val 設定する値
 * @param mask 設定するフラグ
 * @param old 設定前の値
 * @param mem 計算に使ったメモリ
 * @return 設定した値
 */
static uint8_t flags(int val, int mask, int old = -1, int mem = -1)
{
    if (mask & FLAG_NEGATIVE)
    {
        if (val & 0x80)
        {
            reg.p |= FLAG_NEGATIVE;
        }
        else
        {
            reg.p &= ~FLAG_NEGATIVE;
        }
    }
    if (mask & FLAG_OVERFLOW)
    {
        if (old < 0)
        {
            if (val & 0x40)
            {
                reg.p |= FLAG_OVERFLOW;
            }
            else
            {
                reg.p &= ~FLAG_OVERFLOW;
            }
        }
        else
        {
            if ((old ^ val) & (mem ^ val) & 0x80)
            {
                reg.p |= FLAG_OVERFLOW;
            }
            else
            {
                reg.p &= ~FLAG_OVERFLOW;
            }
        }
    }
    if (mask & FLAG_ZERO)
    {
        if (val & 0xff)
        {
            reg.p &= ~FLAG_ZERO;
        }
        else
        {
            reg.p |= FLAG_ZERO;
        }
    }
    if (mask & FLAG_CARRY)
    {
        if (val & 0xf00)
        {
            reg.p |= FLAG_CARRY;
        }
        else
        {
            reg.p &= ~FLAG_CARRY;
        }
    }
    return val & 0xff;
}

/**
 * CPUのアドレスモード定義
 */
struct CpuAddressing
{
    std::function<void()> operand;
    std::function<uint16_t(uint16_t, char *)> text;
};

static CpuAddressing immediate = {
    []()
    {
        context.addr = reg.pc++;
        context.cycle = 2;
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " #$%02x", readMem(addr));
        return addr + 1;
    }};
static CpuAddressing accumulator = {
    []()
    {
        context.addr = -1;
    },
    [](uint16_t addr, char *txt)
    {
        return addr;
    }};
static CpuAddressing zeroPage = {
    []()
    {
        context.addr = readMem(reg.pc++);
        context.cycle = 3;
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " $%02x", readMem(addr));
        return addr + 1;
    }};
static CpuAddressing zeroPageX = {
    []()
    {
        context.addr = (readMem(reg.pc++) + reg.x) & 0xff;
        context.cycle = 4;
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " $%02x,X", readMem(addr));
        return addr + 1;
    }};
static CpuAddressing zeroPageY = {
    []()
    {
        context.addr = (readMem(reg.pc++) + reg.y) & 0xff;
        context.cycle = 4;
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " $%02x,Y", readMem(addr));
        return addr + 1;
    }};
static CpuAddressing absolute = {
    []()
    {
        context.addr = readMem(reg.pc++) | (readMem(reg.pc++) << 8);
        context.cycle = 4;
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " $%04x", readMem(addr) | (readMem(addr + 1) << 8));
        return addr + 2;
    }};
static CpuAddressing absoluteX = {
    []()
    {
        uint16_t addr = readMem(reg.pc++) | (readMem(reg.pc++) << 8);
        context.addr = (addr + reg.x) & 0xffff;
        context.cycle = 4;
        // STAだけ常に+1
        if ((addr & 0xff00) != (context.addr & 0xff00))
        {
            context.cycle++;
        }
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " $%04x,X", readMem(addr) | (readMem(addr + 1) << 8));
        return addr + 2;
    }};
static CpuAddressing absoluteXsta = {
    []()
    {
        uint16_t addr = readMem(reg.pc++) | (readMem(reg.pc++) << 8);
        context.addr = (addr + reg.x) & 0xffff;
        context.cycle = 5;
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " $%04x,X", readMem(addr) | (readMem(addr + 1) << 8));
        return addr + 2;
    }};
static CpuAddressing absoluteY = {
    []()
    {
        uint16_t addr = readMem(reg.pc++) | (readMem(reg.pc++) << 8);
        context.addr = (addr + reg.y) & 0xffff;
        context.cycle = 4;
        // STAだけ常に+1
        if ((addr & 0xff00) != (context.addr & 0xff00))
        {
            context.cycle++;
        }
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " $%04x,Y", readMem(addr) | (readMem(addr + 1) << 8));
        return addr + 2;
    }};
static CpuAddressing absoluteYsta = {
    []()
    {
        uint16_t addr = readMem(reg.pc++) | (readMem(reg.pc++) << 8);
        context.addr = (addr + reg.y) & 0xffff;
        context.cycle = 5;
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " $%04x,Y", readMem(addr) | (readMem(addr + 1) << 8));
        return addr + 2;
    }};
static CpuAddressing indirectX = {
    []()
    {
        uint16_t addr = (readMem(reg.pc++) + reg.x) & 0xff;
        context.addr = readMem(addr) | (readMem((addr + 1) & 0xff) << 8);
        context.cycle = 6;
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " ($%02x,X)", readMem(addr));
        return addr + 1;
    }};
static CpuAddressing indirectY = {
    []()
    {
        uint16_t addr = readMem(reg.pc++);
        context.addr = readMem(addr) | (readMem((addr + 1) & 0xff) << 8);
        addr = context.addr;
        context.addr = (context.addr + reg.y) & 0xffff;
        context.cycle = 5;
        // STAだけ常に+1
        if ((addr & 0xff00) != (context.addr & 0xff00))
        {
            context.cycle++;
        }
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " ($%02x),Y", readMem(addr));
        return addr + 1;
    }};
static CpuAddressing indirectYsta = {
    []()
    {
        uint16_t addr = readMem(reg.pc++);
        context.addr = readMem(addr) | (readMem((addr + 1) & 0xff) << 8);
        addr = context.addr;
        context.addr = (context.addr + reg.y) & 0xffff;
        context.cycle = 6;
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " ($%02x),Y", readMem(addr));
        return addr + 1;
    }};
static CpuAddressing indirect = {
    []()
    {
        uint16_t addr = readMem(reg.pc++) | (readMem(reg.pc++) << 8);
        context.addr = (readMem(addr) | (readMem((addr & 0xff00) | ((addr + 1) & 0xff)) << 8)) & 0xffff;
        context.cycle = 6; // JMPは -1 なので +1 しておく
    },
    [](uint16_t addr, char *txt)
    {
        sprintf(txt, " ($%04x)", readMem(addr) | (readMem(addr + 1) << 8));
        return addr + 2;
    }};
static CpuAddressing relative(std::function<int()> condition)
{
    return {
        [condition]()
        {
            // reg.pcはまだ進んでいない
            if (condition())
            {
                context.cycle++;
                uint16_t bak = reg.pc + 1;
                reg.pc = bak + (int8_t)readMem(reg.pc);
                if ((bak & 0xff00) != (reg.pc & 0xff00))
                {
                    context.cycle++;
                }
            }
            else
            {
                reg.pc++;
            }
        },
        [](uint16_t addr, char *txt)
        {
            uint8_t val = readMem(addr);
            sprintf(txt, " $%02x(=$%04x)", val, addr + 1 + (int8_t)val);
            return addr + 1;
        }};
}

static void _immediate()
{
    context.addr = reg.pc++;
    context.cycle = 2;
}
static void _accumulator()
{
    context.addr = -1;
}
static void _zeroPage()
{
    context.addr = readMem(reg.pc++);
    context.cycle = 3;
}
static void _zeroPageX()
{
    context.addr = (readMem(reg.pc++) + reg.x) & 0xff;
    context.cycle = 4;
}
static void _zeroPageY()
{
    context.addr = (readMem(reg.pc++) + reg.y) & 0xff;
    context.cycle = 4;
}
static void _absolute()
{
    context.addr = readMem(reg.pc++) | (readMem(reg.pc++) << 8);
    context.cycle = 4;
}
static void _absoluteX()
{
    uint16_t addr = readMem(reg.pc++) | (readMem(reg.pc++) << 8);
    context.addr = (addr + reg.x) & 0xffff;
    context.cycle = 4;
    // STAだけ常に+1
    if ((addr & 0xff00) != (context.addr & 0xff00))
    {
        context.cycle++;
    }
}
static void _absoluteXsta()
{
    uint16_t addr = readMem(reg.pc++) | (readMem(reg.pc++) << 8);
    context.addr = (addr + reg.x) & 0xffff;
    context.cycle = 5;
}
static void _absoluteY()
{
    uint16_t addr = readMem(reg.pc++) | (readMem(reg.pc++) << 8);
    context.addr = (addr + reg.y) & 0xffff;
    context.cycle = 4;
    // STAだけ常に+1
    if ((addr & 0xff00) != (context.addr & 0xff00))
    {
        context.cycle++;
    }
}
static void _absoluteYsta()
{
    uint16_t addr = readMem(reg.pc++) | (readMem(reg.pc++) << 8);
    context.addr = (addr + reg.y) & 0xffff;
    context.cycle = 5;
}
static void _indirectX()
{
    uint16_t addr = (readMem(reg.pc++) + reg.x) & 0xff;
    context.addr = readMem(addr) | (readMem((addr + 1) & 0xff) << 8);
    context.cycle = 6;
}
static void _indirectY()
{
    uint16_t addr = readMem(reg.pc++);
    context.addr = readMem(addr) | (readMem((addr + 1) & 0xff) << 8);
    addr = context.addr;
    context.addr = (context.addr + reg.y) & 0xffff;
    context.cycle = 5;
    // STAだけ常に+1
    if ((addr & 0xff00) != (context.addr & 0xff00))
    {
        context.cycle++;
    }
}
static void _indirectYsta()
{
    uint16_t addr = readMem(reg.pc++);
    context.addr = readMem(addr) | (readMem((addr + 1) & 0xff) << 8);
    addr = context.addr;
    context.addr = (context.addr + reg.y) & 0xffff;
    context.cycle = 6;
}
static void _indirect()
{
    uint16_t addr = readMem(reg.pc++) | (readMem(reg.pc++) << 8);
    context.addr = (readMem(addr) | (readMem((addr & 0xff00) | ((addr + 1) & 0xff)) << 8)) & 0xffff;
    context.cycle = 6; // JMPは -1 なので +1 しておく
}
static void _relative(int condition)
{
    // reg.pcはまだ進んでいない
    if (condition)
    {
        context.cycle++;
        uint16_t bak = reg.pc + 1;
        reg.pc = bak + (int8_t)readMem(reg.pc);
        if ((bak & 0xff00) != (reg.pc & 0xff00))
        {
            context.cycle++;
        }
    }
    else
    {
        reg.pc++;
    }
}
static void push(int val)
{
    writeMem(0x100 | reg.s--, val);
}
static int pop()
{
    return readMem(0x100 | ++reg.s);
}

// 命令一覧
static std::function<void()> operand[256];

// 命令のテキスト
static std::function<uint16_t(uint16_t, char *)> operandText[256];
// 命令文字列を返却するバッファ
static char operandResultBuf[32];

using OperandPair = std::pair<int, const CpuAddressing &>;

class CpuOperand
{
    int cycle;
    std::function<void()> func;

public:
    CpuOperand(const char *text, int cycle, std::function<void()> func, std::initializer_list<OperandPair> args);
    // addrを使わない場合
    CpuOperand(const char *text, int opcode, int cycle, std::function<void()> func);
    CpuOperand(const char *text, int opcode, int cycle, const CpuAddressing &addressing);
    void execute(std::function<void()> ope);
};

static void entryOperand(int opcode, std::function<void()> ope)
{
    operand[opcode] = ope;
}
static void entryOperandText(int opcode, std::function<uint16_t(uint16_t, char *)> ope)
{
    operandText[opcode] = ope;
}

CpuOperand::CpuOperand(const char *text, int cycle, std::function<void()> func, std::initializer_list<OperandPair> args) : cycle(cycle), func(func)
{
    for (const auto &arg : args)
    {
        entryOperand(arg.first, [this, arg]()
                     { this->execute(arg.second.operand); });
        entryOperandText(arg.first, [text, arg](uint16_t addr, char *buf)
                         {
            strcpy(buf, text);
            return arg.second.text(addr + 1, buf + strlen(text)); });
    }
}

CpuOperand::CpuOperand(const char *text, int opcode, int cycle, std::function<void()> func) : cycle(cycle), func(func)
{
    entryOperand(opcode, [this]()
                 { this->execute(nullptr); });
    entryOperandText(opcode, [text](uint16_t addr, char *buf)
                     {
            strcpy(buf, text);
            return addr + 1; });
}

CpuOperand::CpuOperand(const char *text, int opcode, int cycle, const CpuAddressing &addressing) : cycle(cycle), func(addressing.operand)
{
    entryOperand(opcode, [this]()
                 { this->execute(nullptr); });
    entryOperandText(opcode, [text, addressing](uint16_t addr, char *buf)
                     {
            strcpy(buf, text);
            return addressing.text(addr+1, buf + strlen(text)); });
}

void CpuOperand::execute(std::function<void()> ope)
{
    reg.pc++;
    context.addr = reg.pc;
    if (ope)
    {
        ope();
    }
    context.cycle += cycle;
    func();
}

static int executeCpu()
{
    context.cycle = 0;
    // 割り込みのチェック
    if (reg.nmiRequest)
    {
        if (debugFlag)
        {
            EM_ASM({
                console.log("NMI");
            });
        }
        reg.nmiRequest = 0;
        push(reg.pc >> 8);
        push(reg.pc);
        reg.p &= ~FLAG_BREAK;
        push(reg.p);
        reg.p |= FLAG_INTERRUPT;
        reg.pc = readMem(0xfffa) | (readMem(0xfffb) << 8);
        reg.nextIrq = -1;
        return 7;
    }
    else if (reg.irqRequest && !(reg.p & FLAG_INTERRUPT))
    {
        if (debugFlag)
        {
            EM_ASM({ console.log("beforeIRQ:" + $0.toString(16) + " , " + $1.toString(16)); }, reg.p, reg.pc);
        }
        reg.irqRequest = 0;
        push(reg.pc >> 8);
        push(reg.pc);
        reg.p &= ~FLAG_BREAK;
        push(reg.p);
        int bak = reg.pc;
        reg.p |= FLAG_INTERRUPT;
        reg.pc = readMem(0xfffe) | (readMem(0xffff) << 8);
        reg.nextIrq = -1;
        if (debugFlag)
        {
            EM_ASM({ console.log("IRQ:" + $0.toString(16) + " -> " + $1.toString(16)); }, bak, reg.pc);
        }
        return 7;
    }
    /*
        命令が終わった後にIRQが変わる
        ただし、以下の流れはちょっと違う
        CLI
        BRK
        これは、CLIでIフラグをクリアしようとするが、クリアされるのはBRKの後
        しかし、BRKは割り込みが発生してIフラグをセットするので、CLIのクリアが無効化される
    */
    int nextIrq = reg.nextIrq;
    int code = readMem(reg.pc);
    if (debugCallback)
    {
        debugCallback(reg.a, reg.x, reg.y, reg.s, reg.p, reg.pc, debugCycle);
        debugCycle = 0;
    }
    std::function<void()> &ope = operand[code];
    if (ope)
    {
        ope();
    }
    else
    {
        // Error
        if ((code & 0x9f) == 0x04)
        {
            context.cycle = 3;
            reg.pc += 2;
        }
        else if (code == 0x0c)
        {
            context.cycle = 4;
            reg.pc += 3;
        }
        else if ((code & 0x1f) == 0x14)
        {
            context.cycle = 4;
            reg.pc += 2;
        }
        else if ((code & 0x1f) == 0x1a)
        {
            context.cycle = 2;
            reg.pc++;
        }
        else if (code == 0x80)
        {
            context.cycle = 2;
            reg.pc += 2;
        }
        else if ((code & 0x1f) == 0x1c)
        {
            // NOP absolute,x
            reg.pc++;
            absoluteX.operand();
        }
        else
        {
            context.cycle = 2;
            EM_ASM({ console.log("No Operation: #" + $0.toString(16) + " ope=$" + $1.toString(16)); }, reg.pc, code);
            reg.pc++;
            faultFlag = true;
        }
    }
    if (nextIrq >= 0 && reg.nextIrq >= 0)
    {
        reg.p = (reg.p & ~FLAG_INTERRUPT) | (nextIrq & FLAG_INTERRUPT);
        if (nextIrq == reg.nextIrq)
        {
            reg.nextIrq = -1;
        }
    }
    return context.cycle;
}
extern "C" EMSCRIPTEN_KEEPALIVE void setApuStepCallback(void (*callback)(int))
{
    apuStepCallback = callback;
}

extern "C" EMSCRIPTEN_KEEPALIVE void setMemWriteCallback(void (*callback)(int, int))
{
    memWriteCallback = callback;
}

extern "C" EMSCRIPTEN_KEEPALIVE void setMemReadCallback(int (*callback)(int))
{
    memReadCallback = callback;
}

extern "C" EMSCRIPTEN_KEEPALIVE void setDebugCallback(void (*callback)(int, int, int, int, int, int, int))
{
    debugCallback = callback;
}

extern "C" EMSCRIPTEN_KEEPALIVE char *getOperandText()
{
    return operandResultBuf;
}
extern "C" EMSCRIPTEN_KEEPALIVE int makeOperandText(int addr)
{
    auto &ope = operandText[readMem(addr)];
    if (ope)
    {
        return ope(addr, operandResultBuf);
    }
    strcpy(operandResultBuf, "???");
    return addr + 1;
}

extern "C" EMSCRIPTEN_KEEPALIVE void reset()
{
    reg.s -= 3;
    reg.p |= (FLAG_INTERRUPT | 0x20);
    reg.pc = readMem(0xfffc) | (readMem(0xfffd) << 8);
    reg.irqRequest = 0;
    reg.nmiRequest = 0;
    reg.nextIrq = -1;
    EM_ASM({ console.log("Start: $" + $0.toString(16)); }, reg.pc);
    debugCycle = 7;
    // reg.pc = 0xc000;
}
extern "C" EMSCRIPTEN_KEEPALIVE void powerOff()
{
    EM_ASM({
        console.log("powerOff");
    });
    powerOn = true;
    std::memset(&reg, 0, sizeof(reg));
}

// CPU処理を実行する
extern "C" EMSCRIPTEN_KEEPALIVE int step(int cycles)
{
    if (powerOn)
    {
        powerOn = false;
        reset();
    }
    cycle.cpuCycle = 0;
    while (cycle.cpuCycle < cycles)
    {
        if (faultFlag)
        {
            cycle.cpuCycle = cycles;
            break;
        }
        int add = executeCpu();
        debugCycle += add;
        cycle.cpuCycle += add;
        cycle.notifyCpuCycle += add;
        notifyApuStep();
    }
    return cycle.cpuCycle;
}

// CPUサイクルをスキップする
extern "C" EMSCRIPTEN_KEEPALIVE void skip(int cycles)
{
    cycle.cpuCycle += cycles;
    cycle.notifyCpuCycle += cycles;
    notifyApuStep();
}

// IRQリクエスト
extern "C" EMSCRIPTEN_KEEPALIVE void irq(int flag)
{
    if (debugFlag)
    {
        EM_ASM({ console.log("IRQ:" + $0.toString(16)); }, flag);
    }
    reg.irqRequest = flag & 1;
}

// NMIリクエスト
extern "C" EMSCRIPTEN_KEEPALIVE void nmi()
{
    reg.nmiRequest = 1;
}

// 命令
static CpuOperand LDA("LDA", 0, []()
                      { reg.a = flags(readMem(context.addr), FLAG_NEGATIVE | FLAG_ZERO); },
                      {{0xa9, immediate},
                       {0xa5, zeroPage},
                       {0xb5, zeroPageX},
                       {0xad, absolute},
                       {0xbd, absoluteX},
                       {0xb9, absoluteY},
                       {0xa1, indirectX},
                       {0xb1, indirectY}});

static CpuOperand LDX("LDX", 0, []()
                      { reg.x = flags(readMem(context.addr), FLAG_NEGATIVE | FLAG_ZERO); },
                      {{0xa2, immediate},
                       {0xa6, zeroPage},
                       {0xb6, zeroPageY},
                       {0xae, absolute},
                       {0xbe, absoluteY}});
static CpuOperand LDY("LDY", 0, []()
                      { reg.y = flags(readMem(context.addr), FLAG_NEGATIVE | FLAG_ZERO); },
                      {{0xa0, immediate},
                       {0xa4, zeroPage},
                       {0xb4, zeroPageX},
                       {0xac, absolute},
                       {0xbc, absoluteX}});

static CpuOperand STA("STA", 0, []()
                      { writeMem(context.addr, reg.a); },
                      {{0x85, zeroPage},
                       {0x95, zeroPageX},
                       {0x8d, absolute},
                       {0x9d, absoluteXsta},
                       {0x99, absoluteYsta},
                       {0x81, indirectX},
                       {0x91, indirectYsta}});
static CpuOperand STX("STX", 0, []()
                      { writeMem(context.addr, reg.x); },
                      {{0x86, zeroPage},
                       {0x96, zeroPageY},
                       {0x8e, absolute}});
static CpuOperand STY("STY", 0, []()
                      { writeMem(context.addr, reg.y); },
                      {{0x84, zeroPage},
                       {0x94, zeroPageX},
                       {0x8c, absolute}});

static CpuOperand TAX("TAX", 0xaa, 2, []()
                      { reg.x = flags(reg.a, FLAG_NEGATIVE | FLAG_ZERO); });
static CpuOperand TAY("TAY", 0xa8, 2, []()
                      { reg.y = flags(reg.a, FLAG_NEGATIVE | FLAG_ZERO); });
static CpuOperand TSX("TSX", 0xba, 2, []()
                      { reg.x = flags(reg.s, FLAG_NEGATIVE | FLAG_ZERO); });
static CpuOperand TXA("TXA", 0x8a, 2, []()
                      { reg.a = flags(reg.x, FLAG_NEGATIVE | FLAG_ZERO); });
static CpuOperand TXS("TXS", 0x9a, 2, []()
                      { reg.s = reg.x; });
static CpuOperand TYA("TYA", 0x98, 2, []()
                      { reg.a = flags(reg.y, FLAG_NEGATIVE | FLAG_ZERO); });

static CpuOperand PHA("PHA", 0x48, 3, []()
                      { push(reg.a); });
static CpuOperand PHP("PHP", 0x08, 3, []()
                      { push(reg.p | FLAG_BREAK); });
static CpuOperand PLA("PLA", 0x68, 4, []()
                      { reg.a = flags(pop(), FLAG_NEGATIVE | FLAG_ZERO); });
static CpuOperand PLP("PLP", 0x28, 4, []()
                      {
    int val = pop();
    // IRQは遅延実行
    reg.nextIrq = (val & FLAG_INTERRUPT);
    reg.p = (reg.p & 0x34) | (val & ~0x34); });

static CpuOperand ASL("ASL", 2, []()
                      { context_set(flags(context_get() << 1, FLAG_NEGATIVE | FLAG_ZERO | FLAG_CARRY)); },
                      {{0x0a, accumulator},
                       {0x06, zeroPage},
                       {0x16, zeroPageX},
                       {0x0e, absolute},
                       {0x1e, absoluteXsta}});

static CpuOperand LSR("LSR", 2, []()
                      {
    uint8_t v = context_get();
    context_set(flags((v >> 1) | ((v & 1) << 8), FLAG_NEGATIVE | FLAG_ZERO | FLAG_CARRY)); },
                      {{0x4a, accumulator},
                       {0x46, zeroPage},
                       {0x56, zeroPageX},
                       {0x4e, absolute},
                       {0x5e, absoluteXsta}});

static CpuOperand ROL("ROL", 2, []()
                      { context_set(flags((context_get() << 1) | (reg.p & FLAG_CARRY), FLAG_NEGATIVE | FLAG_ZERO | FLAG_CARRY)); },
                      {{0x2a, accumulator},
                       {0x26, zeroPage},
                       {0x36, zeroPageX},
                       {0x2e, absolute},
                       {0x3e, absoluteXsta}});

static CpuOperand ROR("ROR", 2, []()
                      {
    uint8_t v = context_get();
    context_set(flags((v >> 1) | ((reg.p & FLAG_CARRY) << 7) | ((v & 1) << 8), FLAG_NEGATIVE | FLAG_ZERO | FLAG_CARRY)); },
                      {{0x6a, accumulator},
                       {0x66, zeroPage},
                       {0x76, zeroPageX},
                       {0x6e, absolute},
                       {0x7e, absoluteXsta}});

static CpuOperand AND("AND", 0, []()
                      { reg.a = flags(reg.a & readMem(context.addr), FLAG_NEGATIVE | FLAG_ZERO); },
                      {{0x29, immediate},
                       {0x25, zeroPage},
                       {0x35, zeroPageX},
                       {0x2d, absolute},
                       {0x3d, absoluteX},
                       {0x39, absoluteY},
                       {0x21, indirectX},
                       {0x31, indirectY}});
static CpuOperand EOR("EOR", 0, []()
                      { reg.a = flags(reg.a ^ readMem(context.addr), FLAG_NEGATIVE | FLAG_ZERO); },
                      {{0x49, immediate},
                       {0x45, zeroPage},
                       {0x55, zeroPageX},
                       {0x4d, absolute},
                       {0x5d, absoluteX},
                       {0x59, absoluteY},
                       {0x41, indirectX},
                       {0x51, indirectY}});
static CpuOperand ORA("ORA", 0, []()
                      { reg.a = flags(reg.a | readMem(context.addr), FLAG_NEGATIVE | FLAG_ZERO); },
                      {{0x09, immediate},
                       {0x05, zeroPage},
                       {0x15, zeroPageX},
                       {0x0d, absolute},
                       {0x1d, absoluteX},
                       {0x19, absoluteY},
                       {0x01, indirectX},
                       {0x11, indirectY}});
static CpuOperand BIT("BIT", 0, []()
                      {
    int val = readMem(context.addr);
    reg.p = (reg.p & 0x3d) | (val & 0xc0) | ((reg.a & val) ? 0 : FLAG_ZERO); },
                      {{0x24, zeroPage},
                       {0x2c, absolute}});

static CpuOperand ADC("ADC", 0, []()
                      {
    int val = readMem(context.addr);
    reg.a = flags(reg.a + val + (reg.p & FLAG_CARRY), FLAG_NEGATIVE | FLAG_ZERO | FLAG_OVERFLOW | FLAG_CARRY, reg.a, val); },
                      {{0x69, immediate},
                       {0x65, zeroPage},
                       {0x75, zeroPageX},
                       {0x6d, absolute},
                       {0x7d, absoluteX},
                       {0x79, absoluteY},
                       {0x61, indirectX},
                       {0x71, indirectY}});
static CpuOperand SBC("SBC", 0, []()
                      {
    int val = readMem(context.addr);
    // Carryが0だと -1、1だと0 なので、256 - 1 = 255 を開始とする
    reg.a = flags(255 + reg.a - val + (reg.p & FLAG_CARRY), FLAG_NEGATIVE | FLAG_ZERO | FLAG_OVERFLOW | FLAG_CARRY, reg.a, -val); },
                      {{0xe9, immediate},
                       {0xe5, zeroPage},
                       {0xf5, zeroPageX},
                       {0xed, absolute},
                       {0xfd, absoluteX},
                       {0xf9, absoluteY},
                       {0xe1, indirectX},
                       {0xf1, indirectY}});

static CpuOperand CMP("CMP", 0, []()
                      { flags(256 + reg.a - readMem(context.addr), FLAG_NEGATIVE | FLAG_ZERO | FLAG_CARRY); },
                      {{0xc9, immediate},
                       {0xc5, zeroPage},
                       {0xd5, zeroPageX},
                       {0xcd, absolute},
                       {0xdd, absoluteX},
                       {0xd9, absoluteY},
                       {0xc1, indirectX},
                       {0xd1, indirectY}});

static CpuOperand CPX("CPX", 0, []()
                      { flags(256 + reg.x - readMem(context.addr), FLAG_NEGATIVE | FLAG_ZERO | FLAG_CARRY); },
                      {{0xe0, immediate},
                       {0xe4, zeroPage},
                       {0xec, absolute}});
static CpuOperand CPY("CPY", 0, []()
                      { flags(256 + reg.y - readMem(context.addr), FLAG_NEGATIVE | FLAG_ZERO | FLAG_CARRY); },
                      {{0xc0, immediate},
                       {0xc4, zeroPage},
                       {0xcc, absolute}});

static CpuOperand DEC("DEC", 2, []()
                      { writeMem(context.addr, flags(readMem(context.addr) - 1, FLAG_NEGATIVE | FLAG_ZERO)); },
                      {{0xc6, zeroPage},
                       {0xd6, zeroPageX},
                       {0xce, absolute},
                       {0xde, absoluteXsta}});
static CpuOperand DEX("DEX", 0xca, 2, []()
                      { reg.x = flags(reg.x - 1, FLAG_NEGATIVE | FLAG_ZERO); });
static CpuOperand DEY("DEY", 0x88, 2, []()
                      { reg.y = flags(reg.y - 1, FLAG_NEGATIVE | FLAG_ZERO); });
static CpuOperand INC("INC", 2, []()
                      { writeMem(context.addr, flags(readMem(context.addr) + 1, FLAG_NEGATIVE | FLAG_ZERO)); },
                      {{0xe6, zeroPage},
                       {0xf6, zeroPageX},
                       {0xee, absolute},
                       {0xfe, absoluteXsta}});
static CpuOperand INX("INX", 0xe8, 2, []()
                      { reg.x = flags(reg.x + 1, FLAG_NEGATIVE | FLAG_ZERO); });
static CpuOperand INY("INY", 0xc8, 2, []()
                      { reg.y = flags(reg.y + 1, FLAG_NEGATIVE | FLAG_ZERO); });

static CpuOperand BRK("BRK", 0, 7, []()
                      {
    reg.pc++;
    push(reg.pc >> 8);
    push(reg.pc);
    // reg.p |= FLAG_BREAK;
    push(reg.p | FLAG_BREAK);
    reg.p |= FLAG_INTERRUPT;
    // reg.p &= ~FLAG_BREAK;
    reg.nextIrq = -1;
    reg.pc = readMem(0xfffe) | (readMem(0xffff) << 8); });

static CpuOperand JMP("JMP", -1, []()
                      { reg.pc = context.addr; },
                      {{0x4c, absolute},
                       {0x6c, indirect}});
static CpuOperand JSR("JSR", 2, []()
                      {
    reg.pc--;
    push(reg.pc >> 8);
    push(reg.pc);
    reg.pc = context.addr; },
                      {{0x20, absolute}});
static CpuOperand RTS("RTS", 0x60, 6, []()
                      {
    reg.pc = pop();
    reg.pc |= pop() << 8;
    reg.pc++; });
static CpuOperand RTI("RTI", 0x40, 6, []()
                      {
    reg.p = (reg.p & 0x30) | (pop() & 0xcf);
    reg.nextIrq = -1;
    reg.pc = pop();
    reg.pc |= pop() << 8; });

static CpuOperand BCC("BCC", 0x90, 2, relative([]
                                               { return !(reg.p & FLAG_CARRY); }));
static CpuOperand BCS("BCS", 0xb0, 2, relative([]()
                                               { return reg.p & FLAG_CARRY; }));
static CpuOperand BEQ("BEQ", 0xf0, 2, relative([]()
                                               { return reg.p & FLAG_ZERO; }));
static CpuOperand BMI("BMI", 0x30, 2, relative([]()
                                               { return reg.p & FLAG_NEGATIVE; }));
static CpuOperand BNE("BNE", 0xd0, 2, relative([]()
                                               { return !(reg.p & FLAG_ZERO); }));
static CpuOperand BPL("BPL", 0x10, 2, relative([]()
                                               { return !(reg.p & FLAG_NEGATIVE); }));
static CpuOperand BVC("BVC", 0x50, 2, relative([]()
                                               { return !(reg.p & FLAG_OVERFLOW); }));
static CpuOperand BVS("BVS", 0x70, 2, relative([]()
                                               { return reg.p & FLAG_OVERFLOW; }));

static CpuOperand CLC("CLC", 0x18, 2, []()
                      { reg.p &= ~FLAG_CARRY; });
static CpuOperand CLD("CLD", 0xd8, 2, []()
                      { reg.p &= ~FLAG_DECIMAL; });
static CpuOperand CLI("CLI", 0x58, 2, []()
                      { reg.nextIrq = 0; });
static CpuOperand CLV("CLV", 0xb8, 2, []()
                      { reg.p &= ~FLAG_OVERFLOW; });
static CpuOperand SEC("SEC", 0x38, 2, []()
                      { reg.p |= FLAG_CARRY; });
static CpuOperand SED("SED", 0xf8, 2, []()
                      { reg.p |= FLAG_DECIMAL; });
static CpuOperand SEI("SEI", 0x78, 2, []()
                      { reg.nextIrq = FLAG_INTERRUPT; });

static CpuOperand NOP("NOP", 0xea, 2, []() {});

// 非公式の命令
static CpuOperand LAX("*LAX", 0, []()
                      {
    reg.a = flags(readMem(context.addr), FLAG_NEGATIVE | FLAG_ZERO);
    reg.x = reg.a; },
                      {{0xa7, zeroPage}, {0xaf, absolute}, {0xb7, zeroPageY}, {0xbf, absoluteY}, {0xa3, indirectX}, {0xb3, indirectY}});

static CpuOperand SAX("*SAX", 0, []()
                      { writeMem(context.addr, reg.x & reg.a); },
                      {{0x87, zeroPage},
                       {0x97, zeroPageY},
                       {0x8f, absolute},
                       {0x83, indirectX}});

static CpuOperand SBC_NO("*SBC", 0, []()
                         {
    int val = readMem(context.addr);
    // Carryが0だと -1、1だと0 なので、256 - 1 = 255 を開始とする
    reg.a = flags(255 + reg.a - val + (reg.p & FLAG_CARRY), FLAG_NEGATIVE | FLAG_ZERO | FLAG_OVERFLOW | FLAG_CARRY, reg.a, -val); },
                         {{0xeb, immediate}});

static CpuOperand DCP("*DCP", 2, []()
                      {
    uint8_t val = readMem(context.addr) - 1;
    writeMem(context.addr, val);
    flags(256 + reg.a - val, FLAG_NEGATIVE | FLAG_ZERO | FLAG_CARRY); },
                      {{0xc7, zeroPage},
                       {0xd7, zeroPageX},
                       {0xcf, absolute},
                       {0xdf, absoluteX},
                       {0xdb, absoluteY},
                       {0xc3, indirectX},
                       {0xd3, indirectY}});

static CpuOperand ISB("*ISB", 2, []()
                      {
    uint8_t val = readMem(context.addr) + 1;
    writeMem(context.addr, val);
    // Carryが0だと -1、1だと0 なので、256 - 1 = 255 を開始とする
    reg.a = flags(255 + reg.a - val + (reg.p & FLAG_CARRY), FLAG_NEGATIVE | FLAG_ZERO | FLAG_OVERFLOW | FLAG_CARRY, reg.a, -val); },
                      {{0xe7, zeroPage},
                       {0xf7, zeroPageX},
                       {0xef, absolute},
                       {0xff, absoluteX},
                       {0xfb, absoluteY},
                       {0xe3, indirectX},
                       {0xf3, indirectY}});

static CpuOperand SLO("*SLO", 2, []()
                      {
    uint8_t val = flags(readMem(context.addr) << 1, FLAG_CARRY);
    writeMem(context.addr, val);
    reg.a = flags(reg.a | val, FLAG_NEGATIVE | FLAG_ZERO); },
                      {{0x07, zeroPage},
                       {0x17, zeroPageX},
                       {0x0f, absolute},
                       {0x1f, absoluteX},
                       {0x1b, absoluteY},
                       {0x03, indirectX},
                       {0x13, indirectY}});

static CpuOperand RLA("*RLA", 2, []()
                      {
    uint8_t val = flags((readMem(context.addr) << 1) | (reg.p & FLAG_CARRY), FLAG_CARRY);
    writeMem(context.addr, val);
    reg.a = flags(reg.a & val, FLAG_NEGATIVE | FLAG_ZERO); },
                      {{0x27, zeroPage},
                       {0x37, zeroPageX},
                       {0x2f, absolute},
                       {0x3f, absoluteX},
                       {0x3b, absoluteY},
                       {0x23, indirectX},
                       {0x33, indirectY}});

static CpuOperand SRE("*SRE", 2, []()
                      {
    int m = readMem(context.addr);
    uint8_t val = flags((m >> 1) | ((m & 1) << 8), FLAG_CARRY);
    writeMem(context.addr, val);
    reg.a = flags(reg.a ^ val, FLAG_NEGATIVE | FLAG_ZERO); },
                      {{0x47, zeroPage},
                       {0x57, zeroPageX},
                       {0x4f, absolute},
                       {0x5f, absoluteX},
                       {0x5b, absoluteY},
                       {0x43, indirectX},
                       {0x53, indirectY}});

static CpuOperand RRA("*RRA", 2, []()
                      {
    int m = readMem(context.addr);
    uint8_t val = flags((m >> 1) | ((m & 1) << 8) | ((reg.p & 1) << 7), FLAG_CARRY);
    writeMem(context.addr, val);
    reg.a = flags(reg.a + val + (reg.p & FLAG_CARRY), FLAG_NEGATIVE | FLAG_ZERO | FLAG_OVERFLOW | FLAG_CARRY, reg.a, val); },
                      {{0x67, zeroPage},
                       {0x77, zeroPageX},
                       {0x6f, absolute},
                       {0x7f, absoluteX},
                       {0x7b, absoluteY},
                       {0x63, indirectX},
                       {0x73, indirectY}});