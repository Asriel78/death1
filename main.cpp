#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <map>
#include <algorithm>
#include <cmath>
#include <stdexcept>

// ============================================================================
// CACHE CONFIGURATION (Variant 1)
// ============================================================================
const uint32_t MEMORY_SIZE = 128 * 1024;      // 128 KBytes (calculated: 2^17)
const uint32_t ADDRESS_LEN = 17;              // 17 bits (given)
const uint32_t CACHE_TAG_LEN = 7;             // 7 bits (calculated: 17 - 4 - 6)
const uint32_t CACHE_INDEX_LEN = 4;           // 4 bits (given)
const uint32_t CACHE_OFFSET_LEN = 6;          // 6 bits (calculated: log2(64))
const uint32_t CACHE_SIZE = 4 * 1024;         // 4 KBytes (calculated: 64 lines * 64 bytes)
const uint32_t CACHE_LINE_SIZE = 64;          // 64 bytes (given)
const uint32_t CACHE_LINE_COUNT = 64;         // 64 lines (given)
const uint32_t CACHE_SET_COUNT = 16;          // 16 sets (calculated: 2^4)
const uint32_t CACHE_WAY = 4;                 // 4-way associative (calculated: 64/16)

// Глобальный флаг отладки
bool g_debug = false;

// ============================================================================
// MEMORY & REGISTERS
// ============================================================================
class Memory {
private:
    const uint32_t MAX_ADDRESS = (1 << ADDRESS_LEN) - 1;
    
public:
    std::map<uint32_t, uint8_t> data;
    
    void validate_address(uint32_t addr) {
        if (addr > MAX_ADDRESS) {
            throw std::runtime_error("Address out of range: 0x" + 
                std::to_string(addr) + " (max: 0x" + std::to_string(MAX_ADDRESS) + ")");
        }
    }
    
    uint8_t read8(uint32_t addr) {
        validate_address(addr);
        return data[addr];
    }
    
    uint16_t read16(uint32_t addr) {
        validate_address(addr);
        validate_address(addr + 1);
        return read8(addr) | (read8(addr + 1) << 8);
    }
    
    uint32_t read32(uint32_t addr) {
        validate_address(addr);
        validate_address(addr + 3);
        return read8(addr) | (read8(addr + 1) << 8) | 
               (read8(addr + 2) << 16) | (read8(addr + 3) << 24);
    }
    
    void write8(uint32_t addr, uint8_t val) {
        validate_address(addr);
        data[addr] = val;
    }
    
    void write16(uint32_t addr, uint16_t val) {
        validate_address(addr);
        validate_address(addr + 1);
        write8(addr, val & 0xFF);
        write8(addr + 1, (val >> 8) & 0xFF);
    }
    
    void write32(uint32_t addr, uint32_t val) {
        validate_address(addr);
        validate_address(addr + 3);
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
    uint32_t lru_counter = 0;
};

// ============================================================================
// CACHE (LRU and bit-pLRU)
// ============================================================================
class Cache {
public:
    CacheLine sets[CACHE_SET_COUNT][CACHE_WAY];
    uint32_t global_counter = 0;
    uint8_t plru_bits[CACHE_SET_COUNT];
    
    // Детальная статистика
    struct Statistics {
        uint64_t instr_access = 0, instr_hit = 0, instr_miss = 0;
        uint64_t data_read_access = 0, data_read_hit = 0, data_read_miss = 0;
        uint64_t data_write_access = 0, data_write_hit = 0, data_write_miss = 0;
        uint64_t evictions = 0;
        uint64_t writebacks = 0;
    } stats;
    
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
            stats.writebacks++;
        }
        
        // Load new line
        line.valid = true;
        line.tag = get_tag(addr);
        line.dirty = false;
        for (uint32_t i = 0; i < CACHE_LINE_SIZE; i++) {
            line.data[i] = memory->read8(block_addr + i);
        }
        
        if (g_debug) {
            printf("  [CACHE] Loaded line: addr=0x%08X, set=%u, way=%u, tag=0x%02X\n",
                   block_addr, set_idx, way_idx, line.tag);
        }
    }
    
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
    
    uint32_t find_plru_victim(uint32_t set_idx) {
        // For 4-way: bit0 = root, bit1 = left subtree, bit2 = right subtree
        uint8_t bits = plru_bits[set_idx];
        uint32_t way;
        
        if ((bits & 0x1) == 0) {
            way = (bits & 0x2) ? 1 : 0;
        } else {
            way = (bits & 0x4) ? 3 : 2;
        }
        
        // Check invalid lines first
        for (uint32_t i = 0; i < CACHE_WAY; i++) {
            if (!sets[set_idx][i].valid) return i;
        }
        
        return way;
    }
    
    void update_plru(uint32_t set_idx, uint32_t way) {
        uint8_t& bits = plru_bits[set_idx];
        
        if (way == 0 || way == 1) {
            bits |= 0x1;
            if (way == 0) bits |= 0x2;
            else bits &= ~0x2;
        } else {
            bits &= ~0x1;
            if (way == 2) bits |= 0x4;
            else bits &= ~0x4;
        }
    }
    
    uint32_t access(uint32_t addr, bool is_write, uint32_t write_data, 
                    uint32_t size, bool is_instruction, bool use_lru) {
        // Валидация
        if (size != 1 && size != 2 && size != 4) {
            throw std::runtime_error("Invalid access size: " + std::to_string(size));
        }
        
        uint32_t offset = get_offset(addr);
        if (offset + size > CACHE_LINE_SIZE) {
            throw std::runtime_error("Access crosses cache line boundary at 0x" + 
                std::to_string(addr));
        }
        
        uint32_t tag = get_tag(addr);
        uint32_t set_idx = get_index(addr);
        
        // Update statistics
        if (is_instruction) {
            stats.instr_access++;
        } else {
            if (is_write) stats.data_write_access++;
            else stats.data_read_access++;
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
            // HIT
            if (is_instruction) {
                stats.instr_hit++;
            } else {
                if (is_write) stats.data_write_hit++;
                else stats.data_read_hit++;
            }
            
            if (g_debug) {
                printf("  [CACHE] HIT: addr=0x%08X, set=%u, way=%d, %s%s\n",
                       addr, set_idx, hit_way, 
                       is_instruction ? "INSTR" : "DATA",
                       is_write ? " WRITE" : " READ");
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
            // MISS
            if (is_instruction) {
                stats.instr_miss++;
            } else {
                if (is_write) stats.data_write_miss++;
                else stats.data_read_miss++;
            }
            stats.evictions++;
            
            uint32_t victim = use_lru ? find_lru_victim(set_idx) : find_plru_victim(set_idx);
            
            if (g_debug) {
                printf("  [CACHE] MISS: addr=0x%08X, set=%u, victim_way=%u, %s%s\n",
                       addr, set_idx, victim,
                       is_instruction ? "INSTR" : "DATA",
                       is_write ? " WRITE" : " READ");
            }
            
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
    
    void print_detailed_stats() {
        uint64_t total_data = stats.data_read_access + stats.data_write_access;
        uint64_t total_data_hit = stats.data_read_hit + stats.data_write_hit;
        
        printf("\n╔════════════════════════════════════════════════════════╗\n");
        printf("║              Detailed Cache Statistics                ║\n");
        printf("╠════════════════════════════════════════════════════════╣\n");
        printf("║ Instructions:                                          ║\n");
        printf("║   Total: %-12lu Hits: %-12lu Misses: %-6lu ║\n", 
               stats.instr_access, stats.instr_hit, stats.instr_miss);
        printf("║ Data Reads:                                            ║\n");
        printf("║   Total: %-12lu Hits: %-12lu Misses: %-6lu ║\n",
               stats.data_read_access, stats.data_read_hit, stats.data_read_miss);
        printf("║ Data Writes:                                           ║\n");
        printf("║   Total: %-12lu Hits: %-12lu Misses: %-6lu ║\n",
               stats.data_write_access, stats.data_write_hit, stats.data_write_miss);
        printf("║ Cache Management:                                      ║\n");
        printf("║   Evictions: %-12lu Writebacks: %-17lu ║\n",
               stats.evictions, stats.writebacks);
        printf("╚════════════════════════════════════════════════════════╝\n");
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
    
    void check_alignment(uint32_t addr, uint32_t size) {
        if (addr % size != 0) {
            if (g_debug) {
                std::cerr << "Warning: Unaligned access at 0x" << std::hex << addr 
                          << " (size=" << std::dec << size << ")" << std::endl;
            }
        }
    }
    
    int32_t sign_extend(uint32_t val, int bits) {
        if (val & (1 << (bits - 1))) {
            return val | (~((1 << bits) - 1));
        }
        return val;
    }
    
    uint32_t fetch() {
        if (g_debug) {
            printf("[FETCH] PC=0x%08X\n", pc);
        }
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
        
        regs[0] = 0;
        
        if (g_debug) {
            printf("[EXEC] PC=0x%08X, instr=0x%08X, opcode=0x%02X\n", pc, instr, opcode);
        }
        
        switch (opcode) {
            case 0x33: { // R-type
                if (funct7 == 0x00 && funct3 == 0x0) regs[rd] = regs[rs1] + regs[rs2];
                else if (funct7 == 0x20 && funct3 == 0x0) regs[rd] = regs[rs1] - regs[rs2];
                else if (funct7 == 0x00 && funct3 == 0x4) regs[rd] = regs[rs1] ^ regs[rs2];
                else if (funct7 == 0x00 && funct3 == 0x6) regs[rd] = regs[rs1] | regs[rs2];
                else if (funct7 == 0x00 && funct3 == 0x7) regs[rd] = regs[rs1] & regs[rs2];
                else if (funct7 == 0x00 && funct3 == 0x1) regs[rd] = regs[rs1] << (regs[rs2] & 0x1F);
                else if (funct7 == 0x00 && funct3 == 0x5) regs[rd] = regs[rs1] >> (regs[rs2] & 0x1F);
                else if (funct7 == 0x20 && funct3 == 0x5) regs[rd] = ((int32_t)regs[rs1]) >> (regs[rs2] & 0x1F);
                else if (funct7 == 0x00 && funct3 == 0x2) regs[rd] = ((int32_t)regs[rs1] < (int32_t)regs[rs2]) ? 1 : 0;
                else if (funct7 == 0x00 && funct3 == 0x3) regs[rd] = (regs[rs1] < regs[rs2]) ? 1 : 0;
                else if (funct7 == 0x01 && funct3 == 0x0) regs[rd] = regs[rs1] * regs[rs2];
                else if (funct7 == 0x01 && funct3 == 0x1) {
                    int64_t result = (int64_t)(int32_t)regs[rs1] * (int64_t)(int32_t)regs[rs2];
                    regs[rd] = result >> 32;
                }
                else if (funct7 == 0x01 && funct3 == 0x2) {
                    int64_t result = (int64_t)(int32_t)regs[rs1] * (uint64_t)regs[rs2];
                    regs[rd] = result >> 32;
                }
                else if (funct7 == 0x01 && funct3 == 0x3) {
                    uint64_t result = (uint64_t)regs[rs1] * (uint64_t)regs[rs2];
                    regs[rd] = result >> 32;
                }
                else if (funct7 == 0x01 && funct3 == 0x4) {
                    if (regs[rs2] == 0) regs[rd] = -1;
                    else regs[rd] = (int32_t)regs[rs1] / (int32_t)regs[rs2];
                }
                else if (funct7 == 0x01 && funct3 == 0x5) {
                    if (regs[rs2] == 0) regs[rd] = 0xFFFFFFFF;
                    else regs[rd] = regs[rs1] / regs[rs2];
                }
                else if (funct7 == 0x01 && funct3 == 0x6) {
                    if (regs[rs2] == 0) regs[rd] = regs[rs1];
                    else regs[rd] = (int32_t)regs[rs1] % (int32_t)regs[rs2];
                }
                else if (funct7 == 0x01 && funct3 == 0x7) {
                    if (regs[rs2] == 0) regs[rd] = regs[rs1];
                    else regs[rd] = regs[rs1] % regs[rs2];
                }
                pc += 4;
                break;
            }
            case 0x13: { // I-type
                int32_t imm = sign_extend((instr >> 20) & 0xFFF, 12);
                if (funct3 == 0x0) regs[rd] = regs[rs1] + imm;
                else if (funct3 == 0x4) regs[rd] = regs[rs1] ^ imm;
                else if (funct3 == 0x6) regs[rd] = regs[rs1] | imm;
                else if (funct3 == 0x7) regs[rd] = regs[rs1] & imm;
                else if (funct3 == 0x1) regs[rd] = regs[rs1] << (imm & 0x1F);
                else if (funct3 == 0x5) {
                    if ((instr >> 30) & 1) regs[rd] = ((int32_t)regs[rs1]) >> (imm & 0x1F);
                    else regs[rd] = regs[rs1] >> (imm & 0x1F);
                }
                else if (funct3 == 0x2) regs[rd] = ((int32_t)regs[rs1] < imm) ? 1 : 0;
                else if (funct3 == 0x3) regs[rd] = (regs[rs1] < (uint32_t)imm) ? 1 : 0;
                pc += 4;
                break;
            }
            case 0x03: { // Load
                int32_t imm = sign_extend((instr >> 20) & 0xFFF, 12);
                uint32_t addr = regs[rs1] + imm;
                if (funct3 == 0x0) {
                    uint8_t val = cache->access(addr, false, 0, 1, false, use_lru);
                    regs[rd] = sign_extend(val, 8);
                } else if (funct3 == 0x1) {
                    check_alignment(addr, 2);
                    uint16_t val = cache->access(addr, false, 0, 2, false, use_lru);
                    regs[rd] = sign_extend(val, 16);
                } else if (funct3 == 0x2) {
                    check_alignment(addr, 4);
                    regs[rd] = cache->access(addr, false, 0, 4, false, use_lru);
                } else if (funct3 == 0x4) {
                    regs[rd] = cache->access(addr, false, 0, 1, false, use_lru);
                } else if (funct3 == 0x5) {
                    check_alignment(addr, 2);
                    regs[rd] = cache->access(addr, false, 0, 2, false, use_lru);
                }
                pc += 4;
                break;
            }
            case 0x23: { // Store
                int32_t imm = sign_extend(((instr >> 25) << 5) | rd, 12);
                uint32_t addr = regs[rs1] + imm;
                if (funct3 == 0x0) {
                    cache->access(addr, true, regs[rs2] & 0xFF, 1, false, use_lru);
                } else if (funct3 == 0x1) {
                    check_alignment(addr, 2);
                    cache->access(addr, true, regs[rs2] & 0xFFFF, 2, false, use_lru);
                } else if (funct3 == 0x2) {
                    check_alignment(addr, 4);
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
                if (funct3 == 0x0) taken = (regs[rs1] == regs[rs2]);
                else if (funct3 == 0x1) taken = (regs[rs1] != regs[rs2]);
                else if (funct3 == 0x4) taken = ((int32_t)regs[rs1] < (int32_t)regs[rs2]);
                else if (funct3 == 0x5) taken = ((int32_t)regs[rs1] >= (int32_t)regs[rs2]);
                else if (funct3 == 0x6) taken = (regs[rs1] < regs[rs2]);
                else if (funct3 == 0x7) taken = (regs[rs1] >= regs[rs2]);
                
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
                if (g_debug) printf("[EXEC] ECALL/EBREAK - terminating\n");
                return;
            }
            default:
                if (g_debug) printf("[EXEC] Unknown opcode: 0x%02X\n", opcode);
                pc += 4;
                break;
        }
        
        regs[0] = 0;
    }
    
    void run() {
        uint64_t instruction_count = 0;
        const uint64_t MAX_INSTRUCTIONS = 1000000;
        
        while (pc != initial_ra && instruction_count < MAX_INSTRUCTIONS) {
            uint32_t instr = fetch();
            execute(instr);
            instruction_count++;
        }
        
        if (instruction_count >= MAX_INSTRUCTIONS) {
            std::cerr << "Warning: Reached max instruction limit (" << MAX_INSTRUCTIONS << ")" << std::endl;
            std::cerr << "PC = 0x" << std::hex << pc << ", initial_ra = 0x" << initial_ra << std::dec << std::endl;
        }
        
        if (g_debug) {
            printf("\n[RUN] Executed %lu instructions\n", instruction_count);
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
    
    file.read((char*)&emu.pc, 4);
    for (int i = 1; i < 32; i++) {
        file.read((char*)&emu.regs[i], 4);
    }
    emu.initial_ra = emu.regs[1];
    
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
    
    if (g_debug) {
        printf("[FILE] Loaded: PC=0x%08X, RA=0x%08X\n", emu.pc, emu.initial_ra);
    }
    
    return true;
}

bool write_output_file(const char* filename, RiscVEmulator& emu, 
                       uint32_t start_addr, uint32_t size) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) return false;
    
    file.write((char*)&emu.pc, 4);
    for (int i = 1; i < 32; i++) {
        file.write((char*)&emu.regs[i], 4);
    }
    
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
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            input_file = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 3 < argc) {
            output_file = argv[++i];
            output_addr = strtoul(argv[++i], nullptr, 0);
            output_size = strtoul(argv[++i], nullptr, 0);
            has_output = true;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
            g_debug = true;
        }
    }
    
    if (input_file.empty()) {
        std::cerr << "Usage: " << argv[0] << " -i <input_file> [-o <output_file> <start_addr> <size>] [-d]" << std::endl;
        return 1;
    }
    
    try {
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
        
        uint64_t lru_total = emu_lru.cache->stats.instr_access + 
                             emu_lru.cache->stats.data_read_access + 
                             emu_lru.cache->stats.data_write_access;
        uint64_t lru_hits = emu_lru.cache->stats.instr_hit + 
                            emu_lru.cache->stats.data_read_hit + 
                            emu_lru.cache->stats.data_write_hit;
        
        if (lru_total > 0) lru_hit_rate = (double)lru_hits / lru_total * 100.0;
        if (emu_lru.cache->stats.instr_access > 0) 
            lru_instr_rate = (double)emu_lru.cache->stats.instr_hit / emu_lru.cache->stats.instr_access * 100.0;
        
        uint64_t lru_data_total = emu_lru.cache->stats.data_read_access + emu_lru.cache->stats.data_write_access;
        uint64_t lru_data_hits = emu_lru.cache->stats.data_read_hit + emu_lru.cache->stats.data_write_hit;
        if (lru_data_total > 0)
            lru_data_rate = (double)lru_data_hits / lru_data_total * 100.0;
        
        uint64_t plru_total = emu_plru.cache->stats.instr_access + 
                              emu_plru.cache->stats.data_read_access + 
                              emu_plru.cache->stats.data_write_access;
        uint64_t plru_hits = emu_plru.cache->stats.instr_hit + 
                             emu_plru.cache->stats.data_read_hit + 
                             emu_plru.cache->stats.data_write_hit;
        
        if (plru_total > 0) plru_hit_rate = (double)plru_hits / plru_total * 100.0;
        if (emu_plru.cache->stats.instr_access > 0)
            plru_instr_rate = (double)emu_plru.cache->stats.instr_hit / emu_plru.cache->stats.instr_access * 100.0;
        
        uint64_t plru_data_total = emu_plru.cache->stats.data_read_access + emu_plru.cache->stats.data_write_access;
        uint64_t plru_data_hits = emu_plru.cache->stats.data_read_hit + emu_plru.cache->stats.data_write_hit;
        if (plru_data_total > 0)
            plru_data_rate = (double)plru_data_hits / plru_data_total * 100.0;
        
        // Print results in required format
        printf("| replacement | hit_rate | instr_hit_rate | data_hit_rate | instr_access | instr_hit | data_access | data_hit |\n");
        printf("| :---------- | :-----: | -------------: | ------------: | -----------: | ---------: | ----------: | --------: |\n");
        
        if (lru_total == 0) {
            printf("| LRU | nan%% | nan%% | nan%% | %12d | %12d | %12d | %12d |\n",
                   0, 0, 0, 0);
        } else {
            printf("| LRU | %3.4f%% | %3.4f%% | %3.4f%% | %12lu | %12lu | %12lu | %12lu |\n",
                   lru_hit_rate, lru_instr_rate, lru_data_rate,
                   (unsigned long)emu_lru.cache->stats.instr_access, 
                   (unsigned long)emu_lru.cache->stats.instr_hit,
                   (unsigned long)lru_data_total, 
                   (unsigned long)lru_data_hits);
        }
        
        if (plru_total == 0) {
            printf("| bpLRU | nan%% | nan%% | nan%% | %12d | %12d | %12d | %12d |\n",
                   0, 0, 0, 0);
        } else {
            printf("| bpLRU | %3.4f%% | %3.4f%% | %3.4f%% | %12lu | %12lu | %12lu | %12lu |\n",
                   plru_hit_rate, plru_instr_rate, plru_data_rate,
                   (unsigned long)emu_plru.cache->stats.instr_access, 
                   (unsigned long)emu_plru.cache->stats.instr_hit,
                   (unsigned long)plru_data_total, 
                   (unsigned long)plru_data_hits);
        }
        
        // Print detailed stats if debug enabled
        if (g_debug) {
            printf("\n=== LRU Statistics ===\n");
            emu_lru.cache->print_detailed_stats();
            
            printf("\n=== bit-pLRU Statistics ===\n");
            emu_plru.cache->print_detailed_stats();
        }
        
        // Write output if requested
        if (has_output) {
            if (!write_output_file(output_file.c_str(), emu_lru, output_addr, output_size)) {
                std::cerr << "Failed to write output file: " << output_file << std::endl;
                return 1;
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
