# Report on lab4

- `Li Yiyan 519021911103`
- `lyy0627@sjtu.edu.cn`

## 1
```assembly
	# MPDIR_EL1 是core identification Register, Aff1字段会存储CPUID(Cortex A75是0x0-0x7)
	# 如果是四核可能是0x0-0x3, 移动到tmp register X8
	mrs	x8, mpidr_el1
	# 和0对比，如果CPU ID为0那么就跳到primary分支，其他核则会继续执行
	cbz	x8, primary
	# 下面是个循环结构，直到bss clear之后才会继续运行程序，否则非CPU ID=0的核
	# 会在此阻塞
wait_for_bss_clear:
	# ...
	bne	wait_for_bss_clear
	# 阻塞后会改变EL Level, 准备SP, 然后同样的循环结构阻塞直到smp被开启进入多核模式
```

## 2

是物理地址，因为其他核心还没有初始化页表，无法完成地址翻译。

`secondary_boot_flag`能传入`enable_smp_cores`是main函数作为参数传入的，main函数是因为`start_kernel`在调用main函数的` bl main`将其函数开头保存的`x0`重新存回了`x0`, 成为了main的第一个参数。而start kernel的参数又是`init_c`传入的。

其被赋值的时候已经设置好了页表，因此需要先转换成虚拟地址然后根据基地址核基于CPU ID 的偏移量循环赋值即可。

## 3

```assembly
	ldr	x1, =secondary_boot_flag
	add	x1, x1, x2
	ldr	x3, [x1]
```

观察这段代码即可知，flag不是0均可。x3在后续代码中也不会用到。但是为了避免破坏CPU0的flag，采用了0xBEEF。

设置好flag后，dsb, 正在忙等的其他核会继续执行，进入secondary_init_c, 初始化MMU之后进入secondary_cpu_boot，在这个函数中其他核将会把cpu status设置为run, 我们只需要忙等即可。

然后，在secondary_cpu_boot模仿main函数初始化一下调度器策略，也使用RR即可。

完成本部分后，可以看到四个CPU 核Active的输出，可以看到，stdout是乱序的。

<img src="https://s2.loli.net/2022/04/09/AmWNbXon7uPvRlB.png" alt="image-20220409212855975" style="zoom:67%;" />

(和前面在原生Linux完成不同，本实验在WSL2完成。)

## 4



## 5

不需要，因为`unlock`的时机是退出内核态之前，此时该保存的寄存器已经保存好了，而退出内核前会恢复进入内核态时保存的寄存器，通用寄存器本来也将被破坏。因此`unlock`没有必要保存和恢复寄存器。

## 6

因为IDLE线程在等待队列非空时不应该被调度，而加入到队列中由于FIFO策略总会被调度到的。这可能会浪费CPU时间片，而且不符合IDLE调度时机的设定。

## 7

面向测试用例编程，设定满足要求的异常情况返回值。

## 8

## 9

这里处理了特别久的Bug. 检查了创建thread的系统调用链，在线程sched返回并Exit后会出错。主要Bug在于rr_sched应当使用`switch_to_thread`，自己写相关逻辑漏了thread->prev 的赋值。然后Lab3中写的Exit系统调用语义和sched不匹配，导致无法正确修改Exit的状态。修改后出现了死锁问题。找了一段时间，暂时删去了el0_syscall中的kernel锁。

## 10



