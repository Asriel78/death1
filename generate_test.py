import struct

"""
Улучшенный генератор task.bin для Варианта 1

Параметры кэша:
- 16 наборов × 4 линии = 64 линии
- Размер линии: 64 байта
- INDEX: биты 6-9 адреса (4 бита)
- OFFSET: биты 0-5 (6 бит)
- TAG: биты 10-16 (7 бит)

Целевые проценты (укажите свои из таблицы):
- instr_hit_rate: 92.3077% (пример: 12/13)
- data_hit_rate: 50.0000% (пример: 4/8)

Стратегия для 92.3077% instruction hit rate:
- 13 инструкций, все в пределах одной кэш-линии (64 байта)
- 13 × 4 = 52 байта < 64 байта
- Результат: 1 miss (первая загрузка) + 12 hits = 92.3077%

Стратегия для 50% data hit rate:
- 8 обращений к данным (4 load + 4 store)
- Используем 4 уникальных адреса
- Каждый адрес: первое обращение = miss, второе = hit
- Результат: 4 miss + 4 hit = 50%
"""


def encode_i_type(opcode, rd, funct3, rs1, imm):
    """Кодирует I-type инструкцию"""
    imm = imm & 0xFFF
    return (imm << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode


def encode_s_type(opcode, funct3, rs1, rs2, imm):
    """Кодирует S-type инструкцию"""
    imm = imm & 0xFFF
    imm_11_5 = (imm >> 5) & 0x7F
    imm_4_0 = imm & 0x1F
    return (imm_11_5 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (imm_4_0 << 7) | opcode


def get_cache_info(addr):
    """Вычисляет tag, index, offset для адреса"""
    offset = addr & 0x3F  # биты 0-5
    index = (addr >> 6) & 0xF  # биты 6-9
    tag = (addr >> 10) & 0x7F  # биты 10-16
    return tag, index, offset


print("=" * 70)
print("RISC-V Cache Test Generator - Variant 1")
print("=" * 70)

# Создаем task.bin
with open('task.bin', 'wb') as f:
    # ========== РЕГИСТРЫ ==========
    PC_START = 0x1000

    # PC = 0x1000 (начало программы)
    f.write(struct.pack('<I', PC_START))

    # x1 (ra) - адрес возврата (вычислим после создания инструкций)
    ra_position = f.tell()
    f.write(struct.pack('<I', 0))  # временно 0

    # x2-x31 = 0
    for i in range(2, 32):
        f.write(struct.pack('<I', 0))

    # ========== КОД (ИНСТРУКЦИИ) ==========
    instructions = []

    # Все 13 инструкций будут в одной кэш-линии
    # Адрес начала: 0x1000 (offset=0, index=0, tag=4)

    # 1. Инициализация базового адреса данных
    instructions.append(encode_i_type(0x13, 2, 0, 0, 0x2000))  # addi x2, x0, 0x2000

    # Группа 1: два обращения к адресу 0x2000
    # 2-3. Load дважды из одного адреса
    instructions.append(encode_i_type(0x03, 3, 2, 2, 0))  # lw x3, 0(x2)   [MISS]
    instructions.append(encode_i_type(0x03, 4, 2, 2, 0))  # lw x4, 0(x2)   [HIT]

    # Группа 2: смещение + load + store
    # 4. Смещение на следующую кэш-линию
    instructions.append(encode_i_type(0x13, 2, 0, 2, 64))  # addi x2, x2, 64
    # 5-6. Load + Store
    instructions.append(encode_i_type(0x03, 3, 2, 2, 0))  # lw x3, 0(x2)   [MISS]
    instructions.append(encode_s_type(0x23, 2, 2, 3, 0))  # sw x3, 0(x2)   [HIT]

    # Группа 3: смещение + load + store
    # 7. Смещение
    instructions.append(encode_i_type(0x13, 2, 0, 2, 64))  # addi x2, x2, 64
    # 8-9. Load + Store
    instructions.append(encode_i_type(0x03, 3, 2, 2, 0))  # lw x3, 0(x2)   [MISS]
    instructions.append(encode_s_type(0x23, 2, 2, 3, 0))  # sw x3, 0(x2)   [HIT]

    # Группа 4: смещение + load + store
    # 10. Смещение
    instructions.append(encode_i_type(0x13, 2, 0, 2, 64))  # addi x2, x2, 64
    # 11-12. Load + Store
    instructions.append(encode_i_type(0x03, 3, 2, 2, 0))  # lw x3, 0(x2)   [MISS]
    instructions.append(encode_s_type(0x23, 2, 2, 3, 0))  # sw x3, 0(x2)   [HIT]

    # 13. NOP для завершения (не обязательно, но хорошая практика)
    instructions.append(encode_i_type(0x13, 0, 0, 0, 0))  # addi x0, x0, 0 (NOP)

    assert len(instructions) == 13, f"Expected 13 instructions, got {len(instructions)}"

    # Вычисляем адрес возврата
    code_size = len(instructions) * 4
    return_addr = PC_START + code_size

    # Обновляем x1 (ra)
    current_pos = f.tell()
    f.seek(ra_position)
    f.write(struct.pack('<I', return_addr))
    f.seek(current_pos)

    # Выводим информацию о коде
    print(f"\nCode segment:")
    print(f"  Address: 0x{PC_START:04X} - 0x{PC_START + code_size - 1:04X}")
    print(f"  Size: {code_size} bytes ({len(instructions)} instructions)")
    tag, index, offset = get_cache_info(PC_START)
    print(f"  Cache: tag=0x{tag:02X}, index={index}, offset={offset}")
    print(f"  Return address (ra): 0x{return_addr:04X}")

    # Записываем фрагмент с кодом
    f.write(struct.pack('<I', PC_START))
    f.write(struct.pack('<I', code_size))
    for instr in instructions:
        f.write(struct.pack('<I', instr & 0xFFFFFFFF))

    # ========== ДАННЫЕ ==========
    # 4 адреса данных со смещением 64 байта (одна кэш-линия)
    data_addresses = [0x2000, 0x2040, 0x2080, 0x20C0]

    print(f"\nData segments:")
    for i, addr in enumerate(data_addresses):
        tag, index, offset = get_cache_info(addr)
        print(f"  #{i + 1}: 0x{addr:04X} -> tag=0x{tag:02X}, index={index}, offset={offset}")

        f.write(struct.pack('<I', addr))
        f.write(struct.pack('<I', 4))
        f.write(struct.pack('<I', 0x12345678 + i))

print("\n" + "=" * 70)
print("✓ Created task.bin")
print("=" * 70)

print("\nExpected cache behavior:")
print("\n1. INSTRUCTIONS (13 accesses):")
print("   - All 13 instructions fit in one cache line (52 bytes < 64 bytes)")
print("   - First fetch: MISS (load cache line)")
print("   - Next 12 fetches: HIT (already in cache)")
print("   - Hit rate: 12/13 = 92.3077%")

print("\n2. DATA (8 accesses):")
print("   - Address 0x2000: load (MISS) + load (HIT)")
print("   - Address 0x2040: load (MISS) + store (HIT)")
print("   - Address 0x2080: load (MISS) + store (HIT)")
print("   - Address 0x20C0: load (MISS) + store (HIT)")
print("   - Total: 4 MISS + 4 HIT = 50.0000%")

print("\n3. OVERALL:")
print("   - Total accesses: 13 (instr) + 8 (data) = 21")
print("   - Total hits: 12 (instr) + 4 (data) = 16")
print("   - Overall hit rate: 16/21 = 76.1905%")

print("\n" + "=" * 70)
print("Run with: ./riscv_emu -i task.bin")
print("Debug mode: ./riscv_emu -i task.bin -d")
print("=" * 70)
