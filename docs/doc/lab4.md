# Report on lab4

- `Li Yiyan 519021911103`
- `lyy0627@sjtu.edu.cn`

## 思考题 1
```assembly
	# MPDIR_EL1 是core identification Register, Aff1字段会存储CPUID(Cortex A75是0x0-0x7)
	# 如果是四核可能是0x0-0x3, 移动到tmp register X8
	mrs	x8, mpidr_el1
	# 和0对比，如果CPU ID为0那么就跳到primary分支，其他核则会继续执行
	cbz	x8, primary
	# 下面是个循环结构，直到bss clear之后才会继续运行程序，否则CPU ID != 0的核
	# 会在此阻塞
wait_for_bss_clear:
	# ...
	bne	wait_for_bss_clear
	# 阻塞后会改变EL Level, 准备SP, 然后同样的循环结构阻塞直到smp被开启进入多核模式
wait_until_smp_enabled:
	/* 直到CPUID = 0的核打开SMP, 否则其他核将在此阻塞 */
	cbz	x3, wait_until_smp_enabled
```

## 思考题 2

`secondary_boot_flag`是物理地址，因为其他核心在Spin的时候还没有初始化页表，无法完成地址翻译。其定义是在`init_c.c`中（被定义时尚未开启MMU），作为参数传入了`start_kernel`, 然后`start_kernel`在调用`kernel/arch/aarch64/main.c`的` bl main`将其函数开头保存的`x0`重新存回了`x0`(即保存第一个参数并在调用`main`前Restore)。然后`main`中再将其传入了`enable_smp_cores`进行赋值。

`secondary_boot_flag`**被赋值的时候CPU0已经设置好了页表**，因此需要将其先转换成虚拟地址，然后基于CPU ID 的偏移量赋值即可让Spin的其他核开始工作。

## 思考题 5

不需要，因为`unlock`的时机是退出内核态之前，此时该保存的寄存器已经保存好了，而退出内核前会恢复进入内核态时保存的寄存器，通用寄存器本来也将被破坏。因此`unlock`没有必要保存和恢复寄存器。

## 思考题 6

因为IDLE线程在等待队列非空时不应该被调度，而加入到队列中由于FIFO策略总会被调度到的。这可能会浪费CPU时间片，而且不符合IDLE调度时机的设定。正确的语义应当是当等待队列为空时主动调度IDLE线程。

## 思考题 8

异常可能导致程序直接终止，也可能回到出现异常的指令重试。中断会回到出现中断的指令的下一条指令继续。

## 旧Bug

`lab4` Debug过程中解决了了`lab 3`残留的Bug。在`lab3 T2`的`debug`过程中提到到弄错了`TYPE_VMSPACE`和`VMSPACE_OBJ_ID`这两个宏，在`lab3`中修复了`create_root_cap_group`，但是没有Fix `sys_create_cap_group`中的相同Bug。似乎`lab3`测试脚本也没有测试这个函数。