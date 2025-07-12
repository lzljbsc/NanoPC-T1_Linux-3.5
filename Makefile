VERSION = 3
PATCHLEVEL = 5
SUBLEVEL = 0
EXTRAVERSION =
NAME = Saber-toothed Squirrel

# 内核版本号，在下面会根据这几个宏，生成版本号相关的其他宏
# 如 UTS_RELEASE , include/linux/version.h  init/version.c

# *DOCUMENTATION*
# 要查看典型目标的列表，请执行“make help”
# 更多信息参考 ./README
# 此文件中的注释仅针对开发人员，不希望通过阅读此文件来学习如何构建内核。
# To see a list of typical targets execute "make help"
# More info can be located in ./README
# Comments in this file are targeted only to the developer, do not
# expect to learn how to build the kernel reading this file.

# 不要：
# 		使用make的内置规则和变量
# 			(这提高了性能并避免了难以调试的行为)
# 		print "Entering directory ..."；
# Do not:
# o  use make's built-in rules and variables
#    (this increases performance and avoids hard-to-debug behaviour);
# o  print "Entering directory ...";
MAKEFLAGS += -rR --no-print-directory

# 避免滑稽的字符集依赖
# Avoid funny character set dependencies
unexport LC_ALL
LC_COLLATE=C
LC_NUMERIC=C
export LC_COLLATE LC_NUMERIC

# 我们正在使用递归构建，因此我们需要做一些思考来获得正确的排序。
# We are using a recursive build, so we need to do a little thinking
# to get the ordering right.
#
# 最重要的是:子makefile应该只修改自己目录下的文件。如果在某些目录中，我们对另一
# 个目录中的文件有依赖关系(这种情况并不经常发生，但当链接built-in.o目标最终变成vmlinux
# 时，这通常是不可避免的)，我们将在另一个目录中调用sub make，然后我们确定在另一
# 个目录中的所有内容现在都是最新的。
# Most importantly: sub-Makefiles should only ever modify files in
# their own directory. If in some directory we have a dependency on
# a file in another dir (which doesn't happen often, but it's often
# unavoidable when linking the built-in.o targets which finally
# turn into vmlinux), we will call a sub make in that other dir, and
# after that we are sure that everything which is in that other dir
# is now up to date.
#
# 需要修改具有全局影响的文件的情况都在递归进行之前分离出来，并作为准备规则显式列
# 出。 prepare规则用于处理这些情况。
# The only cases where we need to modify files which have global
# effects are thus separated out and done before the recursive
# descending is started. They are now explicitly listed as the
# prepare rule.

# 如果需要查看完整的命令，可以使用"make V=1"命令来显示所有命令。默认情况下会减少
# 冗长的输出。
# To put more focus on warnings, be less verbose as default
# Use 'make V=1' to see the full commands

# makefile 中的条件判断语句，用于检查变量V的来源是否为命令行。
# 如果 V 的来源是命令行，则条件成立。
ifeq ("$(origin V)", "command line")
  KBUILD_VERBOSE = $(V)
endif
ifndef KBUILD_VERBOSE
  KBUILD_VERBOSE = 0
endif

# 调用源代码检查器(默认为“sparse”)作为C编译的一部分。
# 使用'make C=1'来启用只检查重新编译的文件。
# 使用'make C=2'启用检查*所有*源文件，无论它们是否被重新编译。
# Call a source code checker (by default, "sparse") as part of the
# C compilation.
#
# Use 'make C=1' to enable checking of only re-compiled files.
# Use 'make C=2' to enable checking of *all* source files, regardless
# of whether they are re-compiled or not.
#
# See the file "Documentation/sparse.txt" for more details, including
# where to get the "sparse" utility.

ifeq ("$(origin C)", "command line")
  KBUILD_CHECKSRC = $(C)
endif
ifndef KBUILD_CHECKSRC
  KBUILD_CHECKSRC = 0
endif

# 使用make M=dir指定要构建的外部模块的目录。旧语法make…SUBDIRS=$PWD仍然被支持
# 优先设置环境变量KBUILD_EXTMOD
# Use make M=dir to specify directory of external module to build
# Old syntax make ... SUBDIRS=$PWD is still supported
# Setting the environment variable KBUILD_EXTMOD take precedence
ifdef SUBDIRS
  KBUILD_EXTMOD ?= $(SUBDIRS)
endif

ifeq ("$(origin M)", "command line")
  KBUILD_EXTMOD := $(M)
endif

# Kbuild支持将输出文件保存在单独的目录中。
# 要在单独的目录中定位输出文件，支持两种语法。
# 在这两种情况下，工作目录必须是内核src的根目录。
# 1) O=
# Use "make O=dir/to/store/output/files/"
#
# 2) Set KBUILD_OUTPUT
# 将环境变量KBUILD_OUTPUT设置为指向应该放置输出文件的目录。
# export KBUILD_OUTPUT=dir/to/store/output/files/
# make
#
# O=赋值优先于KBUILD_OUTPUT环境变量。
#
# kbuild supports saving output files in a separate directory.
# To locate output files in a separate directory two syntaxes are supported.
# In both cases the working directory must be the root of the kernel src.
# 1) O=
# Use "make O=dir/to/store/output/files/"
#
# 2) Set KBUILD_OUTPUT
# Set the environment variable KBUILD_OUTPUT to point to the directory
# where the output files shall be placed.
# export KBUILD_OUTPUT=dir/to/store/output/files/
# make
#
# The O= assignment takes precedence over the KBUILD_OUTPUT environment
# variable.


# 在OBJ目录中调用make时设置KBUILD_SRC
# KBUILD_SRC不打算被普通用户使用(目前)
# 如果没有使用 O= 命令选项，那 KBUILD_SRC 应该一直是空的
# KBUILD_SRC is set on invocation of make in OBJ directory
# KBUILD_SRC is not intended to be used by the regular user (for now)
ifeq ($(KBUILD_SRC),)

# 在内核src所在的目录中调用Make
# 我们是否希望在单独的目录中定位输出文件?
# 命令行中设置了 O 参数，则这里会设置 KBUILD_OUTPUT 变量
# OK, Make called in directory where kernel src resides
# Do we want to locate output files in a separate directory?
ifeq ("$(origin O)", "command line")
  KBUILD_OUTPUT := $(O)
endif

# W 参数用于开启 gcc 的 -W 选项
# gcc编译器的"-W"选项是开启所有警告信息的输出。默认情况下，gcc只会输出某些常见警
# 告，而"-W"选项将启用所有警告信息的输出。这些警告信息可以帮助开发人员找到代码中
# 的问题，包括不规范的代码、潜在的问题等。
ifeq ("$(origin W)", "command line")
  export KBUILD_ENABLE_EXTRA_GCC_CHECKS := $(W)
endif

# 当命令行上没有指定时，这是我们的默认目标
# That's our default target when none is given on the command line
# 定义伪目标 _all
# 真正的伪目标定义是使用 .PHONY ，这里直接使用 PHONY 变量，是因为在本makefile的
# 最后，有一个定义 .PHONY 的地方，并将 PHONY 变量的所有内容做为伪目标
PHONY := _all
_all:

# 这是一条"空指令"，Makefile中使用它来阻止make使用隐含规则构建指定目标。
# make 在执行时，需要一个命名为Makefile 的文件。在一个完整的Makefile 中，一般包
# 含了5部分：规则(显示指定和隐含规则)、变量定义(同样包含显示变量和隐含变量)、指
# 示符( include, define 等 )和注释。
# 显示规则是由作者显示写出的规则，而隐含规则则是内建在make 中，为make 提供了重
# 建某一类目标文件(.o 等)的通用方法，同时这些隐含规则所用到的变量也就是所谓的隐
# 含变量。隐含规则和隐含变量可以通过 make -p -f /dev/null 查看。隐含规则的好处是
# 在Makefile 中不需要明确给出重建某一个目标的命令，甚至可以不需要规则。make会为
# 你自动搜寻匹配的隐含规则链。
# 这个世界上没有免费的午餐，隐含规则的代价之一就是低效，系统必须搜索可能的隐含规
# 则链。同时隐含规则也有可能应用了不是你想要的规则而引入很难debug的错误。
# Cancel implicit rules on top Makefile的意思就是取消上述makefile的隐含规则。
# 为什么要定义一个没有命令的规则。其唯一的原因是，空命令行可以防止make在执行时试
# 图为重建这个目标去查找隐含命令（包括了使用隐含规则中的命令和“.DEFAULT”指定的命令。
# 取消Makefile顶部的隐式规则
# Cancel implicit rules on top Makefile
$(CURDIR)/Makefile Makefile: ;

# 命令行中指定了单独的编译中间文件输出目录，则进行下面的处理
# 如果没有使用 O= 命令行参数 ，并且也没有设置 KBUILD_OUTPUT 环境变量，
# 那下面的这些就不处理了
ifneq ($(KBUILD_OUTPUT),)
# 在输出目录中调用第二个make，传递相关变量，检查输出目录是否实际存在
# 确认输出目录的绝对路径，并判断目录是否存在
# Invoke a second make in the output directory, passing relevant variables
# check that the output directory actually exists
saved-output := $(KBUILD_OUTPUT)
KBUILD_OUTPUT := $(shell cd $(KBUILD_OUTPUT) && /bin/pwd)
$(if $(KBUILD_OUTPUT),, \
     $(error output directory "$(saved-output)" does not exist))

# MAKECMDGOALS是一个Makefile中的特殊变量，用于表示用户在命令行中输入的目标。它保
# 存了用户在命令行中指定的目标列表，可以在Makefile中使用这个变量来获取用户指定的
# 目标，并做相应的处理。通常情况下，MAKECMDGOALS会包含用户在命令行中指定的目标，
# 可以用于动态地控制Makefile中的行为。
PHONY += $(MAKECMDGOALS) sub-make

# MAKECMDGOALS 是 make 的环境变量，存放了指定的终极目标列表，如果命令行上没有指
# 定任何目标，那这个变量就是空值
# 这段代码是一个Makefile规则，它使用了Makefile内置函数filter-out和变量$(Q)。
# $(filter-out _all sub-make $(CURDIR)/Makefile, $(MAKECMDGOALS))
# 这部分使用了filter-out函数，作用是从 $(MAKECMDGOALS) 中过滤掉 _all sub-make 和
# $(CURDIR)/Makefile这些目标。该函数返回过滤后的目标列表。
#
# _all: sub-make
# 这一部分指定了_all目标和其依赖sub-make。
#
# $(Q)@:
# 这一部分使用了变量$(Q)，并且后面紧跟着@:。
# 用来实现安静模式（quiet mode）下的输出，一般情况下@用于禁止Make工具输出当前命
# 令。
# 这里的作用是需要先处理 sub-make 目标
$(filter-out _all sub-make $(CURDIR)/Makefile, $(MAKECMDGOALS)) _all: sub-make
	$(Q)@:

# $(if $(KBUILD_VERBOSE:1=),@) 用来判断 KBUILD_VERBOSE 并替换 @ 符号
# KBUILD_VERBOSE = 1 or KBUILD_VERBOSE is NULL
# 如果命令前有 @ 符号，则命令不进行回显
#
# 这一行指定了sub-make目标，并且它有一个伪目标FORCE作为其依赖。
# 最终效果是执行了 make -C $(KBUILD_OUTPUT) KBUILD_SRC=$(CURDIR)
# KBUILD_EXTMOD="$(KBUILD_EXTMOD)" -f $(CURDIR)/Makefile [targets]
# 也就是又在本目录下执行了一次 make 命令，
# 这是一个子make过程，是在根目录的makefile中递归调用了make执行makefile
# 这个子过程 sub-make ，就是在使用 O= 选项时才使用的
# 目的也是将 KBUILD_OUTPUT 路径传入
sub-make: FORCE
	$(if $(KBUILD_VERBOSE:1=),@)$(MAKE) -C $(KBUILD_OUTPUT) \
	KBUILD_SRC=$(CURDIR) \
	KBUILD_EXTMOD="$(KBUILD_EXTMOD)" -f $(CURDIR)/Makefile \
	$(filter-out _all sub-make,$(MAKECMDGOALS))

# skip-makefile 是用来标记目标是否已经执行完成
# Leave processing to above invocation of make
skip-makefile := 1
endif # ifneq ($(KBUILD_OUTPUT),)
endif # ifeq ($(KBUILD_SRC),)

# 没有指定输出目录， skip-Makefile 为空，这个分支会进入，开始分类执行
# 这个ifeq 一直到文件最后，几乎包含所有的makefile文件内容
# 没有指定输出目录 O= ，那在这里处理的时候，还是在顶层 Make 中的
# 并非子make过程，之后在走了 sub-make 流程时，这里才是子make过程
# We process the rest of the Makefile if this is the final invocation of make
ifeq ($(skip-makefile),)

# 如果构建一个外部模块，我们不关心all:规则，而是_all依赖于 modules
# If building an external module we do not care about the all: rule
# but instead _all depend on modules
# 声明一个伪目标 all
# 如果外部模块定义为空，则 _all 依赖于 all
# 否则依赖于 modules
# makefile 中出现同名目标
# 同名依赖： 所有的依赖将合并在一起，成为目标的最终依赖
# 同名命令： 当多处出现同一目标的命令时，make将发出警告；所有之前定义的命令被最
# 后定义的命令取代
#
# 所以，这里的 _all 依赖了 all
# all 又在下面依赖了 vmlinx， arch/arm/Makefile 中 all 又依赖了 zImage
# 所以，最终的 all 依赖了 vmlinux zImage
PHONY += all
ifeq ($(KBUILD_EXTMOD),)
_all: all
else
_all: modules
endif

# if KBUILD_SRC被定义，则srctree=KBUILD_SRC,else = CURDIR
# objtree 设置为当前目录，目标存放的目录
srctree		:= $(if $(KBUILD_SRC),$(KBUILD_SRC),$(CURDIR))
objtree		:= $(CURDIR)
src		:= $(srctree)
obj		:= $(objtree)

# 当前目录找不到源文件时的搜索目录
VPATH		:= $(srctree)$(if $(KBUILD_EXTMOD),:$(KBUILD_EXTMOD))

# 导出变量给子make使用
export srctree objtree VPATH


# SUBARCH 是用来判断构建主机的架构，如 x86
# 下面的 ARCH 是需要构建的目标主机架构，如果命令行上指定了 ARCH=um
# 则会使用命令行指定的 ARCH 进行构建。
# SUBARCH tells the usermode build what the underlying arch is.  That is set
# first, and if a usermode build is happening, the "ARCH=um" on the command
# line overrides the setting of ARCH below.  If a native build is happening,
# then ARCH is assigned, getting whatever value it gets normally, and 
# SUBARCH is subsequently ignored.

SUBARCH := $(shell uname -m | sed -e s/i.86/i386/ -e s/sun4u/sparc64/ \
				  -e s/arm.*/arm/ -e s/sa110/arm/ \
				  -e s/s390x/s390/ -e s/parisc64/parisc/ \
				  -e s/ppc.*/powerpc/ -e s/mips.*/mips/ \
				  -e s/sh[234].*/sh/ )

# 交叉编译和选择不同的gcc/bin-utils集
# Cross compiling and selecting different set of gcc/bin-utils
# ---------------------------------------------------------------------------
#
# 对其他体系结构进行交叉编译时，应将ARCH设置为目标体系结构。(见arch/*的可能性)。
# 可用的 ARCH 设置都在目录 arch/ 中
# ARCH可以在make调用时设置:
# make ARCH=ia64
# 另一种方法是在环境中设置ARCH。
# 默认ARCH是执行make的主机。
# When performing cross compilation for other architectures ARCH shall be set
# to the target architecture. (See arch/* for the possibilities).
# ARCH can be set during invocation of make:
# make ARCH=ia64
# Another way is to have ARCH set in the environment.
# The default ARCH is the host where make is executed.

# CROSS_COMPILE指定编译期间使用的所有可执行文件的前缀。只有gcc和相关的bin-utils
# 可执行文件使用$(CROSS_COMPILE)作为前缀。
# CROSS_COMPILE可以在命令行上设置
# make CROSS_COMPILE=ia64-linux-
# 也可以在环境中设置CROSS_COMPILE。
# 第三种选择是将设置存储在.config中，以便配置的内核构建目录中的普通“make”始终使
# 用该设置。
# CROSS_COMPILE的默认值是不给可执行文件加上前缀。
# 注意:一些体系结构在arch/*/Makefile中赋值CROSS_COMPILE
# CROSS_COMPILE specify the prefix used for all executables used
# during compilation. Only gcc and related bin-utils executables
# are prefixed with $(CROSS_COMPILE).
# CROSS_COMPILE can be set on the command line
# make CROSS_COMPILE=ia64-linux-
# Alternatively CROSS_COMPILE can be set in the environment.
# A third alternative is to store a setting in .config so that plain
# "make" in the configured kernel build directory always uses that.
# Default value for CROSS_COMPILE is not to prefix executables
# Note: Some architectures assign CROSS_COMPILE in their arch/*/Makefile
# 这里的代码中，直接设置了 ARCH = arm， 所以在执行命令时，直接 make 就可以
export KBUILD_BUILDHOST := $(SUBARCH)
# ARCH		?= $(SUBARCH)
ARCH		= arm
CROSS_COMPILE	?= $(CONFIG_CROSS_COMPILE:"%"=%)

# 这部分的代码，linux官方源码中为如下:
# export KBUILD_BUILDHOST := $(SUBARCH)
# ARCH        ?= $(SUBARCH)
# CROSS_COMPILE   ?= $(CONFIG_CROSS_COMPILE:"%"=%)
# 在未指定 ARCH 时，默认使用 SUBARCH 进行构建
#
# 到这里，基本就能了解如何编译linux内核了，前面的很多参数都不是必须的，
# 比如命令回显，gcc 选项，中间文件输出目录等，可选的
# 重要的两个 ARCH 和 CROSS_COMPILE ，确定了目标架构 和 交叉编译器
# 当然了，对于目标架构为 x86 的，基本都是不用管的
# 以 arm 为例，首选确认架构 ARCH=arm ，再确认交叉编译器
# 还有需要确认使用的配置文件 defconfig  ->  arch/arm/configs/
# 先执行配置命令
# make ARCH=arm CROSS_COMPILE=../../arm-linux-gnueabi- exynos4_defconfig
# 再进行编译 make ARCH=arm CROSS_COMPILE=dir/to/arm-linux-gnueabi-
#

# 在compile.h中呈现的体系结构
# UTS_MACHINE 是 uts_namespace 中使用，当前内核运行的架构
# SRCARCH 是 arch 目录下，目标架构对应的目录，如 arm 对应目录 arm
# Architecture as present in compile.h
UTS_MACHINE 	:= $(ARCH)
SRCARCH 	:= $(ARCH)

# 下面几个是对于 arch 的特殊处理，ARCH 与对应的目录名不一致，调整一下
# Additional ARCH settings for x86
ifeq ($(ARCH),i386)
        SRCARCH := x86
endif
ifeq ($(ARCH),x86_64)
        SRCARCH := x86
endif

# Additional ARCH settings for sparc
ifeq ($(ARCH),sparc32)
       SRCARCH := sparc
endif
ifeq ($(ARCH),sparc64)
       SRCARCH := sparc
endif

# Additional ARCH settings for sh
ifeq ($(ARCH),sh64)
       SRCARCH := sh
endif

# Additional ARCH settings for tile
ifeq ($(ARCH),tilepro)
       SRCARCH := tile
endif
ifeq ($(ARCH),tilegx)
       SRCARCH := tile
endif

# hdr-arch 与架构相关的特殊头文件目录，后面处理时会使用
# Where to locate arch specific headers
hdr-arch  := $(SRCARCH)

# 默认的配置文件，一般就是 .config
KCONFIG_CONFIG	?= .config
export KCONFIG_CONFIG

# kbuild 使用的 SHELL , 一般最终执行结果就是 /bin/bash 或 /bin/sh
# SHELL used by kbuild
CONFIG_SHELL := $(shell if [ -x "$$BASH" ]; then echo $$BASH; \
	  else if [ -x /bin/bash ]; then echo /bin/bash; \
	  else echo sh; fi ; fi)

# 主机上的一些工具、配置，编译主机工具软件时会使用
HOSTCC       = gcc
HOSTCXX      = g++
HOSTCFLAGS   = -Wall -Wmissing-prototypes -Wstrict-prototypes -O2 -fomit-frame-pointer
HOSTCXXFLAGS = -O2

# 决定是构建内置的、模块化的还是两者兼而有之。
# 通常，只做内置的。
# Decide whether to build built-in, modular, or both.
# Normally, just do built-in.

KBUILD_MODULES :=
KBUILD_BUILTIN := 1

#	如果只编译 modules ，就不编译 built-in 了。
#	MAKECMDGOALS 是 make内置变量，包含了命令行上指定的构建目标
#	下面的判断是命令行上只指定了 modules 参数，也就是只构建 modules
#
#	If we have only "make modules", don't compile built-in objects.
#	When we're building modules with modversions, we need to consider
#	the built-in objects during the descend as well, in order to
#	make sure the checksums are up to date before we record them.

# 如果只编译 modules ，应该是不需要都编译的，
# 这里判断了 CONFIG_MODVERSIONS 配置，CONFIG_MODVERSIONS 在编译内核时，会生成模
# 块版本信息，会在模块文件名中包含模块的版本号，区分不同版本的模块。
# 本代码中并未配置
ifeq ($(MAKECMDGOALS),modules)
  KBUILD_BUILTIN := $(if $(CONFIG_MODVERSIONS),1)
endif

#   如果使用的是 "make <whatever> modules" ，那就要编译 modules
#   只是“make”或“make all”也应该构建模块
#	If we have "make <whatever> modules", compile modules
#	in addition to whatever we do anyway.
#	Just "make" or "make all" shall build modules as well

ifneq ($(filter all _all modules,$(MAKECMDGOALS)),)
  KBUILD_MODULES := 1
endif

ifeq ($(MAKECMDGOALS),)
  KBUILD_MODULES := 1
endif

# 导出了一些变量，供子make使用，特别留意 KBUILD_SRC
# 如果使用了 O= 选项，那一定需要经过 sub-make 目标的，
# 能到这里，说明 KBUILD_SRC 不是空的，
# 也就是这个时候是在子 make 中调用的
# 前面的处理中 ifeq ($(KBUILD_SRC),) 判断了是否为空
# 为空则进行编译路径相关的处理，最终会调用 sub-make 目标，
# sub-make 中调用了 make 过程，并将 KBUILD_SRC 做为参数传递了，
# sub-make 中的 make 过程会再次进入到本makefile中，
# 此时 KBUILD_SRC 已经指向了内核源码路径，不会再进入
# ifeq ($(KBUILD_SRC),) 处理过程了
export KBUILD_MODULES KBUILD_BUILTIN
export KBUILD_CHECKSRC KBUILD_SRC KBUILD_EXTMOD

# 美化输出
# ---------------------------------------------------------------------------
# 通常，我们在执行命令之前回显整个命令。
# 通过echo $($(quiet)$(cmd))，我们现在有可能设置$(quiet)来选择其他形式的输出，例
# 如:
# 		quiet_cmd_cc_o_c = Compiling $(RELDIR)/$@
# 		cmd_cc_o_c       = $(CC) $(c_flags) -c -o $@ $<
#
# 如果$(quiet)为空，则打印整个命令。
# 如果设置为“quiet_”，则只打印短版本。
# 如果将其设置为“silent_”，则根本不会打印任何内容，因为变量$(silent_cmd_cc_o_c)
# 不存在。
#
# 一个简单的变体是使用$(Q)作为命令的前缀——这对于应该以非详细模式隐藏的命令很有用。
#
# 	$(Q)ln $@ :<
#
# 如果KBUILD_VERBOSE等于0，那么上面的命令将被隐藏。
# 如果KBUILD_VERBOSE等于1，则显示上述命令。
#
# Beautify output
# ---------------------------------------------------------------------------
#
# Normally, we echo the whole command before executing it. By making
# that echo $($(quiet)$(cmd)), we now have the possibility to set
# $(quiet) to choose other forms of output instead, e.g.
#
#         quiet_cmd_cc_o_c = Compiling $(RELDIR)/$@
#         cmd_cc_o_c       = $(CC) $(c_flags) -c -o $@ $<
#
# If $(quiet) is empty, the whole command will be printed.
# If it is set to "quiet_", only the short version will be printed. 
# If it is set to "silent_", nothing will be printed at all, since
# the variable $(silent_cmd_cc_o_c) doesn't exist.
#
# A simple variant is to prefix commands with $(Q) - that's useful
# for commands that shall be hidden in non-verbose mode.
#
#	$(Q)ln $@ :<
#
# If KBUILD_VERBOSE equals 0 then the above command will be hidden.
# If KBUILD_VERBOSE equals 1 then the above command is displayed.

# 在Makefile中，@符号用于控制命令的输出。当命令前面有@符号时，表示不显示这条命令
# 的实际执行情况，即不在终端输出执行该命令的具体内容，只执行命令本身。这样可以使
# 得执行Makefile时的输出更加清晰和简洁，只显示最终的结果，而不会干扰到其他信息。
#
# 因此，@符号在Makefile中常用于隐藏执行命令的具体细节，使得执行过程更加简洁。
#
# 如果make执行时，带入make参数“-n”或“--just-print”，那么其只是显示命令，但不会执
# 行命令，这个功能很有利于我们调试我们的Makefile，看看我们书写的命令是执行起来是
# 什么样子的或是什么顺序的。 
# 而make参数“-s”或“--slient”则是全面禁止命令的显示。
#
ifeq ($(KBUILD_VERBOSE),1)
  quiet =
  Q =
else
  quiet=quiet_
  Q = @
endif

# 如果命令中有 -s 选项，则静默模式
# If the user is running make -s (silent mode), suppress echoing of
# commands

ifneq ($(filter s% -s%,$(MAKEFLAGS)),)
  quiet=silent_
endif

export quiet Q KBUILD_VERBOSE


# 查找相对于内核src根目录的make include文件
# Look for make include files relative to root of kernel src
MAKEFLAGS += --include-dir=$(srctree)

# 编译过程有两个非常重要的文件  scripts/Kbuild.include scripts/Makefile.build
# scripts/Kbuild.include 中定义了通用的函数和变量
# scripts/Makefile.build 中定义了编译的规则
#
# 参考网上的一个介绍分析:https://www.cnblogs.com/jianhua1992/p/16852800.html
# 单独编译单个 .o 文件的编译过程
# 顶层Makefile 中，这样定义的：
# %.o: %.c prepare scripts FORCE
# 	$(Q)$(MAKE) $(build)=$(build-dir) $(target-dir)$(notdir $@)
# $(MAKE) 扩展为 make 指令
# $(build) 在 scripts/Kbuild.include 中，扩展为
# # $(Q)$(MAKE) $(build)=dir
# build := -f $(if $(KBUILD_SRC),$(srctree)/)scripts/Makefile.build obj
# build := -f scripts/Makefile.build obj
# $(build-dir) 扩展为 .c 源码的路径
# $(target-dir) 扩展为 .o 存放的路径
# $(notdir $@) 扩展为 没有路径的 .o 文件名，如 mem.o
#
# 这样，顶层 Makefile 的命令就变成了
#     %.o: %.c
#             make -f scripts/Makefile.build obj=drivers/char drivers/char/mem.o
#
# 这次的调用使用的是 scripts/Makefile.build
# 精简后的规则为 ：
#     $(obj)/%.o: $(src)/%.c
#             $(call if_changed_rule,cc_o_c)
#
# if_changed_rule 会把 cc_o_c 再组装成 rule_cc_o_c
#
# rule_cc_o_c 精简后，规则为：
#     rule_cc_o_c
#     		$(call echo-cmd,cc_o_c) $(cmd_cc_o_c)
#
# cmd_cc_o_c 命令就是最后执行的命令：
# 	  cmd_cc_o_c = $(CC) $(c_flags) -c -o $@ $<
#
#
# 我们需要一些通用的定义(不要试图重制文件)。
# scripts/Kbuild.include 中定义了大量的通用函数和变量，在整个 makefile 中有大量
# 的应用，如 if_changed if_changed_rule
# We need some generic definitions (do not try to remake the file).
$(srctree)/scripts/Kbuild.include: ;
include $(srctree)/scripts/Kbuild.include

# 编译器命令，编译选项
# Make variables (CC, etc...)

AS		= $(CROSS_COMPILE)as
LD		= $(CROSS_COMPILE)ld
CC		= $(CROSS_COMPILE)gcc
CPP		= $(CC) -E
AR		= $(CROSS_COMPILE)ar
NM		= $(CROSS_COMPILE)nm
STRIP		= $(CROSS_COMPILE)strip
OBJCOPY		= $(CROSS_COMPILE)objcopy
OBJDUMP		= $(CROSS_COMPILE)objdump
AWK		= awk
GENKSYMS	= scripts/genksyms/genksyms
INSTALLKERNEL  := installkernel
DEPMOD		= /sbin/depmod
PERL		= perl
CHECK		= sparse

CHECKFLAGS     := -D__linux__ -Dlinux -D__STDC__ -Dunix -D__unix__ \
		  -Wbitwise -Wno-return-void $(CF)
CFLAGS_MODULE   =
AFLAGS_MODULE   =
LDFLAGS_MODULE  =
CFLAGS_KERNEL	=
AFLAGS_KERNEL	=
CFLAGS_GCOV	= -fprofile-arcs -ftest-coverage


# 当必须引用include/目录时，请使用LINUXINCLUDE。
# 需要与O=选项兼容
# 包含了几个主要的 include/ 目录
# arch/arm/include/
# arch/arm/include/generated/
# include/
# include/generated/autoconf.h
# Use LINUXINCLUDE when you must reference the include/ directory.
# Needed to be compatible with the O= option
LINUXINCLUDE    := -I$(srctree)/arch/$(hdr-arch)/include \
                   -Iarch/$(hdr-arch)/include/generated -Iinclude \
                   $(if $(KBUILD_SRC), -I$(srctree)/include) \
                   -include $(srctree)/include/linux/kconfig.h

KBUILD_CPPFLAGS := -D__KERNEL__

KBUILD_CFLAGS   := -Wall -Wundef -Wstrict-prototypes -Wno-trigraphs \
		   -fno-strict-aliasing -fno-common \
		   -Werror-implicit-function-declaration \
		   -Wno-format-security \
		   -fno-delete-null-pointer-checks
KBUILD_AFLAGS_KERNEL :=
KBUILD_CFLAGS_KERNEL :=
KBUILD_AFLAGS   := -D__ASSEMBLY__
KBUILD_AFLAGS_MODULE  := -DMODULE
KBUILD_CFLAGS_MODULE  := -DMODULE
KBUILD_LDFLAGS_MODULE := -T $(srctree)/scripts/module-common.lds

# include/config/kernel.release 中包含了版本号字符串  如 3.5.0
# KERNELVERSION 拼接版本号字符， 拼接后  3.5.0
# Read KERNELRELEASE from include/config/kernel.release (if it exists)
KERNELRELEASE = $(shell cat include/config/kernel.release 2> /dev/null)
KERNELVERSION = $(VERSION)$(if $(PATCHLEVEL),.$(PATCHLEVEL)$(if $(SUBLEVEL),.$(SUBLEVEL)))$(EXTRAVERSION)

export VERSION PATCHLEVEL SUBLEVEL KERNELRELEASE KERNELVERSION
export ARCH SRCARCH CONFIG_SHELL HOSTCC HOSTCFLAGS CROSS_COMPILE AS LD CC
export CPP AR NM STRIP OBJCOPY OBJDUMP
export MAKE AWK GENKSYMS INSTALLKERNEL PERL UTS_MACHINE
export HOSTCXX HOSTCXXFLAGS LDFLAGS_MODULE CHECK CHECKFLAGS

export KBUILD_CPPFLAGS NOSTDINC_FLAGS LINUXINCLUDE OBJCOPYFLAGS LDFLAGS
export KBUILD_CFLAGS CFLAGS_KERNEL CFLAGS_MODULE CFLAGS_GCOV
export KBUILD_AFLAGS AFLAGS_KERNEL AFLAGS_MODULE
export KBUILD_AFLAGS_MODULE KBUILD_CFLAGS_MODULE KBUILD_LDFLAGS_MODULE
export KBUILD_AFLAGS_KERNEL KBUILD_CFLAGS_KERNEL
export KBUILD_ARFLAGS

# 在编译树外模块时，将MODVERDIR放在模块树中，而不是内核树中。内核树甚至可能是只
# 读的。
# When compiling out-of-tree modules, put MODVERDIR in the module
# tree rather than in the kernel tree. The kernel tree might
# even be read-only.
export MODVERDIR := $(if $(KBUILD_EXTMOD),$(firstword $(KBUILD_EXTMOD))/).tmp_versions

# 使用 find 时，忽略的文件类型
# Files to ignore in find ... statements

RCS_FIND_IGNORE := \( -name SCCS -o -name BitKeeper -o -name .svn -o -name CVS \
		   -o -name .pc -o -name .hg -o -name .git \) -prune -o
export RCS_TAR_IGNORE := --exclude SCCS --exclude BitKeeper --exclude .svn \
			 --exclude CVS --exclude .pc --exclude .hg --exclude .git

# ===========================================================================
# 由*config目标和构建目标共享的规则
# Rules shared between *config targets and build targets

# $(Q)$(MAKE) $(build)=scripts/basic 展开后为
# make -f ./scripts/Makefile.build obj=scripts/basic
# $(build) 的定义在 scripts/Kbuild.include 中,该文件已被包含了
# 这个命令，就是使用 scripts/Makefile.build 编译 scripts/basic/ 下的 Makefile
# Basic helpers built in scripts/
PHONY += scripts_basic
scripts_basic:
	$(Q)$(MAKE) $(build)=scripts/basic
	$(Q)rm -f .tmp_quiet_recordmcount

# 为了避免使用任何隐式规则，定义一个空命令。
# To avoid any implicit rule to kick in, define an empty command.
scripts/basic/%: scripts_basic ;

# 如果使用单独的输出目录，outputmakefile会在输出目录中生成一个Makefile。这允许在
# 输出目录中方便地使用make。
# 如果没有指定 O= 选项，那 KBUILD_SRC 是空，这里就不需要任何处理
PHONY += outputmakefile
# outputmakefile generates a Makefile in the output directory, if using a
# separate output directory. This allows convenient use of make in the
# output directory.
outputmakefile:
ifneq ($(KBUILD_SRC),)
	$(Q)ln -fsn $(srctree) source
	$(Q)$(CONFIG_SHELL) $(srctree)/scripts/mkmakefile \
	    $(srctree) $(objtree) $(VERSION) $(PATCHLEVEL)
endif

# 支持使用 asm-generic 中的通用头文件
# 在 include/asm-generic/ 目录中，包含了与架构无关的头文件，具体的平台代码可以无
# 需自行定义，直接引用就可以
# 但在linux内核中，不建议平台代码直接引用 include/asm-generic/ 下的头文件，应该通过
# 平台相关的头文件来间接包含，用于间接包含的头文件，被放在 arch/$(ARCH)
# /include/generated/asm 中，这个目录是构建时 asm-generic 目标生成的
# 这里的处理，是指定 scripts/Makefile.asm-generic 进行处理，处理目录在
# arch/$(ARCH)/include/generated/asm 中，具体处理方式，在 Makefile.asm-generic
# Support for using generic headers in asm-generic
PHONY += asm-generic
asm-generic:
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.asm-generic \
	            obj=arch/$(SRCARCH)/include/generated/asm

# To make sure we do not include .config for any of the *config targets
# catch them early, and hand them over to scripts/kconfig/Makefile
# It is allowed to specify more targets when calling make, including
# mixing *config targets and build targets.
# For example 'make oldconfig all'.
# Detect when mixed targets is specified, and make a second invocation
# of make so .config is not included in this case either (for *config).

# no-dot-config-targets 是这些目标和 .config 完全无关的。
# 这些目标的构建，是不需要 .config 的
no-dot-config-targets := clean mrproper distclean \
			 cscope gtags TAGS tags help %docs check% coccicheck \
			 include/linux/version.h headers_% archheaders archscripts \
			 kernelversion %src-pkg

# config-targets 是和 .config 有关的目标，有两种：
# 一种是产生 .config 文件，如 menuconfig, oldconfig,defconfig 等；
# 另一种是需要使用 .config 的目标，需要使用 .config 文件中的配置作为构建自己的依
# 据，如 all,vmlinux,modules,bzImage 等
config-targets := 0
# mixed-targets 混合目标，表示在执行 make 命令时，同时指定了配置目标和其他的目标，
# 而不仅仅是指配置目标和构建相混合。如 make defconfig vmlinux
mixed-targets  := 0
# 是否需要 .config 文件
dot-config     := 1

# 判断构建目标中是否包含有 no-dot-config-targets
# 如果包含有这些目标，则将 make 命令中所有的这些目标去除，并判断剩余的目标是否为
# 空，如果为空，那说明命令行目标中，只有这些不依赖 .config 的目标
# 就将 dot-config 设为 0
ifneq ($(filter $(no-dot-config-targets), $(MAKECMDGOALS)),)
	ifeq ($(filter-out $(no-dot-config-targets), $(MAKECMDGOALS)),)
		dot-config := 0
	endif
endif

# 是否需要编译外部模块，为空则不编译，进入此判断中
# 再判断命令中指定的目标中是否有 config 或者 %config 这样的目标，
# 如果有的话，就将 config-targets 设为 1
# 接着再判断是否还有除了 config 或 %config 之外的目标，如果有，则是一个 混合目标
ifeq ($(KBUILD_EXTMOD),)
        ifneq ($(filter config %config,$(MAKECMDGOALS)),)
                config-targets := 1
                ifneq ($(filter-out config %config,$(MAKECMDGOALS)),)
                        mixed-targets := 1
                endif
        endif
endif

# 混合目标处理，一般不会这样调用make，不做分析了
ifeq ($(mixed-targets),1)
# ===========================================================================
# We're called with mixed targets (*config and build targets).
# Handle them one by one.

%:: FORCE
	$(Q)$(MAKE) -C $(srctree) KBUILD_SRC= $@

else
# config 目标处理，执行 make *config 时，会进入这里处理
ifeq ($(config-targets),1)
# ===========================================================================
# *config targets only - make sure prerequisites are updated, and descend
# in scripts/kconfig to make the *config target

# Read arch specific Makefile to set KBUILD_DEFCONFIG as needed.
# KBUILD_DEFCONFIG may point out an alternative default configuration
# used for 'make defconfig'
include $(srctree)/arch/$(SRCARCH)/Makefile
export KBUILD_DEFCONFIG KBUILD_KCONFIG

config: scripts_basic outputmakefile FORCE
	$(Q)mkdir -p include/linux include/config
	$(Q)$(MAKE) $(build)=scripts/kconfig $@

# 任何形式的 xxxconfig 都会调用到这里
# 在执行 make 或 make all 等命令时，会执行到下面的 silentoldconfig 相关的处理
# 也会在子make过程中调用到这里
# 这里的处理，又会在 scripts/kconfig/ 目录中完成
# 下面指令扩展后, 如果是 silentoldconfig, 则为
# make -f scripts/Makefile.build obj=scripts/kconfig silentoldconfig
%config: scripts_basic outputmakefile FORCE
	$(Q)mkdir -p include/linux include/config
	$(Q)$(MAKE) $(build)=scripts/kconfig $@

else
# ===========================================================================
# 构建目标，包括 vmlinux, 架构特殊的目标， clean 还有一些其他目标
# 一般来说，除了 *config 目标之外的所有目标
# Build targets only - this includes vmlinux, arch specific targets, clean
# targets and others. In general all targets except *config targets.

# 是否编译外部模块，编译外部模块，这里就不处理了
# 默认构建 _all 或 vmlinux 等目标，这个判断会进入
ifeq ($(KBUILD_EXTMOD),)
# Additional helpers built in scripts/
# Carefully list dependencies so we do not try to build scripts twice
# in parallel
PHONY += scripts
scripts: scripts_basic include/config/auto.conf include/config/tristate.conf
	$(Q)$(MAKE) $(build)=$(@)

# Objects we will link into vmlinux / subdirs we need to visit
init-y		:= init/
drivers-y	:= drivers/ sound/ firmware/
net-y		:= net/
libs-y		:= lib/
core-y		:= usr/
endif # KBUILD_EXTMOD

ifeq ($(dot-config),1)
# Read in config
# -include 进行声明，这个文件即使不存在，也不会报错退出
-include include/config/auto.conf

# 外部模块编译需要单独处理
ifeq ($(KBUILD_EXTMOD),)
# 构建非外部模块时，包含 auto.conf.cmd 文件
# Read in dependencies to all Kconfig* files, make sure to run
# oldconfig if changes are detected.
-include include/config/auto.conf.cmd

# 如果不是第一次构建，已经生成过 include/config/auto.conf.cmd
# 这里会直接包含成功，这个文件中还定义了 include/config/auto.conf 目标
# include/config/auto.conf: \
# 	$(deps_config)
#
# $(deps_config) 是各个目录的 Kconfig
# 这样就可以用于判断 Kconfig 是否更新过了

# 避免隐含规则，定义一个空命令
# To avoid any implicit rule to kick in, define an empty command
$(KCONFIG_CONFIG) include/config/auto.conf.cmd: ;

# $(KCONFIG_CONFIG) 就是 根目录下的 .config
# 在第一次编译内核时， .config 已经有了，但是 auto.conf.cmd 还没有
# 但是根据上面的目标， auto.conf.cmd 是空命令，也就是默认是最新的
# 所以就执行 下面的处理了
# 	$(Q)$(MAKE) -f $(srctree)/Makefile silentoldconfig
# 真正的 silentoldconfig 目标是在 scripts/kconfig/Makefile 中的，
# 但这里执行的时候，-f 指定了 顶层Makefile ，而且目标是 silentoldconfig
# 所以，就会执行 %config 目标
# 这里执行了 make 指令，指定了顶层Makefile，调用了子make过程
# 所以上面的 config-targets 在子make过程中是设为 1 的
# If .config is newer than include/config/auto.conf, someone tinkered
# with it and forgot to run make oldconfig.
# if auto.conf.cmd is missing then we are probably in a cleaned tree so
# we execute the config step to be sure to catch updated Kconfig files
# 这里调用的 silentoldconfig 目标，最终调用到了 scripts/kconfig/Makefile 中的
# silentoldconfig 目标，silentoldconfig 中会生成主机工具 conf
# conf 根据.config 配置，最终生成 include/config/auto.conf include/config/auto.conf.cmd
# include/config/tristate.conf include/generated/autoconf.h
include/config/%.conf: $(KCONFIG_CONFIG) include/config/auto.conf.cmd
	$(Q)$(MAKE) -f $(srctree)/Makefile silentoldconfig
else
# external modules needs include/generated/autoconf.h and include/config/auto.conf
# but do not care if they are up-to-date. Use auto.conf to trigger the test
PHONY += include/config/auto.conf

include/config/auto.conf:
	$(Q)test -e include/generated/autoconf.h -a -e $@ || (		\
	echo;								\
	echo "  ERROR: Kernel configuration is invalid.";		\
	echo "         include/generated/autoconf.h or $@ are missing.";\
	echo "         Run 'make oldconfig && make prepare' on kernel src to fix it.";	\
	echo;								\
	/bin/false)

endif # KBUILD_EXTMOD

else
# Dummy target needed, because used as prerequisite
include/config/auto.conf: ;
endif # $(dot-config)

# all: 目标 是默认目标，当命令行上没有指定任何目标时调用
# 这允许用户只发出'make'来构建包含模块的内核
# 默认为vmlinux，但arch makefile通常会添加更多的目标
# The all: target is the default when no target is given on the
# command line.
# This allow a user to issue only 'make' to build a kernel including modules
# Defaults to vmlinux, but the arch makefile usually adds further targets
all: vmlinux

# 代码尺寸优化，优化等级设置为优化代码尺寸
# 否则设置优化等级为 O2
ifdef CONFIG_CC_OPTIMIZE_FOR_SIZE
KBUILD_CFLAGS	+= -Os
else
KBUILD_CFLAGS	+= -O2
endif

# 包含架构相关的 Makefile
# 其中可能会另外添加编译目标，如 arm 架构中，默认会编译 zImage
# uImage 目标也是架构相关的目标
include $(srctree)/arch/$(SRCARCH)/Makefile

ifdef CONFIG_READABLE_ASM
# Disable optimizations that make assembler listings hard to read.
# reorder blocks reorders the control in the function
# ipa clone creates specialized cloned functions
# partial inlining inlines only parts of functions
KBUILD_CFLAGS += $(call cc-option,-fno-reorder-blocks,) \
                 $(call cc-option,-fno-ipa-cp-clone,) \
                 $(call cc-option,-fno-partial-inlining)
endif

# -Wframe-larger-than是GCC编译器的一个参数，用于设置函数栈帧的最大大小。这个参数
#  可以用来指定函数栈帧的最大字节数。例如，可以使用-Wframe-larger-than=2048来设
#  置最大栈帧大小为2048字节。
#  本代码中，默认设置为 1024
ifneq ($(CONFIG_FRAME_WARN),0)
KBUILD_CFLAGS += $(call cc-option,-Wframe-larger-than=${CONFIG_FRAME_WARN})
endif

# 强制gcc即使在有bug的发行版中也能正常工作
# Force gcc to behave correct even for buggy distributions
ifndef CONFIG_CC_STACKPROTECTOR
KBUILD_CFLAGS += $(call cc-option, -fno-stack-protector)
endif

# 此警告在常规构建中产生太多告警。使用make W=1来启用此警告(参见
# scripts/Makefile.build)
# This warning generated too much noise in a regular build.
# Use make W=1 to enable this warning (see scripts/Makefile.build)
KBUILD_CFLAGS += $(call cc-disable-warning, unused-but-set-variable)

# gcc 的 -fno-omit-frame-pointer 参数用于阻止优化器在生成代码时忽略帧指针。这意
# 味着编译器会生成代码来保留帧指针，这对于调试和性能分析非常有用。
#
# 而 -fno-optimize-sibling-calls 参数用于阻止优化器对尾递归调用进行优化。尾递归
# 调用是指函数的最后一个操作是调用自身的情况。使用这个参数可以防止优化器对尾递归
# 调用进行优化，这在一些情况下可能有助于调试并保留函数调用栈的结构。
ifdef CONFIG_FRAME_POINTER
KBUILD_CFLAGS	+= -fno-omit-frame-pointer -fno-optimize-sibling-calls
else
# Some targets (ARM with Thumb2, for example), can't be built with frame
# pointers.  For those, we don't have FUNCTION_TRACER automatically
# select FRAME_POINTER.  However, FUNCTION_TRACER adds -pg, and this is
# incompatible with -fomit-frame-pointer with current GCC, so we don't use
# -fomit-frame-pointer with FUNCTION_TRACER.
ifndef CONFIG_FUNCTION_TRACER
KBUILD_CFLAGS	+= -fomit-frame-pointer
endif
endif

# gcc 的 -g 参数是用来生成调试信息的。它告诉编译器在生成可执行文件时，包含额外的
# 调试信息，以便在调试过程中能够更好地跟踪代码的执行。
ifdef CONFIG_DEBUG_INFO
KBUILD_CFLAGS	+= -g
KBUILD_AFLAGS	+= -gdwarf-2
endif

ifdef CONFIG_DEBUG_INFO_REDUCED
KBUILD_CFLAGS 	+= $(call cc-option, -femit-struct-debug-baseonly)
endif

ifdef CONFIG_FUNCTION_TRACER
KBUILD_CFLAGS	+= -pg
ifdef CONFIG_DYNAMIC_FTRACE
	ifdef CONFIG_HAVE_C_RECORDMCOUNT
		BUILD_C_RECORDMCOUNT := y
		export BUILD_C_RECORDMCOUNT
	endif
endif
endif

# We trigger additional mismatches with less inlining
ifdef CONFIG_DEBUG_SECTION_MISMATCH
KBUILD_CFLAGS += $(call cc-option, -fno-inline-functions-called-once)
endif

# arch Makefile may override CC so keep this after arch Makefile is included
NOSTDINC_FLAGS += -nostdinc -isystem $(shell $(CC) -print-file-name=include)
CHECKFLAGS     += $(NOSTDINC_FLAGS)

# warn about C99 declaration after statement
KBUILD_CFLAGS += $(call cc-option,-Wdeclaration-after-statement,)

# disable pointer signed / unsigned warnings in gcc 4.0
KBUILD_CFLAGS += $(call cc-disable-warning, pointer-sign)

# disable invalid "can't wrap" optimizations for signed / pointers
KBUILD_CFLAGS	+= $(call cc-option,-fno-strict-overflow)

# conserve stack if available
KBUILD_CFLAGS   += $(call cc-option,-fconserve-stack)

# use the deterministic mode of AR if available
KBUILD_ARFLAGS := $(call ar-option,D)

# check for 'asm goto'
ifeq ($(shell $(CONFIG_SHELL) $(srctree)/scripts/gcc-goto.sh $(CC)), y)
	KBUILD_CFLAGS += -DCC_HAVE_ASM_GOTO
endif

# Add user supplied CPPFLAGS, AFLAGS and CFLAGS as the last assignments
# But warn user when we do so
warn-assign = \
$(warning "WARNING: Appending $$K$(1) ($(K$(1))) from $(origin K$(1)) to kernel $$$(1)")

ifneq ($(KCPPFLAGS),)
        $(call warn-assign,CPPFLAGS)
        KBUILD_CPPFLAGS += $(KCPPFLAGS)
endif
ifneq ($(KAFLAGS),)
        $(call warn-assign,AFLAGS)
        KBUILD_AFLAGS += $(KAFLAGS)
endif
ifneq ($(KCFLAGS),)
        $(call warn-assign,CFLAGS)
        KBUILD_CFLAGS += $(KCFLAGS)
endif

# Use --build-id when available.
LDFLAGS_BUILD_ID = $(patsubst -Wl$(comma)%,%,\
			      $(call cc-ldoption, -Wl$(comma)--build-id,))
KBUILD_LDFLAGS_MODULE += $(LDFLAGS_BUILD_ID)
LDFLAGS_vmlinux += $(LDFLAGS_BUILD_ID)

ifeq ($(CONFIG_STRIP_ASM_SYMS),y)
LDFLAGS_vmlinux	+= $(call ld-option, -X,)
endif

# 在没有给定特定目标时构建的默认内核映像。
# KBUILD_IMAGE可以在命令行上被重定义，也可以在环境中设置
# 此外，arch/$(arch)/Makefile中的任何赋值都优先于此默认值
# arch/$(arch)/Makefile 是在前面被包含的，这里赋值使用 ?=
# 所以 arch/$(arch)/Makefile 里面的赋值优先级高
# 对于arm架构，已经被赋值为 zImage 或 xipImage
# Default kernel image to build when no specific target is given.
# KBUILD_IMAGE may be overruled on the command line or
# set in the environment
# Also any assignments in arch/$(ARCH)/Makefile take precedence over
# this default value
export KBUILD_IMAGE ?= vmlinux

# INSTALL_PATH指定将更新的内核和系统映射映像放置在何处。默认值是/boot，但您可以
# 将其设置为其他值
# 对于 arm架构，在 x86 主机编译，这里是用不到的，一般都是编译后得到 zImage
# 然后将 zImage 拷贝到目标机上运行
#
# INSTALL_PATH specifies where to place the updated kernel and system map
# images. Default is /boot, but you can set it to other values
export	INSTALL_PATH ?= /boot

#
# INSTALL_MOD_PATH为构建根所需的模块目录重定位指定MODLIB的前缀。这在makefile中没
# 有定义，但如果需要，可以将参数传递给make。
# INSTALL_MOD_PATH specifies a prefix to MODLIB for module directory
# relocations required by build roots.  This is not defined in the
# makefile but the argument can be passed to make if needed.
#

MODLIB	= $(INSTALL_MOD_PATH)/lib/modules/$(KERNELRELEASE)
export MODLIB

#
# 如果定义了INSTALL_MOD_STRIP，将导致模块在安装后被剥离。
# 如果INSTALL_MOD_STRIP为'1'，那么将使用默认选项——strip-debug。否则，将使用
# INSTALL_MOD_STRIP值作为strip命令的选项。
#  INSTALL_MOD_STRIP, if defined, will cause modules to be
#  stripped after they are installed.  If INSTALL_MOD_STRIP is '1', then
#  the default option --strip-debug will be used.  Otherwise,
#  INSTALL_MOD_STRIP value will be used as the options to the strip command.

ifdef INSTALL_MOD_STRIP
ifeq ($(INSTALL_MOD_STRIP),1)
mod_strip_cmd = $(STRIP) --strip-debug
else
mod_strip_cmd = $(STRIP) $(INSTALL_MOD_STRIP)
endif # INSTALL_MOD_STRIP=1
else
mod_strip_cmd = true
endif # INSTALL_MOD_STRIP
export mod_strip_cmd


# 不是单独编译外部模块，进入这个处理流程中
ifeq ($(KBUILD_EXTMOD),)
# core-y 的值，会在架构相关的 Makefile 中增加
# 如 arch/arm/Makefile 中
core-y		+= kernel/ mm/ fs/ ipc/ security/ crypto/ block/

# 注意，这是以 iTop4412 开发板代码测试，部分目录可能不同

# 分析各 -y 参数，并调试打印出具体的变量值
# init usr arch/arm/vfp arch/arm/kernel arch/arm/mm arch/arm/common
# arch/arm/mach-exynos arch/arm/plat-s5p arch/arm/plat-samsung kernel mm fs ipc
# security crypto block drivers sound firmware net arch/arm/lib lib
# 注意，这是以 iTop4412 开发板代码测试，部分目录可能不同
vmlinux-dirs	:= $(patsubst %/,%,$(filter %/, $(init-y) $(init-m) \
		     $(core-y) $(core-m) $(drivers-y) $(drivers-m) \
		     $(net-y) $(net-m) $(libs-y) $(libs-m)))
# $(warning vmlinux-dirs = $(vmlinux-dirs))

# 这里进行了一个排序，最终变量值
# arch/arm/common arch/arm/kernel arch/arm/lib arch/arm/mach-exynos arch/arm/mm
# arch/arm/plat-s5p arch/arm/plat-samsung arch/arm/vfp block crypto drivers
# firmware fs init ipc kernel lib mm net security sound usr
# 注意，这是以 iTop4412 开发板代码测试，部分目录可能不同
vmlinux-alldirs	:= $(sort $(vmlinux-dirs) $(patsubst %/,%,$(filter %/, \
		     $(init-n) $(init-) \
		     $(core-n) $(core-) $(drivers-n) $(drivers-) \
		     $(net-n)  $(net-)  $(libs-n)    $(libs-))))
# $(warning vmlinux-alldirs = $(vmlinux-alldirs))

# init/built-in.o
init-y		:= $(patsubst %/, %/built-in.o, $(init-y))
# $(warning init = $(init-y))

# usr/built-in.o  arch/arm/vfp/built-in.o  arch/arm/kernel/built-in.o
# arch/arm/mm/built-in.o  arch/arm/common/built-in.o
# arch/arm/mach-exynos/built-in.o  arch/arm/plat-s5p/built-in.o
# arch/arm/plat-samsung/built-in.o  kernel/built-in.o
# mm/built-in.o  fs/built-in.o  ipc/built-in.o  security/built-in.o
# crypto/built-in.o  block/built-in.o
core-y		:= $(patsubst %/, %/built-in.o, $(core-y))
# $(warning core = $(core-y))

# drivers/built-in.o  sound/built-in.o  firmware/built-in.o
drivers-y	:= $(patsubst %/, %/built-in.o, $(drivers-y))
# $(warning drivers = $(drivers-y))

# net/built-in.o
net-y		:= $(patsubst %/, %/built-in.o, $(net-y))
# $(warning net = $(net-y))

# arch/arm/lib/lib.a  lib/lib.a  arch/arm/lib/built-in.o  lib/built-in.o
libs-y1		:= $(patsubst %/, %/lib.a, $(libs-y))
libs-y2		:= $(patsubst %/, %/built-in.o, $(libs-y))
libs-y		:= $(libs-y1) $(libs-y2)
# $(warning lib = $(libs-y))

# 下面是 linux-3.0.15 内核源码中注释
# 本 Makefile 中的有些变量变了，但整体流程未变
# 下面的这些注释助于理解 
# 本版本的内核代码，将 vmlinux 的一些处理，放到了 scripts/link-vmlinux.sh 
# 脚本中，可以参考 scripts/link-vmlinux.sh 脚本中的处理
# 构建 vmlinux
# Build vmlinux
# ---------------------------------------------------------------------------
# Vmlinux是由$(Vmlinux -init)和$(Vmlinux -main)选择的对象构建的。
# 大多数 built-in.o 文件是在内核树的顶层目录中，其他的 built-in.o 在
# arch/$(arch)/Makefile 中。
# 链接时排序很重要，$(vmlinux-init) 必须放在第一位。
# vmlinux is built from the objects selected by $(vmlinux-init) and
# $(vmlinux-main). Most are built-in.o files from top-level directories
# in the kernel tree, others are specified in arch/$(ARCH)/Makefile.
# Ordering when linking is important, and $(vmlinux-init) must be first.
#
# vmlinux
#   ^
#   |
#   +-< $(vmlinux-init)
#   |   +--< init/version.o + more
#   |
#   +--< $(vmlinux-main)
#   |    +--< driver/built-in.o mm/built-in.o + more
#   |
#   +-< kallsyms.o (see description in CONFIG_KALLSYMS section)
#
# 在正常进入子目录阶段期间（uname -v）无法更新vmlinux版本，因为我们还不知道是否
# 需要更新vmlinux。
# 因此，这一步骤要等到vmlinux的最终链接完成之后才能进行（除kallsyms情况外，该步
# 骤在将符号添加到内核之前执行）。
# vmlinux version (uname -v) cannot be updated during normal
# descending-into-subdirs phase since we do not yet know if we need to
# update vmlinux.
# Therefore this step is delayed until just before final link of vmlinux -
# except in the kallsyms case where it is done just before adding the
# symbols to the kernel.
#
# System.map 是生成的所有内核符号的地址文档
# System.map is generated to document addresses of all kernel symbols

# $(head-y) 是在 arch/arm/Makefile 中设置的
# 是与架构相关的初始化代码，必须放在最前面
# arm下包含两个 arch/arm/kernel/head.S arch/arm/kernel/init_task.c
# $(init-y) 是在上面设置的 init/build-in.o
# vmlinux-main 包含了其他的内核顶层目录构建文件，上面已分析
# vmlinux-all 整合了所有的构建文件
# vmlinux-lds 是链接时用到的 lds 文件，架构相关
# vmlinux-init := $(head-y) $(init-y)
# vmlinux-main := $(core-y) $(libs-y) $(drivers-y) $(net-y)
# vmlinux-all  := $(vmlinux-init) $(vmlinux-main)
# vmlinux-lds  := arch/$(SRCARCH)/kernel/vmlinux.lds
# export KBUILD_VMLINUX_OBJS := $(vmlinux-all)
# Externally visible symbols (used by link-vmlinux.sh)
export KBUILD_VMLINUX_INIT := $(head-y) $(init-y)
export KBUILD_VMLINUX_MAIN := $(core-y) $(libs-y) $(drivers-y) $(net-y)
export KBUILD_LDS          := arch/$(SRCARCH)/kernel/vmlinux.lds
export LDFLAGS_vmlinux

# vmlinux 的编译依赖，也是编译目标
vmlinux-deps := $(KBUILD_LDS) $(KBUILD_VMLINUX_INIT) $(KBUILD_VMLINUX_MAIN)

# vmlinux 链接规则 - 也在配置KALLSYMS期间使用
# 有可能被架构代码重写 arch/$(ARCH)/Makefile
# LDFLAGS_vmlinux 在 arch/arm/Makefile 中设置了
# Final link of vmlinux
      cmd_link-vmlinux = $(CONFIG_SHELL) $< $(LD) $(LDFLAGS) $(LDFLAGS_vmlinux)
quiet_cmd_link-vmlinux = LINK    $@

# vmlinux 目标
# Vmlinux映像-包括更新的内核符号
# 这里看出生成 vmlinux 的依赖项
# 调用的 link-vmlinux 生成的 vmlinux
# Include targets which we want to
# execute if the rest of the kernel build went well.
vmlinux: scripts/link-vmlinux.sh $(vmlinux-deps) FORCE
ifdef CONFIG_HEADERS_CHECK
	$(Q)$(MAKE) -f $(srctree)/Makefile headers_check
endif
ifdef CONFIG_SAMPLES
	$(Q)$(MAKE) $(build)=samples
endif
ifdef CONFIG_BUILD_DOCSRC
	$(Q)$(MAKE) $(build)=Documentation
endif
	+$(call if_changed,link-vmlinux)

# 在进行编译时，确保在生成实际的目标文件时不会触发任何隐含的规则。
# 这是为了确保在构建过程中能够准确地控制每一个步骤，以避免意外的行为或不确定性。
# 在内核构建过程中，这一点尤为重要，因为内核的构建需要严格的控制和管理，以保证生
# 成的内核镜像是稳定可靠的。
# The actual objects are generated when descending, 
# make sure no implicit rule kicks in
$(sort $(vmlinux-deps)): $(vmlinux-dirs) ;

# 在内核构建过程中，需要处理进入$(vmlinux-dirs)中列出的子目录，并预设本地变量以
# 加速构建过程。
# 内核构建过程中的两个步骤：
# 第一个步骤是处理进入(vmlinux−dirs)中列出的子目录。这里的(vmlinux-dirs)是一个包
# 含了内核构建所需要的各种子目录的列表，例如驱动程序、文件系统、网络协议栈等等。
# 进入这些子目录是为了编译和链接子目录中的代码，生成相应的目标文件，最终生成内核
# 镜像。
# 第二个步骤是预设本地变量，以加速构建过程。预设本地变量的目的是为了提高构建过程
# 的效率，例如通过设置环境变量来指定编译器的优化选项，或者设置其他相关的编译、链
# 接参数等等。
#
# 在进行本地环境调整时要注意仅对当前位置进行调整，以避免在运行"make menuconfig"
# 等命令时出现错误的语言设置，确保内核配置的正确性。
# Handle descending into subdirectories listed in $(vmlinux-dirs)
# Preset locale variables to speed up the build process. Limit locale
# tweaks to this spot to avoid wrong language settings when running
# make menuconfig etc.
# Error messages still appears in the original language

# $(vmlinux-dirs) 是需要编译的所有内核目录，前面已经备注了具体目录名
# 通过 $(MAKE) $(build)=$@ 实现了进入每个目录进行编译
PHONY += $(vmlinux-dirs)
$(vmlinux-dirs): prepare scripts
	$(Q)$(MAKE) $(build)=$@

# 创建新的 kernel.release , 存储最新的内核版本信息
# Store (new) KERNELRELASE string in include/config/kernel.release
include/config/kernel.release: include/config/auto.conf FORCE
	$(Q)rm -f $@
	$(Q)echo "$(KERNELVERSION)$$($(CONFIG_SHELL) $(srctree)/scripts/setlocalversion $(srctree))" > $@

# 在我们递归地开始构建内核或模块之前，我们需要做的事情列在“prepare”中。
# 采用了多层次得处理方法， prepareN 的处理在 prepareN-1 之前。
# "archprepare"是在Linux内核的Makefile中使用的一个目标，用于在处理汇编符号链接
# （asm symlink）、version.h和scripts_basic时进行相应的准备工作。
#
# 在Linux内核的Makefile中，"archprepare"通常用于执行一些与特定架构相关的准备工作，
# 这些工作可能包括生成必要的汇编符号链接、创建version.h文件以及处理或创建
# scripts_basic相关内容。这些工作通常是为了准备架构特定的信息或文件，以便后续的
# 构建过程能够顺利进行。
# "archprepare"目标在Linux内核的Makefile中扮演着对特定架构进行准备工作的角色，通
# 常会在处理汇编符号链接、生成version.h以及处理或创建scripts_basic时被调用，以确
# 保Linux内核在特定架构上的构建过程能够顺利进行。
#
# Things we need to do before we recursively start building the kernel
# or the modules are listed in "prepare".
# A multi level approach is used. prepareN is processed before prepareN-1.
# archprepare is used in arch Makefiles and when processed asm symlink,
# version.h and scripts_basic is processed / created.

# 依赖列表
# Listed in dependency order
PHONY += prepare archprepare prepare0 prepare1 prepare2 prepare3

# Prepare3用于检查我们是否在一个单独的输出目录中构建，如果是的话:
# 1) 检查是否在内核src $(srctree)中执行make命令。
# prepare3 is used to check if we are building in a separate output directory,
# and if so do:
# 1) Check that make has not been executed in the kernel src $(srctree)
prepare3: include/config/kernel.release
ifneq ($(KBUILD_SRC),)
	@$(kecho) '  Using $(srctree) as source for kernel'
	$(Q)if [ -f $(srctree)/.config -o -d $(srctree)/include/config ]; then \
		echo "  $(srctree) is not clean, please run 'make mrproper'"; \
		echo "  in the '$(srctree)' directory.";\
		/bin/false; \
	fi;
endif

# 如果使用单独的输出目录，Prepare2将创建一个makefile
# prepare2 creates a makefile if using a separate output directory
prepare2: prepare3 outputmakefile asm-generic

# 重要预处理目标，需要预先创建的基础文件
prepare1: prepare2 include/linux/version.h include/generated/utsrelease.h \
                   include/config/auto.conf
	$(cmd_crmodverdir)

# archprepare，依赖项是一些通用的、基础的预处理目标
# archprepare 在架构Makefile 中也有声明 arch/arm/Makefile
# archprepare:
# 	$(Q)$(MAKE) $(build)=arch/arm/tools include/generated/mach-types.h
archprepare: archheaders archscripts prepare1 scripts_basic

# prepare0 执行了两条命令，都是针对顶层目录的
# 第一条命令中并未指定目标，会模式构建 scripts/Makefile.build 中的 __build
# 第一条命令执行时，实际执行的是 __build 目标中的 $(always) 依赖,而且顶层是有
# Kbuild 文件的，scripts.Makefile.build 是优先包含 Kbuild 的，所以 $(always) 需
# 要在顶层的 Kbuild 中寻找
#
# 第二条命令中的 missing-syscalls 也是在 顶层Kbuild 中定义的
prepare0: archprepare FORCE
	$(Q)$(MAKE) $(build)=.

# 所有的预处理目标，都是从 prepare 开始的
# 处理完 prepare 目标，就开始进入各个目录进行构建了
# All the preparing..
prepare: prepare0

# 生成一些文件
# 基本的文件，所有构建都需要的
# Generate some files
# ---------------------------------------------------------------------------

# KERNELRELEASE 可以从不同的地方更改，需要更新 version.h
# 所有构建总强制执行的
# KERNELRELEASE can change from a few different places, meaning version.h
# needs to be updated, so this check is forced on all builds

# uts 最大长度
# 用来检测 KERNELRELEASE 对应的版本号字符串的最大长度
# 本代码中， KERNELRELEASE 对应 "3.0.15"
# 最终会输出 #define UTS_RELEASE "3.0.15" 到 include/generated/utsrelease.h
uts_len := 64
define filechk_utsrelease.h
	if [ `echo -n "$(KERNELRELEASE)" | wc -c ` -gt $(uts_len) ]; then \
	  echo '"$(KERNELRELEASE)" exceeds $(uts_len) characters' >&2;    \
	  exit 1;                                                         \
	fi;                                                               \
	(echo \#define UTS_RELEASE \"$(KERNELRELEASE)\";)
endef

# include/linux/version.h 文件
# 输出内容：
# #define LINUX_VERSION_CODE 196623
# #define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))
define filechk_version.h
	(echo \#define LINUX_VERSION_CODE $(shell                         \
	expr $(VERSION) \* 65536 + 0$(PATCHLEVEL) \* 256 + 0$(SUBLEVEL)); \
	echo '#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))';)
endef

# include/linux/version.h 目标
include/linux/version.h: $(srctree)/Makefile FORCE
	$(call filechk,version.h)

# include/generated/utsrelease.h 目标
include/generated/utsrelease.h: include/config/kernel.release FORCE
	$(call filechk,utsrelease.h)

# 检查头文件依赖关系
# 分析头文件的包含关系，如果有递归包含，会进行警告提示
PHONY += headerdep
headerdep:
	$(Q)find $(srctree)/include/ -name '*.h' | xargs --max-args 1 \
	$(srctree)/scripts/headerdep.pl -I$(srctree)/include

# ---------------------------------------------------------------------------

PHONY += depend dep
depend dep:
	@echo '*** Warning: make $@ is unnecessary now.'

# Firmware 安装路径
# ---------------------------------------------------------------------------
# Firmware install
INSTALL_FW_PATH=$(INSTALL_MOD_PATH)/lib/firmware
export INSTALL_FW_PATH

PHONY += firmware_install
firmware_install: FORCE
	@mkdir -p $(objtree)/firmware
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.fwinst obj=firmware __fw_install

# ---------------------------------------------------------------------------
# Kernel headers
# 内核头文件安装
# 默认路径在 $(objtree)/usr 中
# 需要安装的头文件，在 include/Kbuild 中使用 header-y 定义了

#Default location for installed headers
export INSTALL_HDR_PATH = $(objtree)/usr

# 安装头文件使用的 Makefile
hdr-inst := -rR -f $(srctree)/scripts/Makefile.headersinst obj

# If we do an all arch process set dst to asm-$(hdr-arch)
hdr-dst = $(if $(KBUILD_HEADERS), dst=include/asm-$(hdr-arch), dst=include/asm)

PHONY += archheaders
archheaders:

PHONY += archscripts
archscripts:

PHONY += __headers
__headers: include/linux/version.h scripts_basic asm-generic archheaders archscripts FORCE
	$(Q)$(MAKE) $(build)=scripts build_unifdef

PHONY += headers_install_all
headers_install_all:
	$(Q)$(CONFIG_SHELL) $(srctree)/scripts/headers.sh install

PHONY += headers_install
headers_install: __headers
	$(if $(wildcard $(srctree)/arch/$(hdr-arch)/include/asm/Kbuild),, \
	$(error Headers not exportable for the $(SRCARCH) architecture))
	$(Q)$(MAKE) $(hdr-inst)=include
	$(Q)$(MAKE) $(hdr-inst)=arch/$(hdr-arch)/include/asm $(hdr-dst)

PHONY += headers_check_all
headers_check_all: headers_install_all
	$(Q)$(CONFIG_SHELL) $(srctree)/scripts/headers.sh check

PHONY += headers_check
headers_check: headers_install
	$(Q)$(MAKE) $(hdr-inst)=include HDRCHECK=1
	$(Q)$(MAKE) $(hdr-inst)=arch/$(hdr-arch)/include/asm $(hdr-dst) HDRCHECK=1

# ---------------------------------------------------------------------------
#  内核模块构建
#  暂不分析，分析内核模块时再进行分析
# Modules

ifdef CONFIG_MODULES

# By default, build modules as well

all: modules

#	Build modules
#
#	A module can be listed more than once in obj-m resulting in
#	duplicate lines in modules.order files.  Those are removed
#	using awk while concatenating to the final file.

PHONY += modules
modules: $(vmlinux-dirs) $(if $(KBUILD_BUILTIN),vmlinux) modules.builtin
	$(Q)$(AWK) '!x[$$0]++' $(vmlinux-dirs:%=$(objtree)/%/modules.order) > $(objtree)/modules.order
	@$(kecho) '  Building modules, stage 2.';
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.modpost
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.fwinst obj=firmware __fw_modbuild

modules.builtin: $(vmlinux-dirs:%=%/modules.builtin)
	$(Q)$(AWK) '!x[$$0]++' $^ > $(objtree)/modules.builtin

%/modules.builtin: include/config/auto.conf
	$(Q)$(MAKE) $(modbuiltin)=$*


# Target to prepare building external modules
PHONY += modules_prepare
modules_prepare: prepare scripts

# Target to install modules
PHONY += modules_install
modules_install: _modinst_ _modinst_post

PHONY += _modinst_
_modinst_:
	@rm -rf $(MODLIB)/kernel
	@rm -f $(MODLIB)/source
	@mkdir -p $(MODLIB)/kernel
	@ln -s $(srctree) $(MODLIB)/source
	@if [ ! $(objtree) -ef  $(MODLIB)/build ]; then \
		rm -f $(MODLIB)/build ; \
		ln -s $(objtree) $(MODLIB)/build ; \
	fi
	@cp -f $(objtree)/modules.order $(MODLIB)/
	@cp -f $(objtree)/modules.builtin $(MODLIB)/
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.modinst

# This depmod is only for convenience to give the initial
# boot a modules.dep even before / is mounted read-write.  However the
# boot script depmod is the master version.
PHONY += _modinst_post
_modinst_post: _modinst_
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.fwinst obj=firmware __fw_modinst
	$(call cmd,depmod)

else # CONFIG_MODULES

# Modules not configured
# ---------------------------------------------------------------------------

modules modules_install: FORCE
	@echo
	@echo "The present kernel configuration has modules disabled."
	@echo "Type 'make config' and enable loadable module support."
	@echo "Then build a kernel with module support enabled."
	@echo
	@exit 1

endif # CONFIG_MODULES

###
# 清理工作分三层次进行
# make clean     删除大部分生成的文件
# 				 留下足够的文件用来构建外部模块
# make mrproper  删除当前配置和所有生成的文件
# make distclean 删除编辑器临时文件，补丁文件，各种中间文件
# Cleaning is done on three levels.
# make clean     Delete most generated files
#                Leave enough to build external modules
# make mrproper  Delete the current configuration, and all generated files
# make distclean Remove editor backup files, patch leftover files and the like
#
# 从下面的代码分析，清理彻底程度依次为 clean mrproper distclean

# make clean 命令需要清除的目录和文件
# Directories & files removed with 'make clean'
CLEAN_DIRS  += $(MODVERDIR)

# make mrproper 命令需要清除的目录和文件
# Directories & files removed with 'make mrproper'
MRPROPER_DIRS  += include/config usr/include include/generated          \
                  arch/*/include/generated
MRPROPER_FILES += .config .config.old .version .old_version             \
                  include/linux/version.h                               \
		  Module.symvers tags TAGS cscope* GPATH GTAGS GRTAGS GSYMS

# clean - Delete most, but leave enough to build external modules
#
clean: rm-dirs  := $(CLEAN_DIRS)
clean: rm-files := $(CLEAN_FILES)
clean-dirs      := $(addprefix _clean_, . $(vmlinux-alldirs) Documentation samples)

PHONY += $(clean-dirs) clean archclean
$(clean-dirs):
	$(Q)$(MAKE) $(clean)=$(patsubst _clean_%,%,$@)

# clean 依赖架构的 archclean
clean: archclean

# mrproper - Delete all generated files, including .config
#
mrproper: rm-dirs  := $(wildcard $(MRPROPER_DIRS))
mrproper: rm-files := $(wildcard $(MRPROPER_FILES))
mrproper-dirs      := $(addprefix _mrproper_,Documentation/DocBook scripts)

PHONY += $(mrproper-dirs) mrproper archmrproper
$(mrproper-dirs):
	$(Q)$(MAKE) $(clean)=$(patsubst _mrproper_%,%,$@)

mrproper: clean archmrproper $(mrproper-dirs)
	$(call cmd,rmdirs)
	$(call cmd,rmfiles)

# 最彻底的清理
# distclean
#
PHONY += distclean

distclean: mrproper
	@find $(srctree) $(RCS_FIND_IGNORE) \
		\( -name '*.orig' -o -name '*.rej' -o -name '*~' \
		-o -name '*.bak' -o -name '#*#' -o -name '.*.orig' \
		-o -name '.*.rej' \
		-o -name '*%' -o -name '.*.cmd' -o -name 'core' \) \
		-type f -print | xargs rm -f


# 将内核打包成各种格式
# 具体的处理在 scripts/package/Makefile 中
# 这里的构建目标匹配时隐式匹配，而在 scripts/package/Makefile 中，
# 隐式匹配规则有所变化
# 如 perf-targz-src-pkg 在这里匹配 %src-pkg
# 而在处理时，匹配 perf-%pkg
# Packaging of the kernel to various formats
# ---------------------------------------------------------------------------
# rpm target kept for backward compatibility
package-dir	:= $(srctree)/scripts/package

%src-pkg: FORCE
	$(Q)$(MAKE) $(build)=$(package-dir) $@
%pkg: include/config/kernel.release FORCE
	$(Q)$(MAKE) $(build)=$(package-dir) $@
rpm: include/config/kernel.release FORCE
	$(Q)$(MAKE) $(build)=$(package-dir) $@


# 典型构建目标 帮助文档
# 这里面的处理，有通用的构建目标（所有linux版本都支持的）
# 另外一些是针对架构、单板所支持的
# 需要使用与架构、单板相关的 Makefile 中的 help 目标
# Brief documentation of the typical targets used
# ---------------------------------------------------------------------------

# 选择某个架构后，该架构所支持的所有配置文件
# 对于 arm，目录为 arch/arm/configs/
# 该目录定义了很多的适用于不同芯片、单板的配置文件，可以作为配置模板开始构建
boards := $(wildcard $(srctree)/arch/$(SRCARCH)/configs/*_defconfig)
boards := $(notdir $(boards))
board-dirs := $(dir $(wildcard $(srctree)/arch/$(SRCARCH)/configs/*/*_defconfig))
board-dirs := $(sort $(notdir $(board-dirs:/=)))

help:
	@echo  'Cleaning targets:'
	@echo  '  clean		  - Remove most generated files but keep the config and'
	@echo  '                    enough build support to build external modules'
	@echo  '  mrproper	  - Remove all generated files + config + various backup files'
	@echo  '  distclean	  - mrproper + remove editor backup and patch files'
	@echo  ''
	@echo  'Configuration targets:'
	@$(MAKE) -f $(srctree)/scripts/kconfig/Makefile help
	@echo  ''
	@echo  'Other generic targets:'
	@echo  '  all		  - Build all targets marked with [*]'
	@echo  '* vmlinux	  - Build the bare kernel'
	@echo  '* modules	  - Build all modules'
	@echo  '  modules_install - Install all modules to INSTALL_MOD_PATH (default: /)'
	@echo  '  firmware_install- Install all firmware to INSTALL_FW_PATH'
	@echo  '                    (default: $$(INSTALL_MOD_PATH)/lib/firmware)'
	@echo  '  dir/            - Build all files in dir and below'
	@echo  '  dir/file.[oisS] - Build specified target only'
	@echo  '  dir/file.lst    - Build specified mixed source/assembly target only'
	@echo  '                    (requires a recent binutils and recent build (System.map))'
	@echo  '  dir/file.ko     - Build module including final link'
	@echo  '  modules_prepare - Set up for building external modules'
	@echo  '  tags/TAGS	  - Generate tags file for editors'
	@echo  '  cscope	  - Generate cscope index'
	@echo  '  gtags           - Generate GNU GLOBAL index'
	@echo  '  kernelrelease	  - Output the release version string'
	@echo  '  kernelversion	  - Output the version stored in Makefile'
	@echo  '  headers_install - Install sanitised kernel headers to INSTALL_HDR_PATH'; \
	 echo  '                    (default: $(INSTALL_HDR_PATH))'; \
	 echo  ''
	@echo  'Static analysers'
	@echo  '  checkstack      - Generate a list of stack hogs'
	@echo  '  namespacecheck  - Name space analysis on compiled kernel'
	@echo  '  versioncheck    - Sanity check on version.h usage'
	@echo  '  includecheck    - Check for duplicate included header files'
	@echo  '  export_report   - List the usages of all exported symbols'
	@echo  '  headers_check   - Sanity check on exported headers'
	@echo  '  headerdep       - Detect inclusion cycles in headers'
	@$(MAKE) -f $(srctree)/scripts/Makefile.help checker-help
	@echo  ''
	@echo  'Kernel packaging:'
	@$(MAKE) $(build)=$(package-dir) help
	@echo  ''
	@echo  'Documentation targets:'
	@$(MAKE) -f $(srctree)/Documentation/DocBook/Makefile dochelp
	@echo  ''
	@echo  'Architecture specific targets ($(SRCARCH)):'
	@$(if $(archhelp),$(archhelp),\
		echo '  No architecture specific help defined for $(SRCARCH)')
	@echo  ''
	@$(if $(boards), \
		$(foreach b, $(boards), \
		printf "  %-24s - Build for %s\\n" $(b) $(subst _defconfig,,$(b));) \
		echo '')
	@$(if $(board-dirs), \
		$(foreach b, $(board-dirs), \
		printf "  %-16s - Show %s-specific targets\\n" help-$(b) $(b);) \
		printf "  %-16s - Show all of the above\\n" help-boards; \
		echo '')

	@echo  '  make V=0|1 [targets] 0 => quiet build (default), 1 => verbose build'
	@echo  '  make V=2   [targets] 2 => give reason for rebuild of target'
	@echo  '  make O=dir [targets] Locate all output files in "dir", including .config'
	@echo  '  make C=1   [targets] Check all c source with $$CHECK (sparse by default)'
	@echo  '  make C=2   [targets] Force check of all c source with $$CHECK'
	@echo  '  make RECORDMCOUNT_WARN=1 [targets] Warn about ignored mcount sections'
	@echo  '  make W=n   [targets] Enable extra gcc checks, n=1,2,3 where'
	@echo  '		1: warnings which may be relevant and do not occur too often'
	@echo  '		2: warnings which occur quite often but may still be relevant'
	@echo  '		3: more obscure warnings, can most likely be ignored'
	@echo  '		Multiple levels can be combined with W=12 or W=123'
	@echo  ''
	@echo  'Execute "make" or "make all" to build all targets marked with [*] '
	@echo  'For further info see the ./README file'


# 针对具有子目录的配置文件的帮助
help-board-dirs := $(addprefix help-,$(board-dirs))

help-boards: $(help-board-dirs)

boards-per-dir = $(notdir $(wildcard $(srctree)/arch/$(SRCARCH)/configs/$*/*_defconfig))

$(help-board-dirs): help-%:
	@echo  'Architecture specific targets ($(SRCARCH) $*):'
	@$(if $(boards-per-dir), \
		$(foreach b, $(boards-per-dir), \
		printf "  %-24s - Build for %s\\n" $*/$(b) $(subst _defconfig,,$(b));) \
		echo '')


# 文档类模板
# 所有的文档类目标，结尾都是 docs
# Documentation targets
# ---------------------------------------------------------------------------
%docs: scripts_basic FORCE
	$(Q)$(MAKE) $(build)=scripts build_docproc
	$(Q)$(MAKE) $(build)=Documentation/DocBook $@

else # KBUILD_EXTMOD

# 单独编译外部模块，走这个流程
# 暂不做分析
###
# External module support.
# When building external modules the kernel used as basis is considered
# read-only, and no consistency checks are made and the make
# system is not used on the basis kernel. If updates are required
# in the basis kernel ordinary make commands (without M=...) must
# be used.
#
# The following are the only valid targets when building external
# modules.
# make M=dir clean     Delete all automatically generated files
# make M=dir modules   Make all modules in specified dir
# make M=dir	       Same as 'make M=dir modules'
# make M=dir modules_install
#                      Install the modules built in the module directory
#                      Assumes install directory is already created

# We are always building modules
KBUILD_MODULES := 1
PHONY += crmodverdir
crmodverdir:
	$(cmd_crmodverdir)

PHONY += $(objtree)/Module.symvers
$(objtree)/Module.symvers:
	@test -e $(objtree)/Module.symvers || ( \
	echo; \
	echo "  WARNING: Symbol version dump $(objtree)/Module.symvers"; \
	echo "           is missing; modules will have no dependencies and modversions."; \
	echo )

module-dirs := $(addprefix _module_,$(KBUILD_EXTMOD))
PHONY += $(module-dirs) modules
$(module-dirs): crmodverdir $(objtree)/Module.symvers
	$(Q)$(MAKE) $(build)=$(patsubst _module_%,%,$@)

modules: $(module-dirs)
	@$(kecho) '  Building modules, stage 2.';
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.modpost

PHONY += modules_install
modules_install: _emodinst_ _emodinst_post

install-dir := $(if $(INSTALL_MOD_DIR),$(INSTALL_MOD_DIR),extra)
PHONY += _emodinst_
_emodinst_:
	$(Q)mkdir -p $(MODLIB)/$(install-dir)
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.modinst

PHONY += _emodinst_post
_emodinst_post: _emodinst_
	$(call cmd,depmod)

clean-dirs := $(addprefix _clean_,$(KBUILD_EXTMOD))

PHONY += $(clean-dirs) clean
$(clean-dirs):
	$(Q)$(MAKE) $(clean)=$(patsubst _clean_%,%,$@)

clean:	rm-dirs := $(MODVERDIR)
clean: rm-files := $(KBUILD_EXTMOD)/Module.symvers

help:
	@echo  '  Building external modules.'
	@echo  '  Syntax: make -C path/to/kernel/src M=$$PWD target'
	@echo  ''
	@echo  '  modules         - default target, build the module(s)'
	@echo  '  modules_install - install the module'
	@echo  '  clean           - remove generated files in module directory only'
	@echo  ''

# Dummies...
PHONY += prepare scripts
prepare: ;
scripts: ;
endif # KBUILD_EXTMOD

# clean 清理
# 各种中间文件，生成文件
# rmdirs rmfiles 命令都在后面
clean: $(clean-dirs)
	$(Q)$(CONFIG_SHELL) $(srctree)/scripts/link-vmlinux.sh clean
	$(call cmd,rmdirs)
	$(call cmd,rmfiles)
	@find $(if $(KBUILD_EXTMOD), $(KBUILD_EXTMOD), .) $(RCS_FIND_IGNORE) \
		\( -name '*.[oas]' -o -name '*.ko' -o -name '.*.cmd' \
		-o -name '.*.d' -o -name '.*.tmp' -o -name '*.mod.c' \
		-o -name '*.symtypes' -o -name 'modules.order' \
		-o -name modules.builtin -o -name '.tmp_*.o.*' \
		-o -name '*.gcno' \) -type f -print | xargs rm -f

# 编辑器使用的 tags
# ctags cscope
# Generate tags for editors
# ---------------------------------------------------------------------------
quiet_cmd_tags = GEN     $@
      cmd_tags = $(CONFIG_SHELL) $(srctree)/scripts/tags.sh $@

tags TAGS cscope gtags: FORCE
	$(call cmd,tags)

# 检查一致性的脚本
# Scripts to check various things for consistency
# ---------------------------------------------------------------------------

PHONY += includecheck versioncheck coccicheck namespacecheck export_report

includecheck:
	find $(srctree)/* $(RCS_FIND_IGNORE) \
		-name '*.[hcS]' -type f -print | sort \
		| xargs $(PERL) -w $(srctree)/scripts/checkincludes.pl

versioncheck:
	find $(srctree)/* $(RCS_FIND_IGNORE) \
		-name '*.[hcS]' -type f -print | sort \
		| xargs $(PERL) -w $(srctree)/scripts/checkversion.pl

coccicheck:
	$(Q)$(CONFIG_SHELL) $(srctree)/scripts/$@

namespacecheck:
	$(PERL) $(srctree)/scripts/namespace.pl

export_report:
	$(PERL) $(srctree)/scripts/export_report.pl

endif #ifeq ($(config-targets),1)
endif #ifeq ($(mixed-targets),1)

PHONY += checkstack kernelrelease kernelversion

# UML 需要做一些特殊处理。它希望使用主机工具链。因此，需要将 $(SUBARCH) 传递给
# checkstack.pl
# 其他的希望是 $(ARCH), 包括使用交叉构建的人，这意味着 $(SUBARCH) 在这里不起作用。
# checkstack.pl 用于检查内核源代码中的函数调用栈是否超出了预设的栈大小。
# 1. 检查函数调用栈： 遍历内核源代码中的函数，并检查每个函数的调用栈大小。调用栈
# 是用来保存函数调用过程中的局部变量、返回地址等信息的一块内存区域。
# 2. 判断栈大小是否超出限制： checkstack.pl 会将每个函数的调用栈大小与预设的栈大
# 小进行比较，判断是否超出了限制。如果超出了限制，可能会导致栈溢出等问题。
# 3. 生成报告： 检查完成后，checkstack.pl 会生成一个报告，列出超出限制的函数和相
# 应的调用栈大小。
# UML needs a little special treatment here.  It wants to use the host
# toolchain, so needs $(SUBARCH) passed to checkstack.pl.  Everyone
# else wants $(ARCH), including people doing cross-builds, which means
# that $(SUBARCH) doesn't work here.
ifeq ($(ARCH), um)
CHECKSTACK_ARCH := $(SUBARCH)
else
CHECKSTACK_ARCH := $(ARCH)
endif
checkstack:
	$(OBJDUMP) -d vmlinux $$(find . -name '*.ko') | \
	$(PERL) $(src)/scripts/checkstack.pl $(CHECKSTACK_ARCH)

# 输出 kernel release 版本
kernelrelease:
	@echo "$(KERNELVERSION)$$($(CONFIG_SHELL) $(srctree)/scripts/setlocalversion $(srctree))"

# 输出 kernel version 版本
kernelversion:
	@echo $(KERNELVERSION)

# Clear a bunch of variables before executing the submake
tools/: FORCE
	$(Q)$(MAKE) LDFLAGS= MAKEFLAGS= -C $(src)/tools/

tools/%: FORCE
	$(Q)$(MAKE) LDFLAGS= MAKEFLAGS= -C $(src)/tools/ $*

# 单一目标编译
# Single targets
# ---------------------------------------------------------------------------
# 单目标兼容：
# - 源代码和输出文件路径一致进行构建
# - 使用 make O= 控制单独的输出文件路径进行构建
# - 外部模块
# Single targets are compatible with:
# - build with mixed source and output
# - build with separate output dir 'make O=...'
# - external modules
#
# target-dir 存储输出文件的路径
# build-dir  内核源代码中使用的路径
#  target-dir => where to store outputfile
#  build-dir  => directory in kernel source tree to use

ifeq ($(KBUILD_EXTMOD),)
        build-dir  = $(patsubst %/,%,$(dir $@))
        target-dir = $(dir $@)
else
        zap-slash=$(filter-out .,$(patsubst %/,%,$(dir $@)))
        build-dir  = $(KBUILD_EXTMOD)$(if $(zap-slash),/$(zap-slash))
        target-dir = $(if $(KBUILD_EXTMOD),$(dir $<),$(dir $@))
endif

# 各种源文件的构建方法
# 所有的源文件构建，都会调用到 scripts/Makefile.build
# 最终的构建命令，也是在 scripts/Makefile.build 中的，如 cmd_cc_o_c
%.s: %.c prepare scripts FORCE
	$(Q)$(MAKE) $(build)=$(build-dir) $(target-dir)$(notdir $@)
%.i: %.c prepare scripts FORCE
	$(Q)$(MAKE) $(build)=$(build-dir) $(target-dir)$(notdir $@)
%.o: %.c prepare scripts FORCE
	$(Q)$(MAKE) $(build)=$(build-dir) $(target-dir)$(notdir $@)
%.lst: %.c prepare scripts FORCE
	$(Q)$(MAKE) $(build)=$(build-dir) $(target-dir)$(notdir $@)
%.s: %.S prepare scripts FORCE
	$(Q)$(MAKE) $(build)=$(build-dir) $(target-dir)$(notdir $@)
%.o: %.S prepare scripts FORCE
	$(Q)$(MAKE) $(build)=$(build-dir) $(target-dir)$(notdir $@)
%.symtypes: %.c prepare scripts FORCE
	$(Q)$(MAKE) $(build)=$(build-dir) $(target-dir)$(notdir $@)

# Modules 构建方法
# 也是在 scripts/Makefile.build 中完成的
# Modules
/: prepare scripts FORCE
	$(cmd_crmodverdir)
	$(Q)$(MAKE) KBUILD_MODULES=$(if $(CONFIG_MODULES),1) \
	$(build)=$(build-dir)
%/: prepare scripts FORCE
	$(cmd_crmodverdir)
	$(Q)$(MAKE) KBUILD_MODULES=$(if $(CONFIG_MODULES),1) \
	$(build)=$(build-dir)
%.ko: prepare scripts FORCE
	$(cmd_crmodverdir)
	$(Q)$(MAKE) KBUILD_MODULES=$(if $(CONFIG_MODULES),1)   \
	$(build)=$(build-dir) $(@:.ko=.o)
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.modpost

# FIXME Should go into a make.lib or something 
# ===========================================================================

# clean 目录、文件命令
quiet_cmd_rmdirs = $(if $(wildcard $(rm-dirs)),CLEAN   $(wildcard $(rm-dirs)))
      cmd_rmdirs = rm -rf $(rm-dirs)

quiet_cmd_rmfiles = $(if $(wildcard $(rm-files)),CLEAN   $(wildcard $(rm-files)))
      cmd_rmfiles = rm -f $(rm-files)

# 只有在有 System.map 和 depmod 可执行的情况下才运行 depmod
# Run depmod only if we have System.map and depmod is executable
quiet_cmd_depmod = DEPMOD  $(KERNELRELEASE)
      cmd_depmod = $(CONFIG_SHELL) $(srctree)/scripts/depmod.sh $(DEPMOD) \
                   $(KERNELRELEASE)

# 为模块支持文件创建临时目录
# 只有在构建所有模块时才清理它
# Create temporary dir for module support files
# clean it up only when building all modules
cmd_crmodverdir = $(Q)mkdir -p $(MODVERDIR) \
                  $(if $(KBUILD_MODULES),; rm -f $(MODVERDIR)/*)

# read all saved command lines

targets := $(wildcard $(sort $(targets)))
cmd_files := $(wildcard .*.cmd $(foreach f,$(targets),$(dir $(f)).$(notdir $(f)).cmd))

ifneq ($(cmd_files),)
  $(cmd_files): ;	# Do not try to update included dependency files
  include $(cmd_files)
endif

# 用于 clean 相关的处理
# Shorthand for $(Q)$(MAKE) -f scripts/Makefile.clean obj=dir
# Usage:
# $(Q)$(MAKE) $(clean)=dir
clean := -f $(if $(KBUILD_SRC),$(srctree)/)scripts/Makefile.clean obj

endif	# skip-makefile

# FORCE是一个常见的伪目标，它通常用于强制执行某些规则，即使这些规则的依赖没有发
# 生变化。在这种情况下，sub-make目标可能会被强制执行，即使没有明确的依赖关系或文
# 件更新。
PHONY += FORCE
FORCE:

# 在Makefile中，.PHONY是一个特殊的目标，它表示某个规则不对应实际的文件名，而是一
# 个伪目标，用于定义一些需要执行的操作，而不是生成文件的规则。使用.PHONY可以避免
# 与同名文件冲突，而且可以确保即使有同名文件也会执行对应的操作。常见的用法是定义
# clean规则为.PHONY，以便在执行make clean时确保会执行clean操作，而不会因为有
# clean文件而导致误删文件。
# Declare the contents of the .PHONY variable as phony.  We keep that
# information in a variable so we can use it in if_changed and friends.
.PHONY: $(PHONY)
