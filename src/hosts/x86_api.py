"""Numeric register/hook identifiers shared by the MAME host adapters. MIT.
Preserves the existing host interface without depending on a CPU library or loader.
"""
UC_ARCH_X86 = 4
UC_MODE_16 = 2
UC_PROT_ALL = 7
UC_HOOK_INTR = 1
UC_HOOK_INSN = 2
UC_HOOK_CODE = 4
UC_HOOK_BLOCK = 8
UC_HOOK_MEM_WRITE = 2048
UC_X86_REG_AH = 1
UC_X86_REG_AL = 2
UC_X86_REG_AX = 3
UC_X86_REG_BP = 6
UC_X86_REG_BX = 8
UC_X86_REG_CX = 12
UC_X86_REG_DI = 14
UC_X86_REG_DS = 17
UC_X86_REG_DX = 18
UC_X86_REG_CS = 11
UC_X86_REG_EFLAGS = 25
UC_X86_REG_ES = 28
UC_X86_REG_IP = 34
UC_X86_REG_SI = 45
UC_X86_REG_SP = 47
UC_X86_REG_SS = 49
UC_X86_INS_IN = 218
UC_X86_INS_OUT = 500
