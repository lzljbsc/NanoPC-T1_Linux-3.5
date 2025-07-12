
这里分析一下boot目录下的各镜像生成过程：

1. 经过除了本目录之外的编译，已经生成了顶层目录的 vmlinux, ../../../vmlinux
2. arch/arm/boot 目录中 Makefile 依赖 顶层vmlinux 生成本目录中的 Image
    使用 objcopy 工具，生成 Image
    arm-none-linux-gnueabi-objcopy -O binary -R .comment -S  vmlinux arch/arm/boot/Image
3. 需要再把 Image 进行 gzip压缩，是在 compressed 目录中操作
    cat arch/arm/boot/compressed/../Image | gzip -n -f -9 > arch/arm/boot/compressed/piggy.gzip
    此时 compressed/piggy.gzip 文件，就是 gzip 压缩后的 Image 镜像
4. piggy.gzip 文件会被 compressed/piggy.gzip.S 汇编文件包含，也就是将整个压缩后
    的镜像文件，完全不改动的当作一个二进制镜像，编译到解压代码中
    compressed/piggy.gzip.S 文件：
        .section .piggydata,#alloc
        .globl	input_data
    input_data:
        .incbin	"arch/arm/boot/compressed/piggy.gzip"
        .globl	input_data_end
    input_data_end:
5. compressed 目录中，会将 head.S misc.c decompress.c piggy.gzip.S 文件，根据
    vmlinux.lds 编译生成一个镜像文件 compressed/vmlinux
    根据 vmlinux.lds 内容，链接时会将 .piggydata 段放在镜像最后，其他的段从前面
    排列，这样编译后， compressed 目录中解压程序就在镜像最前面， gzip压缩后的镜
    像就最后了，形成了 [解压程序][压缩镜像] 的分布方式。
    compressed/vmlinux 虽然与顶层目录中的 vmlinux 同名，但完全不是一种东西；
    compressed/vmlinux 中的，是一个前面具有解压缩程序，后面是一个gzip压缩的二进
    制linux镜像
6. zImage 依赖与 compressed/vmlinux 生成，依然是通过 objcopy 生成
    arm-none-linux-gnueabi-objcopy -O binary -R .comment -S  arch/arm/boot/compressed/vmlinux arch/arm/boot/zImage
7. uImage 是通过 zImage 生成的， itop4412 中并未使用 uImage ,这里提一下
    uImage 是在 zImage 前面，加一个长度为 0x40 的头，包含了镜像类型，校验等，主
    要用于uboot启动

压缩后的镜像文件，启动内核时，是先从 arch/arm/boot/compressed 中启动的，需要先运
行解压缩程序，将内核解压缩后，再把参数传递给内核，启动内核。
这部分参考 compressed/head.S

传参方式，与uboot中启动内核传参方式一致；
theKernel (0, machid, bd->bi_boot_params);
寄存器  参数含义
r0      0
r1      architecture number
r2      atags pointer


备注：
a、vmlinux (xmlinuz) 是一个包含 linux kernel 的静态链接的可执行文件，文件类型可
能是linux接受的可执行文件格式之一(ELF,COFF或a.out)，vmlinux若要用于调试时则必须
要在引导前增加symbol table.
应用场景：
* 用于调试，但需要包含调试信息
* 编译出来的内核原始文件，可以被用来制作后面的 zImage, bzImage等启动Image
* Uboot不能直接使用 vmlinux

相关内容：
* vmlinux是静态编译出来的最原始的 ELF 文件，包括了内核镜像、调试信息、符号表等内
容；其中 “vm” 代表 “Virtual Memory”，现在一般都是虚拟内存模式，这个是相对于 8086
的实地址而言
* vmlinuz是被压缩的linux内核，是可以被引导的，有两种详细的表现形式：zImage和
bzImage(big zImage)
* zImage是vmlinuz经过gzip压缩后的文件，适用于小内核
* bzImage是vmlinuz经过gzip压缩后的文件，适用于大内核
* uImage 是 uboot 专用的镜像文件，它是在 zImage 之前加上一个长度为 0x40 的头信息，
包括了该镜像文件的类型、加载位置、生成时间、大小等信息

zImage、bzImage 中均包含一个微型的 gzip 用于解压缩内核并引导，两者的不同之处在于：
zImage 解压缩内核到低端内存 (第一个640K)，bzImage 解压缩内核到高端内存 (1M以上)。
也就是，它们之间最大的差别是对于内核体积大小的限制。

由于 zImage 内核需要放在实模式 1MB 的内存之内，所以其体积受到了限制，目前采用的
内核格式大多采用的是 bzImage ，这种格式没有 1MB 内存限制。arm 中常用的是 zImage，
而 x86 中常用的是 bzImage 。

zImage是ARM linux常用的一种压缩镜像文件，它是由vmlinux经过objcopy ， objcopy实现
由vmlinux的elf文件拷贝成纯二进制数据文件加上解压代码经gzip压缩而成，命令格式是
#make zImage.这种格式的Linux镜像文件多存放在NAND上. 适用于小内核的情况，它的存在
是为了向后的兼容性。
