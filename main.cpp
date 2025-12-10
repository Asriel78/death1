#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <map>
#include <algorithm>
#include <cmath>

// ============================================================================
// CACHE CONFIGURATION (Variant 1)
// ============================================================================
const uint32_t MEMORY_SIZE = 128 * 1024;      // 128 KBytes
const uint32_t ADDRESS_LEN = 17;              // 17 bits
const uint32_t CACHE_TAG_LEN = 7;             // 7 bits (calculated: 17 - 4 - 6)
const uint32_t CACHE_INDEX_LEN = 4;           // 4 bits
const uint32_t CACHE_OFFSET_LEN = 6;          // 6 bits (calculated: log2(64))
const uint32_t CACHE_SIZE = 4 * 1024;         // 4 KBytes (calculated: 64 lines * 64 bytes)
const uint32_t CACHE_LINE_SIZE = 64;          // 64 bytes
const uint32_t CACHE_LINE_COUNT = 64;         // 64 lines
const uint32_t CACHE_SET_COUNT = 16;          // 16 sets (calculated: 2^4)
const uint32_t CACHE_WAY = 4;                 // 4-way associative (calculated: 64/16)

// ============================================================================
// MEMORY & REGISTERS
// ============================================================================
class Memory {
public:
    std::map<uint32_t, uint8_t> data;
    
    uint8_t read8(uint32_t addr) {
        return data[addr];
    }
    
    uint16_t read16(uint32_t addr) {
        return read8(addr) | (read8(addr + 1) << 8);
    }
    
    uint32_t read32(uint32_t addr) {
        return read8(addr) | (read8(addr + 1) << 8) | 
               (read8(addr + 2) << 16) | (read8(addr + 3) << 24);
    }
    
    void write8(uint32_t addr, uint8_t val) {
        data[addr] = val;
    }
    
    void write16(uint32_t addr, uint16_t val) {
        write8(addr, val & 0xFF);
        write8(addr + 1, (val >> 8) & 0xFF);
    }
    
    void write32(uint32_t addr, uint32_t val) {
        write8(addr, val & 0xFF);
        write8(addr + 1, (val >> 8) & 0xFF);
        write8(addr + 2, (val >> 16) & 0xFF);
        write8(addr + 3, (val >> 24) & 0xFF);
    }
};

// ============================================================================
// CACHE LINE
// ============================================================================
struct CacheLine {
    bool valid = false;
    uint32_t tag = 0;
    uint8_t data[CACHE_LINE_SIZE];
    bool dirty = false;
    uint32_t lru_counter = 0;  // For LRU
};

// ============================================================================
// CACHE (LRU and bit-pLRU)
// ============================================================================
class Cache {
public:
    CacheLine sets[CACHE_SET_COUNT][CACHE_WAY];
    uint32_t global_counter = 0;
    
    // bit-pLRU: binary tree bits for each set
    // For 4-way: need 3 bits per set (tree structure: root, left child, right child)
    uint8_t plru_bits[CACHE_SET_COUNT]; // 3 bits used per set (16 sets for variant 1)
    
    // Statistics
    uint64_t instr_access = 0, instr_hit = 0;
    uint64_t data_access = 0, data_hit = 0;
    
    Memory* memory;
    
    Cache(Memory* mem) : memory(mem) {
        memset(plru_bits, 0, sizeof(plru_bits));
    }
    
    uint32_t get_tag(uint32_t addr) {
        return (addr >> (CACHE_INDEX_LEN + CACHE_OFFSET_LEN)) & ((1 << CACHE_TAG_LEN) - 1);
    }
    
    uint32_t get_index(uint32_t addr) {
        return (addr >> CACHE_OFFSET_LEN) & ((1 << CACHE_INDEX_LEN) - 1);
    }
    
    uint32_t get_offset(uint32_t addr) {
        return addr & ((1 << CACHE_OFFSET_LEN) - 1);
    }
    
    uint32_t get_block_addr(uint32_t addr) {
        return addr & ~((1 << CACHE_OFFSET_LEN) - 1);
    }
    
    // Load cache line from memory
    void load_line(uint32_t set_idx, uint32_t way_idx, uint32_t addr) {
        uint32_t block_addr = get_block_addr(addr);
        CacheLine& line = sets[set_idx][way_idx];
        
        // Write back if dirty
        if (line.valid && line.dirty) {
            uint32_t old_addr = (line.tag << (CACHE_INDEX_LEN + CACHE_OFFSET_LEN)) | 
                                (set_idx << CACHE_OFFSET_LEN);
            for (uint32_t i = 0; i < CACHE_LINE_SIZE; i++) {
                memory->write8(old_addr + i, line.data[i]);
            }
        }
        
        // Load new line
        line.valid = true;
        line.tag = get_tag(addr);
        line.dirty = false;
        for (uint32_t i = 0; i < CACHE_LINE_SIZE; i++) {
            line.data[i] = memory->read8(block_addr + i);
        }
    }
    
    // LRU: find victim
    uint32_t find_lru_victim(uint32_t set_idx) {
        uint32_t victim = 0;
        uint32_t min_counter = sets[set_idx][0].lru_counter;
        
        for (uint32_t i = 1; i < CACHE_WAY; i++) {
            if (!sets[set_idx][i].valid) return i;
            if (sets[set_idx][i].lru_counter < min_counter) {
                min_counter = sets[set_idx][i].lru_counter;
                victim = i;
            }
        }
        return victim;
    }
    
    // bit-pLRU: find victim using tree bits
    uint32_t find_plru_victim(uint32_t set_idx) {
        // For 4-way: bit0 = root, bit1 = left subtree, bit2 = right subtree
        // Tree structure:
        //        bit0
        //       /    \
        //    bit1    bit2
        //    / \      / \
        //   w0 w1    w2 w3
        
        uint8_t bits = plru_bits[set_idx];
        uint32_t way;
        
        if ((bits & 0x1) == 0) {  // bit0 = 0, go left
            way = (bits & 0x2) ? 1 : 0;  // bit1
        } else {  // bit0 = 1, go right
            way = (bits & 0x4) ? 3 : 2;  // bit2
        }
        
        // Check if any line is invalid first
        for (uint32_t i = 0; i < CACHE_WAY; i++) {
            if (!sets[set_idx][i].valid) return i;
        }
        
        return way;
    }
    
    // Update bit-pLRU on access
    void update_plru(uint32_t set_idx, uint32_t way) {
        uint8_t& bits = plru_bits[set_idx];
        
        // Update tree bits based on accessed way
        if (way == 0 || way == 1) {
            bits |= 0x1;   // Set bit0 to 1 (point away from left)
            if (way == 0) bits |= 0x2;   // Set bit1
            else bits &= ~0x2;           // Clear bit1
        } else {
            bits &= ~0x1;  // Clear bit0 (point away from right)
            if (way == 2) bits |= 0x4;   // Set bit2
            else bits &= ~0x4;           // Clear bit2
        }
    }
    
    // Access cache (unified for instruction and data)
    uint32_t access(uint32_t addr, bool is_write, uint32_t write_data, 
                    uint32_t size, bool is_instruction, bool use_lru) {
        uint32_t tag = get_tag(addr);
        uint32_t set_idx = get_index(addr);
        uint32_t offset = get_offset(addr);
        
        // Update statistics
        if (is_instruction) {
            instr_access++;
        } else {
            data_access++;
        }
        
        // Check for hit
        int hit_way = -1;
        for (uint32_t i = 0; i < CACHE_WAY; i++) {
            if (sets[set_idx][i].valid && sets[set_idx][i].tag == tag) {
                hit_way = i;
                break;
            }
        }
        
        if (hit_way != -1) {
            // Hit!
            if (is_instruction) {
                instr_hit++;
            } else {
                data_hit++;
            }
            
            // Update LRU/pLRU
            if (use_lru) {
                sets[set_idx][hit_way].lru_counter = ++global_counter;
            } else {
                update_plru(set_idx, hit_way);
            }
            
            // Handle write
            if (is_write) {
                sets[set_idx][hit_way].dirty = true;
                if (size == 1) sets[set_idx][hit_way].data[offset] = write_data & 0xFF;
                else if (size == 2) {
                    sets[set_idx][hit_way].data[offset] = write_data & 0xFF;
                    sets[set_idx][hit_way].data[offset + 1] = (write_data >> 8) & 0xFF;
                } else if (size == 4) {
                    for (int i = 0; i < 4; i++) {
                        sets[set_idx][hit_way].data[offset + i] = (write_data >> (i * 8)) & 0xFF;
                    }
                }
            }
            
            // Read data
            uint32_t result = 0;
            if (size == 1) result = sets[set_idx][hit_way].data[offset];
            else if (size == 2) result = sets[set_idx][hit_way].data[offset] | 
                                         (sets[set_idx][hit_way].data[offset + 1] << 8);
            else if (size == 4) {
                for (int i = 0; i < 4; i++) {
                    result |= (sets[set_idx][hit_way].data[offset + i] << (i * 8));
                }
            }
            return result;
        } else {
            // Miss - find victim and load
            uint32_t victim = use_lru ? find_lru_victim(set_idx) : find_plru_victim(set_idx);
            load_line(set_idx, victim, addr);
            
            // Update LRU/pLRU
            if (use_lru) {
                sets[set_idx][victim].lru_counter = ++global_counter;
            } else {
                update_plru(set_idx, victim);
            }
            
            // Handle write (write-allocate)
            if (is_write) {
                sets[set_idx][victim].dirty = true;
                if (size == 1) sets[set_idx][victim].data[offset] = write_data & 0xFF;
                else if (size == 2) {
                    sets[set_idx][victim].data[offset] = write_data & 0xFF;
                    sets[set_idx][victim].data[offset + 1] = (write_data >> 8) & 0xFF;
                } else if (size == 4) {
                    for (int i = 0; i < 4; i++) {
                        sets[set_idx][victim].data[offset + i] = (write_data >> (i * 8)) & 0xFF;
                    }
                }
            }
            
            // Read data
            uint32_t result = 0;
            if (size == 1) result = sets[set_idx][victim].data[offset];
            else if (size == 2) result = sets[set_idx][victim].data[offset] | 
                                        (sets[set_idx][victim].data[offset + 1] << 8);
            else if (size == 4) {
                for (int i = 0; i < 4; i++) {
                    result |= (sets[set_idx][victim].data[offset + i] << (i * 8));
                }
            }
            return result;
        }
    }
    
    void flush() {
        for (uint32_t s = 0; s < CACHE_SET_COUNT; s++) {
            for (uint32_t w = 0; w < CACHE_WAY; w++) {
                if (sets[s][w].valid && sets[s][w].dirty) {
                    uint32_t addr = (sets[s][w].tag << (CACHE_INDEX_LEN + CACHE_OFFSET_LEN)) | 
                                   (s << CACHE_OFFSET_LEN);
                    for (uint32_t i = 0; i < CACHE_LINE_SIZE; i++) {
                        memory->write8(addr + i, sets[s][w].data[i]);
                    }
                }
            }
        }
    }
};

// ============================================================================
// RISC-V EMULATOR
// ============================================================================
class RiscVEmulator {
public:
    uint32_t regs[32];
    uint32_t pc;
    Memory memory;
    Cache* cache;
    uint32_t initial_ra;
    bool use_lru;
    
    RiscVEmulator(bool lru) : use_lru(lru) {
        memset(regs, 0, sizeof(regs));
        pc = 0;
        cache = new Cache(&memory);
    }
    
    ~RiscVEmulator() {
        delete cache;
    }
    
    int32_t sign_extend(uint32_t val, int bits) {
        if (val & (1 << (bits - 1))) {
            return val | (~((1 << bits) - 1));
        }
        return val;
    }
    
    uint32_t fetch() {
        uint32_t instr = cache->access(pc, false, 0, 4, true, use_lru);
        return instr;
    }
    
    void execute(uint32_t instr) {
        uint32_t opcode = instr & 0x7F;
        uint32_t rd = (instr >> 7) & 0x1F;
        uint32_t funct3 = (instr >> 12) & 0x7;
        uint32_t rs1 = (instr >> 15) & 0x1F;
        uint32_t rs2 = (instr >> 20) & 0x1F;
        uint32_t funct7 = (instr >> 25) & 0x7F;
        
        regs[0] = 0; // x0 always 0
        
        switch (opcode) {
            case 0x33: { // R-type (ADD, SUB, MUL, etc.)
                if (funct7 == 0x00 && funct3 == 0x0) { // ADD
                    regs[rd] = regs[rs1] + regs[rs2];
                } else if (funct7 == 0x20 && funct3 == 0x0) { // SUB
                    regs[rd] = regs[rs1] - regs[rs2];
                } else if (funct7 == 0x00 && funct3 == 0x4) { // XOR
                    regs[rd] = regs[rs1] ^ regs[rs2];
                } else if (funct7 == 0x00 && funct3 == 0x6) { // OR
                    regs[rd] = regs[rs1] | regs[rs2];
                } else if (funct7 == 0x00 && funct3 == 0x7) { // AND
                    regs[rd] = regs[rs1] & regs[rs2];
                } else if (funct7 == 0x00 && funct3 == 0x1) { // SLL
                    regs[rd] = regs[rs1] << (regs[rs2] & 0x1F);
                } else if (funct7 == 0x00 && funct3 == 0x5) { // SRL
                    regs[rd] = regs[rs1] >> (regs[rs2] & 0x1F);
                } else if (funct7 == 0x20 && funct3 == 0x5) { // SRA
                    regs[rd] = ((int32_t)regs[rs1]) >> (regs[rs2] & 0x1F);
                } else if (funct7 == 0x00 && funct3 == 0x2) { // SLT
                    regs[rd] = ((int32_t)regs[rs1] < (int32_t)regs[rs2]) ? 1 : 0;
                } else if (funct7 == 0x00 && funct3 == 0x3) { // SLTU
                    regs[rd] = (regs[rs1] < regs[rs2]) ? 1 : 0;
                }
                // RV32M
                else if (funct7 == 0x01 && funct3 == 0x0) { // MUL
                    regs[rd] = regs[rs1] * regs[rs2];
                } else if (funct7 == 0x01 && funct3 == 0x1) { // MULH
                    int64_t result = (int64_t)(int32_t)regs[rs1] * (int64_t)(int32_t)regs[rs2];
                    regs[rd] = result >> 32;
                } else if (funct7 == 0x01 && funct3 == 0x2) { // MULHSU
                    int64_t result = (int64_t)(int32_t)regs[rs1] * (uint64_t)regs[rs2];
                    regs[rd] = result >> 32;
                } else if (funct7 == 0x01 && funct3 == 0x3) { // MULHU
                    uint64_t result = (uint64_t)regs[rs1] * (uint64_t)regs[rs2];
                    regs[rd] = result >> 32;
                } else if (funct7 == 0x01 && funct3 == 0x4) { // DIV
                    if (regs[rs2] == 0) regs[rd] = -1;
                    else regs[rd] = (int32_t)regs[rs1] / (int32_t)regs[rs2];
                } else if (funct7 == 0x01 && funct3 == 0x5) { // DIVU
                    if (regs[rs2] == 0) regs[rd] = 0xFFFFFFFF;
                    else regs[rd] = regs[rs1] / regs[rs2];
                } else if (funct7 == 0x01 && funct3 == 0x6) { // REM
                    if (regs[rs2] == 0) regs[rd] = regs[rs1];
                    else regs[rd] = (int32_t)regs[rs1] % (int32_t)regs[rs2];
                } else if (funct7 == 0x01 && funct3 == 0x7) { // REMU
                    if (regs[rs2] == 0) regs[rd] = regs[rs1];
                    else regs[rd] = regs[rs1] % regs[rs2];
                }
                pc += 4;
                break;
            }
            case 0x13: { // I-type (ADDI, SLTI, etc.)
                int32_t imm = sign_extend((instr >> 20) & 0xFFF, 12);
                if (funct3 == 0x0) { // ADDI
                    regs[rd] = regs[rs1] + imm;
                } else if (funct3 == 0x4) { // XORI
                    regs[rd] = regs[rs1] ^ imm;
                } else if (funct3 == 0x6) { // ORI
                    regs[rd] = regs[rs1] | imm;
                } else if (funct3 == 0x7) { // ANDI
                    regs[rd] = regs[rs1] & imm;
                } else if (funct3 == 0x1) { // SLLI
                    regs[rd] = regs[rs1] << (imm & 0x1F);
                } else if (funct3 == 0x5) {
                    if ((instr >> 30) & 1) { // SRAI
                        regs[rd] = ((int32_t)regs[rs1]) >> (imm & 0x1F);
                    } else { // SRLI
                        regs[rd] = regs[rs1] >> (imm & 0x1F);
                    }
                } else if (funct3 == 0x2) { // SLTI
                    regs[rd] = ((int32_t)regs[rs1] < imm) ? 1 : 0;
                } else if (funct3 == 0x3) { // SLTIU
                    regs[rd] = (regs[rs1] < (uint32_t)imm) ? 1 : 0;
                }
                pc += 4;
                break;
            }
            case 0x03: { // Load
                int32_t imm = sign_extend((instr >> 20) & 0xFFF, 12);
                uint32_t addr = regs[rs1] + imm;
                if (funct3 == 0x0) { // LB
                    uint8_t val = cache->access(addr, false, 0, 1, false, use_lru);
                    regs[rd] = sign_extend(val, 8);
                } else if (funct3 == 0x1) { // LH
                    uint16_t val = cache->access(addr, false, 0, 2, false, use_lru);
                    regs[rd] = sign_extend(val, 16);
                } else if (funct3 == 0x2) { // LW
                    regs[rd] = cache->access(addr, false, 0, 4, false, use_lru);
                } else if (funct3 == 0x4) { // LBU
                    regs[rd] = cache->access(addr, false, 0, 1, false, use_lru);
                } else if (funct3 == 0x5) { // LHU
                    regs[rd] = cache->access(addr, false, 0, 2, false, use_lru);
                }
                pc += 4;
                break;
            }
            case 0x23: { // Store
                int32_t imm = sign_extend(((instr >> 25) << 5) | rd, 12);
                uint32_t addr = regs[rs1] + imm;
                if (funct3 == 0x0) { // SB
                    cache->access(addr, true, regs[rs2] & 0xFF, 1, false, use_lru);
                } else if (funct3 == 0x1) { // SH
                    cache->access(addr, true, regs[rs2] & 0xFFFF, 2, false, use_lru);
                } else if (funct3 == 0x2) { // SW
                    cache->access(addr, true, regs[rs2], 4, false, use_lru);
                }
                pc += 4;
                break;
            }
            case 0x63: { // Branch
                int32_t imm = sign_extend(
                    ((instr >> 31) << 12) | (((instr >> 7) & 1) << 11) |
                    (((instr >> 25) & 0x3F) << 5) | (((instr >> 8) & 0xF) << 1), 13);
                bool taken = false;
                if (funct3 == 0x0) taken = (regs[rs1] == regs[rs2]); // BEQ
                else if (funct3 == 0x1) taken = (regs[rs1] != regs[rs2]); // BNE
                else if (funct3 == 0x4) taken = ((int32_t)regs[rs1] < (int32_t)regs[rs2]); // BLT
                else if (funct3 == 0x5) taken = ((int32_t)regs[rs1] >= (int32_t)regs[rs2]); // BGE
                else if (funct3 == 0x6) taken = (regs[rs1] < regs[rs2]); // BLTU
                else if (funct3 == 0x7) taken = (regs[rs1] >= regs[rs2]); // BGEU
                
                if (taken) pc += imm;
                else pc += 4;
                break;
            }
            case 0x6F: { // JAL
                int32_t imm = sign_extend(
                    ((instr >> 31) << 20) | (((instr >> 12) & 0xFF) << 12) |
                    (((instr >> 20) & 1) << 11) | (((instr >> 21) & 0x3FF) << 1), 21);
                regs[rd] = pc + 4;
                pc += imm;
                break;
            }
            case 0x67: { // JALR
                int32_t imm = sign_extend((instr >> 20) & 0xFFF, 12);
                uint32_t target = (regs[rs1] + imm) & ~1;
                regs[rd] = pc + 4;
                pc = target;
                break;
            }
            case 0x37: { // LUI
                int32_t imm = instr & 0xFFFFF000;
                regs[rd] = imm;
                pc += 4;
                break;
            }
            case 0x17: { // AUIPC
                int32_t imm = instr & 0xFFFFF000;
                regs[rd] = pc + imm;
                pc += 4;
                break;
            }
            case 0x73: { // ECALL/EBREAK
                return; // Terminate
            }
            default:
                pc += 4;
                break;
        }
        
        regs[0] = 0;
    }
    
    void run() {
        uint64_t instruction_count = 0;
        const uint64_t MAX_INSTRUCTIONS = 1000000; // Защита от бесконечного цикла
        
        while (pc != initial_ra && instruction_count < MAX_INSTRUCTIONS) {
            uint32_t instr = fetch();
            execute(instr);
            instruction_count++;
        }
        
        if (instruction_count >= MAX_INSTRUCTIONS) {
            std::cerr << "Warning: Reached max instruction limit (" << MAX_INSTRUCTIONS << ")" << std::endl;
            std::cerr << "PC = 0x" << std::hex << pc << ", initial_ra = 0x" << initial_ra << std::dec << std::endl;
        }
        
        cache->flush();
    }
};

// ============================================================================
// FILE I/O
// ============================================================================
bool read_input_file(const char* filename, RiscVEmulator& emu) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) return false;
    
    // Read registers (32 * 4 bytes)
    file.read((char*)&emu.pc, 4);
    for (int i = 1; i < 32; i++) {
        file.read((char*)&emu.regs[i], 4);
    }
    emu.initial_ra = emu.regs[1]; // ra = x1
    
    // Read memory fragments
    while (file.peek() != EOF) {
        uint32_t addr, size;
        file.read((char*)&addr, 4);
        file.read((char*)&size, 4);
        
        for (uint32_t i = 0; i < size; i++) {
            uint8_t byte;
            file.read((char*)&byte, 1);
            emu.memory.write8(addr + i, byte);
        }
    }
    
    return true;
}

bool write_output_file(const char* filename, RiscVEmulator& emu, 
                       uint32_t start_addr, uint32_t size) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return false;
    
    // Write registers
    file.write((char*)&emu.pc, 4);
    for (int i = 1; i < 32; i++) {
        file.write((char*)&emu.regs[i], 4);
    }
    
    // Write memory fragment
    file.write((char*)&start_addr, 4);
    file.write((char*)&size, 4);
    for (uint32_t i = 0; i < size; i++) {
        uint8_t byte = emu.memory.read8(start_addr + i);
        file.write((char*)&byte, 1);
    }
    
    return true;
}

// ============================================================================
// MAIN
// ============================================================================
int main(int argc, char* argv[]) {
    std::string input_file;
    std::string output_file;
    uint32_t output_addr = 0;
    uint32_t output_size = 0;
    bool has_output = false;
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 3 < argc) {
            output_file = argv[++i];
            output_addr = strtoul(argv[++i], nullptr, 0);
            output_size = strtoul(argv[++i], nullptr, 0);
            has_output = true;
        }
    }
    
    if (input_file.empty()) {
        std::cerr << "Usage: " << argv[0] << " -i <input_file> [-o <output_file> <start_addr> <size>]" << std::endl;
        return 1;
    }
    
    // Run with LRU
    RiscVEmulator emu_lru(true);
    if (!read_input_file(input_file.c_str(), emu_lru)) {
        std::cerr << "Failed to read input file: " << input_file << std::endl;
        return 1;
    }
    emu_lru.run();
    
    // Run with bit-pLRU
    RiscVEmulator emu_plru(false);
    if (!read_input_file(input_file.c_str(), emu_plru)) {
        std::cerr << "Failed to read input file: " << input_file << std::endl;
        return 1;
    }
    emu_plru.run();
    
    // Calculate hit rates
    double lru_hit_rate = 0.0, lru_instr_rate = 0.0, lru_data_rate = 0.0;
    double plru_hit_rate = 0.0, plru_instr_rate = 0.0, plru_data_rate = 0.0;
    
    uint64_t lru_total = emu_lru.cache->instr_access + emu_lru.cache->data_access;
    uint64_t lru_hits = emu_lru.cache->instr_hit + emu_lru.cache->data_hit;
    
    if (lru_total > 0) lru_hit_rate = (double)lru_hits / lru_total * 100.0;
    if (emu_lru.cache->instr_access > 0) 
        lru_instr_rate = (double)emu_lru.cache->instr_hit / emu_lru.cache->instr_access * 100.0;
    if (emu_lru.cache->data_access > 0)
        lru_data_rate = (double)emu_lru.cache->data_hit / emu_lru.cache->data_access * 100.0;
    
    uint64_t plru_total = emu_plru.cache->instr_access + emu_plru.cache->data_access;
    uint64_t plru_hits = emu_plru.cache->instr_hit + emu_plru.cache->data_hit;
    
    if (plru_total > 0) plru_hit_rate = (double)plru_hits / plru_total * 100.0;
    if (emu_plru.cache->instr_access > 0)
        plru_instr_rate = (double)emu_plru.cache->instr_hit / emu_plru.cache->instr_access * 100.0;
    if (emu_plru.cache->data_access > 0)
        plru_data_rate = (double)emu_plru.cache->data_hit / emu_plru.cache->data_access * 100.0;
    
    // Print results
    printf("| replacement | hit_rate | instr_hit_rate | data_hit_rate | instr_access | instr_hit | data_access | data_hit |\n");
    printf("| :---------- | :-----: | -------------: | ------------: | -----------: | ---------: | ----------: | --------: |\n");
    
    if (lru_total == 0) {
        printf("| LRU | nan%% | nan%% | nan%% | %12d | %12d | %12d | %12d |\n",
               0, 0, 0, 0);
    } else {
        printf("| LRU | %3.4f%% | %3.4f%% | %3.4f%% | %12lu | %12lu | %12lu | %12lu |\n",
               lru_hit_rate, lru_instr_rate, lru_data_rate,
               (unsigned long)emu_lru.cache->instr_access, (unsigned long)emu_lru.cache->instr_hit,
               (unsigned long)emu_lru.cache->data_access, (unsigned long)emu_lru.cache->data_hit);
    }
    
    if (plru_total == 0) {
        printf("| bpLRU | nan%% | nan%% | nan%% | %12d | %12d | %12d | %12d |\n",
               0, 0, 0, 0);
    } else {
        printf("| bpLRU | %3.4f%% | %3.4f%% | %3.4f%% | %12lu | %12lu | %12lu | %12lu |\n",
               plru_hit_rate, plru_instr_rate, plru_data_rate,
               (unsigned long)emu_plru.cache->instr_access, (unsigned long)emu_plru.cache->instr_hit,
               (unsigned long)emu_plru.cache->data_access, (unsigned long)emu_plru.cache->data_hit);
    }
    
    // Write output if requested
    if (has_output) {
        if (!write_output_file(output_file.c_str(), emu_lru, output_addr, output_size)) {
            std::cerr << "Failed to write output file: " << output_file << std::endl;
            return 1;
        }
    }
    
    return 0;
}