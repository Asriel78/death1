import struct

"""
Генератор task.bin для достижения целевых процентов:
- instr_hit_rate: 92.3077% (12/13 обращений)
- data_hit_rate: 50.0000% (ровно половина)

Вариант 1 кэша:
- 16 наборов × 4 линии = 64 линии
- Размер линии: 64 байта
- INDEX: 4 бита (биты 6-9 адреса)
- OFFSET: 6 бит (биты 0-5)

Стратегия:
1. Инструкции (13 обращений):
   - 1 miss (первое fetch)
   - 12 hits (остальные в том же блоке или цикле)
   
2. Данные (нужно четное количество для 50%):
   - Используем 2, 4, 6, 8... обращений
   - Половина miss, половина hit
"""

def encode_r_type(opcode, rd, funct3, rs1, rs2, funct7):
    """Кодирует R-type инструкцию"""
    return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode

def encode_i_type(opcode, rd, funct3, rs1, imm):
    """Кодирует I-type инструкцию"""
    imm = imm & 0xFFF  # Обрезаем до 12 бит
    return (imm << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode

def encode_s_type(opcode, funct3, rs1, rs2, imm):
    """Кодирует S-type инструкцию"""
    imm = imm & 0xFFF  # Обрезаем до 12 бит
    imm_11_5 = (imm >> 5) & 0x7F
    imm_4_0 = imm & 0x1F
    return (imm_11_5 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (imm_4_0 << 7) | opcode

def encode_b_type(opcode, funct3, rs1, rs2, imm):
    """Кодирует B-type инструкцию"""
    imm = imm & 0x1FFF  # Обрезаем до 13 бит
    imm_12 = (imm >> 12) & 1
    imm_10_5 = (imm >> 5) & 0x3F
    imm_4_1 = (imm >> 1) & 0xF
    imm_11 = (imm >> 11) & 1
    return (imm_12 << 31) | (imm_10_5 << 25) | (rs2 << 20) | (rs1 << 15) | \
           (funct3 << 12) | (imm_4_1 << 8) | (imm_11 << 7) | opcode

# Создаем task.bin
with open('task.bin', 'wb') as f:
    # ========== РЕГИСТРЫ ==========
    # PC = 0x1000 (начало программы)
    f.write(struct.pack('<I', 0x1000))
    
    # x1 (ra) = 0x1034 (адрес возврата - после 13 инструкций = 0x1000 + 13*4)
    f.write(struct.pack('<I', 0x1034))
    
    # x2-x31 = 0
    for i in range(2, 32):
        f.write(struct.pack('<I', 0))
    
    # ========== КОД (ИНСТРУКЦИИ) ==========
    # Адрес: 0x1000
    # Программа: 13 инструкций (52 байта)
    # Все инструкции в пределах одной кэш-линии (64 байта)
    # -> 1 miss (первое fetch) + 12 hits = 92.3077%
    
    instructions = []
    
    # Инициализация (2 инструкции)
    instructions.append(encode_i_type(0x13, 2, 0, 0, 0x2000))   # addi x2, x0, 0x2000 (базовый адрес данных)
    instructions.append(encode_i_type(0x13, 3, 0, 0, 4))        # addi x3, x0, 4 (счетчик)
    
    # Первое обращение к данным (2 инструкции)
    instructions.append(encode_i_type(0x03, 4, 2, 2, 0))        # lw x4, 0(x2)     - load 1
    instructions.append(encode_s_type(0x23, 2, 2, 4, 0))        # sw x4, 0(x2)     - store 1
    
    # Смещение и второе обращение (3 инструкции)
    instructions.append(encode_i_type(0x13, 2, 0, 2, 1024))     # addi x2, x2, 1024
    instructions.append(encode_i_type(0x03, 4, 2, 2, 0))        # lw x4, 0(x2)     - load 2
    instructions.append(encode_s_type(0x23, 2, 2, 4, 0))        # sw x4, 0(x2)     - store 2
    
    # Смещение и третье обращение (3 инструкции)
    instructions.append(encode_i_type(0x13, 2, 0, 2, 1024))     # addi x2, x2, 1024
    instructions.append(encode_i_type(0x03, 4, 2, 2, 0))        # lw x4, 0(x2)     - load 3
    instructions.append(encode_s_type(0x23, 2, 2, 4, 0))        # sw x4, 0(x2)     - store 3
    
    # Смещение и четвертое обращение (3 инструкции)
    instructions.append(encode_i_type(0x13, 2, 0, 2, 1024))     # addi x2, x2, 1024
    instructions.append(encode_i_type(0x03, 4, 2, 2, 0))        # lw x4, 0(x2)     - load 4
    instructions.append(encode_s_type(0x23, 2, 2, 4, 0))        # sw x4, 0(x2)     - store 4
    
    # Итого: 2 + 2 + 3 + 3 + 3 = 13 инструкций
    # PC дойдет до 0x1034 (0x1000 + 13*4) и затем до 0x1040 не дойдет
    # Нужно поправить ra!
    
    # Записываем фрагмент с кодом
    code_addr = 0x1000
    code_size = len(instructions) * 4
    
    f.write(struct.pack('<I', code_addr))
    f.write(struct.pack('<I', code_size))
    for instr in instructions:
        f.write(struct.pack('<I', instr & 0xFFFFFFFF))  # Маска для unsigned
    
    # ========== ДАННЫЕ ==========
    # Адреса: 0x2000, 0x2400, 0x2800, 0x2C00
    # Смещение 1024 байта = 16 кэш-линий
    # Используем set 0 (биты 6-9 = 0000)
    
    # 4 фрагмента данных (по 4 байта)
    data_addresses = [0x2000, 0x2400, 0x2800, 0x2C00]
    
    for addr in data_addresses:
        f.write(struct.pack('<I', addr))
        f.write(struct.pack('<I', 4))  # размер
        f.write(struct.pack('<I', 0))  # значение = 0

print("✓ Created task.bin")
print()
print("Expected results:")
print("  Instructions: 13 accesses, 12 hits → 92.3077%")
print("  Data: 8 accesses (4 loads + 4 stores)")
print("    - With evictions → 50% hit rate")
print()
print("Run: riscv_emu -i task.bin")
