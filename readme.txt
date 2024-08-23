
本文档记录内核移植的整个过程，包括一些乱七八糟的记录

移植采用的为 linux官方的 linux-3.5 版本，从官方下载的纯净源码，使用的开发板为
FriendlyARM 的 NanoPC-T1 ，该开发板具有一个可用的 linux3.5 版本源码可供参考。

遗留问题:
1、paging_init 中，关于内存的初始化，很多都没有分析；
2、时钟部分的配置，开机启动的打印中，有部分时钟显示没有开启， smdk4x12_map_io 中
的 s3c24xx_init_clocks 中的处理；
3、exynos_init_io 关于 io 映射相关的处理，重点需理解 io 内存映射；
4、crashkernel 功能值得看一下，本代码中并未配置启用；




知识点：
1、如果是压缩的内核，如 zImage ，启动的代码在 arch/arm/boot/compressed/head.S
中，这里进行内核的解压，核心的代码在 decompress_kernel 函数中，调用的为 lib/ 目
录下的解压缩代码；
2、内核启动的代码，在 arch/arm/kernel/head.S ，跳转到 C语言入口, start_kernel 是
在 arch/arm/kernel/head-common.S 中；这两个文件是所有的arm平台共用的;
3、head.s 中说明了虚拟内存页表的起始地址，这也是为什么内核的启动地址一般是
0xc0008000的原因；
swapper_pg_dir 是内核初始页表，主要目的是在真正的内核页表创建之前，把内核启动过
程中的代码（虚拟地址）映射到物理内存。在ARM体系结构中，swapper_pg_dir本身位于
16KB-32KB的位置，32KB（0x8000）开始存放内核代码。swapper_pg_dir 只映射了1M的地址
空间，这已经足够了，内核的初始化代码不会超过1M。
在ARM体系架构中，内核一般使用 0x8000（32KB）作为起始地址，0-32KB预留，其中
16KB-32KB用于存放初始的内核页表 swapper_pg_dir。
4、在 head.s 中，设置最基本的初始页表后，就打开了MMU，然后跳转到 head-common.s
中，__mmap_switched 是打开MMU后运行的第一个代码；
5、__mmap_switched 只做了简单的清理工作，就跳转到C语言接口了， start_kernel;
6、start_kernel 是内核的C语言入口，初始化动作都是在这里做的；
7、内核的第一行打印，是在 smp_setup_processor_id 中打印的，
    Booting Linux on physical CPU 0
8、内核初始化过程中，关闭了本CPU的中断，初始化完成后再开启；
9、架构相关的初始化 setup_arch, 在 arch/arm/kernel/setup.c 中，内核初始化了最基
本的几个模块后就调用了;
10、setup_arch 中，有个平台相关的重要调用， setup_machine_tags，此函数中，会根据
传递给内核的machid，调用相应的 machine_desc 结构，这些结构是使用 MACHINE_START
和 MACHINE_END 进行定义初始化的，如 arch/arm/mach-exynos/mach-nanopc-t1.c 中定义
的；关于 machine_arch_type 的值，是个很有意思的处理：
首先，在 include/generated/mach-types.h 中，有很多关于 machine的定义，针对每一种
machine，都有如下的定义：
    #ifdef CONFIG_MACH_NANOPC_T1
    # ifdef machine_arch_type
    #  undef machine_arch_type
    #  define machine_arch_type	__machine_arch_type
    # else
    #  define machine_arch_type	MACH_TYPE_NANOPC_T1
    # endif
    # define machine_is_nanopc_t1()	(machine_arch_type == MACH_TYPE_NANOPC_T1)
    #else
    # define machine_is_nanopc_t1()	(0)
    #endif
可以看到， machine_arch_type 的定义有两种可能， __machine_arch_type 或 MACH_TYPE_XXX
但是这里还做了一个处理，也就是如果定义了 machine_arch_type ，则会去掉已经定义的，
重新将 machine_arch_type 设置为 __machine_arch_type，如果没有，则定义 machine_arch_type
为某个特定的类型； 分析一下，如果代码中只定义了一种 mach ，那就从来没有定义过 machine_arch_type,
machine_arch_type 就会被定义成某个特定的类型，这样在内核启动的时候，
machine_arch_type 的值就是一个特定的值，而不是由uboot传进来的了；
如果定义了多种 mach，那第一个定义的地方，定义为某个特定类型，但在后面的处理中，
会把它重新定义为 __machine_arch_type，而 __machine_arch_type 是从uboot传递进来的，
综上，内核的处理机制是，如果只定义一种板级类型，那直接就使用这种，不判断uboot传
进来的；如果该内核适配了多种板级类型，那就需要根据uboot传进来的 machid，进行解析，
处理最佳的。
11、__atags_pointer 存放的是uboot传入的 atags 的在内存中的物理地址，这个代码中，
偏移是 0x100, 也就是在物理地址 0x40000100处，根据物理地址转虚拟地址规则，虚拟地
址为 0xc0000100；
uboot传给内核的参数以atags的形式，本代码中，默认情况下，uboot传入的参数包括内存
地址参数，如下：
RAM起始地址     RAM大小
40000000        10000000
50000000        10000000
60000000        10000000
70000000        0ff00000
最后一个长度为 0ff00000 是因为在uboot中，将最后 1M空间预留给 trustzone 了。
12、machine特有的处理函数 fixup 调用，这是调用的第一个 machine 的处理函数，可以
在这个函数中做一些参数矫正，如内存，启动参数等，本代码中未做任何处理；
13、在 parse_tags 处理中，逐个分析 atags ，并调用相应的处理函数，此处主要是处理
ATAG_MEM 参数，将上面的内存信息添加到了 meminfo 结构中；
14、接着处理 cmdline，内核配置中有一个默认的 CONFIG_CMDLINE，并且由三种方式:扩展、
强制使用内核、使用uboot传递的；
扩展：如果uboot提供了，则与内核默认的一同使用；
强制：忽略uboot提供的，强制使用内核默认的；
使用uboot的：如果uboot传递了 ATAG_CMDLINE ，则使用uboot中的；
15、重点分析一下 sanity_check_meminfo 中关于 high_memory 的处理，
linux的高端内存，一直以来都是迷惑的，这次看到这里的处理代码，重点分析了一下高端
内存的含义，作用，简单概括如下：
高端内存的出现，是因为在32位CPU中，内存的寻址范围为4GB，而内核空间范围一般设置为
3GB以后的1GB空间，内核空间只有1GB，如果实际的物理内存超过了1GB，那之后的内存就无
法映射了，所以提出了高端内存；
内核的一部分（低端内存）是直接映射的，与物理内存只有差一个偏移，物理地址与虚拟地
址都是连续的，剩余的物理内存就是用高端内存的几百MB空间进行映射，不是固定映射的，
是在使用的时候进行映射，这样就可以使用少量的虚拟地址访问大量的物理内存了。
首先，内核中可以选择是否启用高端内存，CONFIG_HIGHMEM，如果没有启用高端内存，那超
出所配置的低端内存的部分，都将忽略掉，是不使用的；
低端内存的限制是根据 vmalloc_min 这个变量进行限制，从 PAGE_OFFSET 到 vmalloc_min
之间，都是属于 lowmem 的， vmalloc_min 之后的都属于高端内存 highmem 了，
vmalloc_min 这个变量，默认值是根据 VMALLOC 相关的几个宏计算的，
static void * __initdata vmalloc_min =
	(void *)(VMALLOC_END - (240 << 20) - VMALLOC_OFFSET);
可以看到，是 VMALLOC 区域的结尾地址 VMALLOC_END 预留 240MB 空间之后，又向前偏移
了 VMALLOC_OFFSET（8MB） 之后得到的，按照本代码中的配置，
VMALLOC_END = 0xff000000
VMALLOC_OFFSET = 0x0080000
所以 lowmem 的范围就是 0xc0000000 - 0xef800000   ( 760 MB )
vmalloc 的范围就是     0xf0000000 - 0xff000000   ( 240 MB )
这里的 VMALLOC_OFFSET 所占用的空间，就是 lowmem 和 vmalloc 之间的空洞，是不进行
映射的，这样可以用来捕获内存的越界访问，高端内存里有多个这种空洞；
还有个需要注意的点，VMALLOC_START 的值，这是一个宏，在
arch/arm/include/asm/pgtable.h，
#define VMALLOC_OFFSET		(8*1024*1024)
#define VMALLOC_START		(((unsigned long)high_memory + VMALLOC_OFFSET) & ~(VMALLOC_OFFSET-1))
#define VMALLOC_END		0xff000000UL
VMALLOC_START 是计算出来的，是高端内存起始地址 + VMALLOC_OFFSET
而 high_memory 是在 sanity_check_meminfo 中计算出来的，详细可参考该函数。
这里还有个特别注意的点，就是 meminfo 结构，这里面存放了物理内存的信息，有两个成
员:
struct meminfo {
	int nr_banks;                   // 物理内存 bank 数量
	struct membank bank[NR_BANKS];  // 物理内存每个 bank 的信息
};

每个bank的信息：
struct membank {
	phys_addr_t start;              // 物理内存起始地址
	unsigned long size;             // 物理内存大小
	unsigned int highmem;           // 是否属于高端内存
};

在 setup_arch 中解析 atags 时，添加的物理内存信息，都是原始的，uboot传递进来的物
理内存信息，就像上面分析的那样，起始地址 和 内存大小；
而在 sanity_check_meminfo 中处理后，因为高端内存的起始地址不一定与 bank 的地址正
好对应，会将某个跨 低端内存 和 高端内存 的bank进行拆分，划分为两个bank，这样就可
以在每个bank中表示是否属于高端内存了；
arm_lowmem_limit 时低端内存的高地址（高端内存起始地址）的物理内存地址
high_memory 是高端内存地址的虚拟内存地址

memblock_set_current_limit 用来设置系统可用的物理内存中低端内存 和 高端内核 的分
界值，这样内核就知道如何创建页表了；
16、vmalloc 区域的大小，在 ARM架构下，不同版本内核，预留的默认值不同，查看了几个：
linux-3.0.15    128M
linux-4.14.2    240M
linux-5.9.12    240M
而且，VMALLOC_END 地址也是不同的
有一个可以控制 vmalloc大小的参数，可以通过uboot传递，vmalloc=size;
但实测，添加了这个参数就卡死了。。。。。这个函数是进入了的，应该是在初始化阶段，
创建的初始页表太小了，有些函数无法访问导致的；
17、添加一个不一定准确的分析，从代码分析的话，是这样的：
在 paging_init 中，有一个 devicemaps_init ，里面进行了一项 vector 的初始化，是将
vector 放在最高的4KB地址处； vector 所需的空间是这样分配的：
	vectors = early_alloc(PAGE_SIZE);
这是早期使用的分配函数，分配了一个PAGE，这个是从 lowmem 中分配；
vector 经过了一些处理之后，需要将其添加到页表映射中，
是这样处理的：
	map.pfn = __phys_to_pfn(virt_to_phys(vectors));
	map.virtual = 0xffff0000;
	map.length = PAGE_SIZE;
	map.type = MT_HIGH_VECTORS;
	create_mapping(&map);
是将 vectors 所对应的虚拟地址转换为物理地址，然后又计算得到 pfn，页表对应的虚拟
地址强制写成了 0xffff0000；
从该代码的输出看，vectors 分配的地址为 ef7ff000 这是一个 lowmem 中的地址，转换为
物理地址后为 6f7ff000 是在物理内存低端内存中的；
18、寄存器的映射，使用 iotable_init 进行初始化，需要预先设置到 map_desc 结构，包
含了虚拟内存地址、物理内存地址，这里的虚拟内存地址是提前设计好的，本代码中，所有
的寄存器都是从0xF6000000开始的，使用S3C_ADDR 宏进行处理；
19、paging_init 中有个非常重要的函数 map_io 的回调函数，这里面会进行寄存器相关的
映射，本代码中，这个回调为 smdk4x12_map_io，重要的处理在 exynos_init_io 中，详见
文件 arch/arm/mach-exynos/common.c 中的处理，还有一些关于外设相关的初始化，如设
置外设的回调函数； cpu_table 是一个很重要的结构，列举了适配的 CPU，根据读取的
CPU_ID，选择对应的处理函数，用来初始化芯片相关的时钟等外设；
20、kmap_init 中创建了 pkmap 映射，这部分暂未分析；
21、smp_init_cpus 中，检测CPU核心数量，实际检测的数量4核，但配置文件中只配置了2
核，修改配置文件为4核；









下面就是过程记录了；
1、官方源码下载，linux-3.5版本，从纯净源码开始；
2、使用 ubuntu-16.04，脚本存在兼容性，修改了 kernel/timeconst.pl 文件；
3、为了方便调试，修改 Makefile 中的 ARCH = arm，不必每次设置了；
4、内核源码中，有 exynos4 的默认配置文件 exynos4_defconfig，就以这个配置作为基础，
开始移植， make exynos4_defconfig ;
5、修改 交叉编译器、hostname 等基本配置，修改 lowlevel uart = 0， 修改 cmdline；
NanoPC-T1 使用的调试串口为 UART0，所以要修改一下；
6、保存为默认配置 make savedefconfig ， arch/arm/configs/nanopc-t1_defconfig ;
7、再次使用 make nanopc-t1_defconfig , 然后 make zImage，生成 arch/arm/boot/zImage;
8、注意，uboot使用的 FriendlyARM 的，没有更改，uboot给内核传递的 machid = 4608，
这导致在启动内核时，内核解析 machid 失败，所以到这个阶段时，启动的时候需要先在
uboot下，设置machid, setenv machid 0xe36，再次启动，可以看到内核的打印了；
9、为内核添加 mach type，在 arch/arm/tools/mach-types 中添加一个 nanopc_t1，注意
最后的id需要与uboot中的一致；
10、新定义了 mach ，需要添加对应的 mach 文件，根据 arch/arm/Makefile 中的处理，
此处使用的为 CONFIG_ARCH_EXYNOS4，对应的目录为 arch/arm/mach-exynos/，此目录中的
Makefile 中处理对应的 mach，新建一个 mach-nanopc-t1.c，板级代码都在这个文件中添
加；
11、添加mach对应的 Kconfig 配置项，在 arch/arm/mach-exynos/Kconfig 中，添加一个
MACH_NANOPC_T1，借用 SOC_EXYNOS4412，这里有一点需要注意，MACH_SMDK4412 是针对
exynos4412芯片的一个官方的开发板，可以参考这个进行实现，在 MACH_SMDK4412 中，还
选择了 MACH_SMDK4212 ，而 MACH_SMDK4212 中又选择了 SOC_EXYNOS4212，是因为
SOC_EXYNOS4212 中有一个必要的选项，S5P_PM 和 S5P_SLEEP，这里的处理是将这两个选项
也添加到了 SOC_EXYNOS4412 中，避免了选择 SOC_EXYNOS4212 和 MACH_SMDK4212；
12、make menuconfig 中，仅选择 NANOPC-T1 即可；
13、配置CPU核心数量，CONFIG_NR_CPUS 为 4 ， exynos4412 为 4核心芯片；




