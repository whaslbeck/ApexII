#include "apeximgui_core.h"
#include <cstdio>
#include <cstring>
#include <strings.h>

static const CpuHelpInfo cpu6809_help[] = {
    {"ABX", "Add B to X (unsigned)", "None", "3"},
    {"ADCA", "Add with Carry to A", "H,N,Z,V,C", "2"},
    {"ADCB", "Add with Carry to B", "H,N,Z,V,C", "2"},
    {"ADDA", "Add to A", "H,N,Z,V,C", "2"},
    {"ADDB", "Add to B", "H,N,Z,V,C", "2"},
    {"ADDD", "Add to D (A:B)", "N,Z,V,C", "4"},
    {"ANDA", "Logical AND A", "N,Z,V=0", "2"},
    {"ANDB", "Logical AND B", "N,Z,V=0", "2"},
    {"ANDCC", "Logical AND Condition Code Register", "All", "3"},
    {"ASL", "Arithmetic Shift Left", "N,Z,V,C", "6"},
    {"ASLA", "Arithmetic Shift Left A", "N,Z,V,C", "2"},
    {"ASLB", "Arithmetic Shift Left B", "N,Z,V,C", "2"},
    {"ASR", "Arithmetic Shift Right", "N,Z,C", "6"},
    {"ASRA", "Arithmetic Shift Right A", "N,Z,C", "2"},
    {"ASRB", "Arithmetic Shift Right B", "N,Z,C", "2"},
    {"BCC", "Branch if Carry Clear (C=0)", "None", "3"},
    {"BCS", "Branch if Carry Set (C=1)", "None", "3"},
    {"BEQ", "Branch if Equal (Z=1)", "None", "3"},
    {"BGE", "Branch if Greater or Equal (signed)", "None", "3"},
    {"BGT", "Branch if Greater (signed)", "None", "3"},
    {"BHI", "Branch if Higher (unsigned)", "None", "3"},
    {"BHS", "Branch if Higher or Same (unsigned)", "None", "3"},
    {"BITA", "Bit Test A", "N,Z,V=0", "2"},
    {"BITB", "Bit Test B", "N,Z,V=0", "2"},
    {"BLE", "Branch if Less or Equal (signed)", "None", "3"},
    {"BLO", "Branch if Lower (unsigned)", "None", "3"},
    {"BLS", "Branch if Lower or Same (unsigned)", "None", "3"},
    {"BLT", "Branch if Less (signed)", "None", "3"},
    {"BMI", "Branch if Minus (N=1)", "None", "3"},
    {"BNE", "Branch if Not Equal (Z=0)", "None", "3"},
    {"BPL", "Branch if Plus (N=0)", "None", "3"},
    {"BRA", "Branch Always", "None", "3"},
    {"BRN", "Branch Never", "None", "3"},
    {"BSR", "Branch to Subroutine", "None", "7"},
    {"BVC", "Branch if Overflow Clear (V=0)", "None", "3"},
    {"BVS", "Branch if Overflow Set (V=1)", "None", "3"},
    {"CLR", "Clear (to 0)", "N=0,Z=1,V=0,C=0", "6"},
    {"CLRA", "Clear A", "N=0,Z=1,V=0,C=0", "2"},
    {"CLRB", "Clear B", "N=0,Z=1,V=0,C=0", "2"},
    {"CMPA", "Compare A", "N,Z,V,C", "2"},
    {"CMPB", "Compare B", "N,Z,V,C", "2"},
    {"CMPD", "Compare D (16-bit)", "N,Z,V,C", "5"},
    {"CMPS", "Compare S (Stack Pointer)", "N,Z,V,C", "5"},
    {"CMPU", "Compare U (User Pointer)", "N,Z,V,C", "5"},
    {"CMPX", "Compare X (Index Register)", "N,Z,V,C", "4"},
    {"CMPY", "Compare Y (Index Register)", "N,Z,V,C", "5"},
    {"COM", "Complement (Logical NOT)", "N,Z,V=0,C=1", "6"},
    {"COMA", "Complement A", "N,Z,V=0,C=1", "2"},
    {"COMB", "Complement B", "N,Z,V=0,C=1", "2"},
    {"CWAI", "Clear CC bits and Wait for Interrupt", "All", "20"},
    {"DAA", "Decimal Adjust A", "N,Z,C", "2"},
    {"DEC", "Decrement", "N,Z,V", "6"},
    {"DECA", "Decrement A", "N,Z,V", "2"},
    {"DECB", "Decrement B", "N,Z,V", "2"},
    {"EORA", "Exclusive OR A", "N,Z,V=0", "2"},
    {"EORB", "Exclusive OR B", "N,Z,V=0", "2"},
    {"EXG", "Exchange Registers", "None", "8"},
    {"INC", "Increment", "N,Z,V", "6"},
    {"INCA", "Increment A", "N,Z,V", "2"},
    {"INCB", "Increment B", "N,Z,V", "2"},
    {"JMP", "Jump Always", "None", "3"},
    {"JSR", "Jump to Subroutine", "None", "9"},
    {"LDA", "Load A", "N,Z,V=0", "2"},
    {"LDB", "Load B", "N,Z,V=0", "2"},
    {"LDD", "Load D (16-bit)", "N,Z,V=0", "4"},
    {"LDS", "Load S (Stack Pointer)", "N,Z,V=0", "4"},
    {"LDU", "Load U (User Pointer)", "N,Z,V=0", "4"},
    {"LDX", "Load X (Index Register)", "N,Z,V=0", "3"},
    {"LDY", "Load Y (Index Register)", "N,Z,V=0", "4"},
    {"LEAS", "Load Effective Address into S", "None", "4"},
    {"LEAU", "Load Effective Address into U", "None", "4"},
    {"LEAX", "Load Effective Address into X", "Z", "4"},
    {"LEAY", "Load Effective Address into Y", "Z", "4"},
    {"LSL", "Logical Shift Left", "N,Z,V,C", "6"},
    {"LSLA", "Logical Shift Left A", "N,Z,V,C", "2"},
    {"LSLB", "Logical Shift Left B", "N,Z,V,C", "2"},
    {"LSR", "Logical Shift Right", "Z,C,N=0", "6"},
    {"LSRA", "Logical Shift Right A", "Z,C,N=0", "2"},
    {"LSRB", "Logical Shift Right B", "Z,C,N=0", "2"},
    {"MUL", "Multiply unsigned (A * B -> D)", "Z", "11"},
    {"NEG", "Negate (Two's Complement)", "N,Z,V,C", "6"},
    {"NEGA", "Negate A", "N,Z,V,C", "2"},
    {"NEGB", "Negate B", "N,Z,V,C", "2"},
    {"NOP", "No Operation", "None", "2"},
    {"ORA", "Logical OR A", "N,Z,V=0", "2"},
    {"ORB", "Logical OR B", "N,Z,V=0", "2"},
    {"ORCC", "Logical OR Condition Code Register", "All", "3"},
    {"PSHS", "Push to System Stack", "None", "5+"},
    {"PSHU", "Push to User Stack", "None", "5+"},
    {"PULS", "Pull from System Stack", "All", "5+"},
    {"PULU", "Pull from User Stack", "All", "5+"},
    {"ROL", "Rotate Left (through Carry)", "N,Z,V,C", "6"},
    {"ROLA", "Rotate Left A", "N,Z,V,C", "2"},
    {"ROLB", "Rotate Left B", "N,Z,V,C", "2"},
    {"ROR", "Rotate Right (through Carry)", "N,Z,V,C", "6"},
    {"RORA", "Rotate Right A", "N,Z,V,C", "2"},
    {"RORB", "Rotate Right B", "N,Z,V,C", "2"},
    {"RTI", "Return from Interrupt", "All", "15"},
    {"RTS", "Return from Subroutine", "None", "5"},
    {"SBCA", "Subtract with Carry from A", "N,Z,V,C", "2"},
    {"SBCB", "Subtract with Carry from B", "N,Z,V,C", "2"},
    {"SEX", "Sign Extend B into A", "N,Z", "2"},
    {"STA", "Store A", "N,Z,V=0", "2"},
    {"STB", "Store B", "N,Z,V=0", "2"},
    {"STD", "Store D (16-bit)", "N,Z,V=0", "4"},
    {"STS", "Store S (Stack Pointer)", "N,Z,V=0", "4"},
    {"STU", "Store U (User Pointer)", "N,Z,V=0", "4"},
    {"STX", "Store X (Index Register)", "N,Z,V=0", "3"},
    {"STY", "Store Y (Index Register)", "N,Z,V=0", "4"},
    {"SUBA", "Subtract from A", "N,Z,V,C", "2"},
    {"SUBB", "Subtract from B", "N,Z,V,C", "2"},
    {"SUBD", "Subtract from D (16-bit)", "N,Z,V,C", "5"},
    {"SWI", "Software Interrupt", "All", "19"},
    {"SWI2", "Software Interrupt 2", "None", "20"},
    {"SWI3", "Software Interrupt 3", "None", "20"},
    {"SYNC", "Synchronize with Interrupt", "None", "4"},
    {"TFR", "Transfer Register to Register", "None", "6"},
    {"TST", "Test for Zero or Minus", "N,Z,V=0", "6"},
    {"TSTA", "Test A", "N,Z,V=0", "2"},
    {"TSTB", "Test B", "N,Z,V=0", "2"},
};

const CpuHelpInfo *lookup_cpu_help(const char *mnemonic)
{
    for (size_t i = 0; i < sizeof(cpu6809_help) / sizeof(cpu6809_help[0]); i++) {
        if (strcasecmp(cpu6809_help[i].mnemonic, mnemonic) == 0) {
            return &cpu6809_help[i];
        }
    }
    return NULL;
}

static const HardwareRegister wpc_hardware[] = {
    {0x3FBC, "WPC_DMD_CTRL",   "DMD Control Register"},
    {0x3FBD, "WPC_DMD_PAGE",   "DMD Page Selection"},
    {0x3FBE, "WPC_DMD_DATA",   "DMD Data Access (Window)"},
    {0x3FD0, "WPC_DIP_SW",     "DIP Switch / Identification"},
    {0x3FD1, "WPC_EXT_MEM",    "Extended Memory Mapping"},
    {0x3FDC, "WPC_LAMP_COL",   "Lamp Matrix Column Output"},
    {0x3FDD, "WPC_LAMP_ROW",   "Lamp Matrix Row Output"},
    {0x3FDE, "WPC_SW_COL",     "Switch Matrix Column Drive"},
    {0x3FDF, "WPC_SW_ROW",     "Switch Matrix Row Read"},
    {0x3FE0, "WPC_SOL_DRV1",   "Solenoid Drive 1-8"},
    {0x3FE1, "WPC_SOL_DRV2",   "Solenoid Drive 9-16"},
    {0x3FE2, "WPC_SOL_DRV3",   "Solenoid Drive 17-24 (Fliptronic)"},
    {0x3FE3, "WPC_SOL_DRV4",   "Solenoid Drive 25-28"},
    {0x3FE4, "WPC_SOL_CON",    "Solenoid Control Register"},
    {0x3FE5, "WPC_FLIP_SW",    "Fliptronic Switch Read"},
    {0x3FE7, "WPC_WATCHDOG",   "ASIC Watchdog / Reset"},
    {0x3FE8, "WPC_EXT_P1",     "Extension Port 1"},
    {0x3FE9, "WPC_EXT_P2",     "Extension Port 2"},
    {0x3FF0, "WPC_SOUND_DATA", "Sound Board Data Interface"},
    {0x3FF1, "WPC_SOUND_CTRL", "Sound Board Control Interface"},
    {0x3FF2, "WPC_GI_CTRL",    "General Illumination Control"},
    {0x3FF3, "WPC_RTC_SEC",    "RTC Seconds"},
    {0x3FF4, "WPC_RTC_MIN",    "RTC Minutes"},
    {0x3FF5, "WPC_RTC_HOUR",   "RTC Hours"},
    {0x3FFC, "WPC_PIC_CTRL",   "PIC Control Register (Security)"},
    {0x3FFD, "WPC_PIC_DATA",   "PIC Data Register (Security)"},
};

const HardwareRegister *lookup_hardware(uint32_t addr)
{
    for (size_t i = 0; i < sizeof(wpc_hardware) / sizeof(wpc_hardware[0]); i++) {
        if (wpc_hardware[i].addr == addr) {
            return &wpc_hardware[i];
        }
    }
    return NULL;
}

std::vector<const HardwareRegister*> find_hardware_in_text(const char *text, size_t length)
{
    std::vector<const HardwareRegister*> found;
    for (size_t i = 0; i < length; i++) {
        if (text[i] == '$' && i + 4 < length) {
            unsigned val;
            if (std::sscanf(text + i + 1, "%4x", &val) == 1) {
                const HardwareRegister *reg = lookup_hardware(val);
                if (reg) {
                    bool exists = false;
                    for (auto f : found) if (f->addr == reg->addr) exists = true;
                    if (!exists) found.push_back(reg);
                }
            }
        }
    }
    return found;
}

size_t hardware_register_count()
{
    return sizeof(wpc_hardware) / sizeof(wpc_hardware[0]);
}

const HardwareRegister *get_hardware_register(size_t index)
{
    if (index >= hardware_register_count()) return NULL;
    return &wpc_hardware[index];
}
