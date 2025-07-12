#
# linux/arch/arm/tools/Makefile
#
# Copyright (C) 2001 Russell King
#

# 使用 arch/arm/tools/mach-types 中已定义的板级类型
# 使用 awk 工具生成 include/generated/mach-types.h 头文件
# mach-types 中包含各种单板的类型编码，用于启动是匹配 machine
include/generated/mach-types.h: $(src)/gen-mach-types $(src)/mach-types
	@echo '  Generating $@'
	@mkdir -p $(dir $@)
	$(Q)$(AWK) -f $^ > $@ || { rm -f $@; /bin/false; }
