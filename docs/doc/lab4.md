# Report on lab4

- `Li Yiyan 519021911103`
- `lyy0627@sjtu.edu.cn`

## 思考题

### 思考题 1
```assembly
	# MPDIR_EL1 是core identification Register
	# 四核处理器的CPUID 是0x0-0x3, 移动到tmp register X8
	mrs	x8, mpidr_el1
	# 和0对比，只用CPU ID = 0才能跳到primary分支，其他核则会继续执行;
	# 跳转的即为主CPU
	cbz	x8, primary
	# 下面是个循环结构，直到bss clear之后才会继续运行程序，
	# 否则CPU ID != 0的核会在此阻塞
wait_for_bss_clear:
	# ...
	bne	wait_for_bss_clear
	# 阻塞后会改变EL Level, 准备SP
	# 然后同样的循环结构阻塞直到smp被开启进入多核模式
wait_until_smp_enabled:
	/* 直到CPUID = 0的核打开SMP, 否则其他核将在此阻塞 */
	cbz	x3, wait_until_smp_enabled
```

### 思考题 2

`secondary_boot_flag`是物理地址，因为其他核心在Spin的时候还没有初始化页表，无法完成地址翻译。其定义是在`init_c.c`中（被定义时尚未开启MMU），作为参数传入了`start_kernel`, 然后`start_kernel`在调用`kernel/arch/aarch64/main.c`的` bl main`将其函数开头保存的`x0`重新存回了`x0`(即保存第一个参数并在调用`main`前Restore)。然后`main`中再将其传入了`enable_smp_cores`进行赋值。

`secondary_boot_flag`**被赋值的时候CPU0已经设置好了页表**，**因此需要将其先转换成虚拟地址**，然后基于CPU ID 的偏移量赋值即可让Spin的其他核开始工作。

### 思考题 5

不需要，因为`unlock`的时机是退出内核态之前，此时该保存的寄存器已经保存好了，而退出内核前会恢复进入内核态时保存的寄存器，通用寄存器本来也将被破坏。因此`unlock`没有必要保存和恢复寄存器。

### 思考题 6

因为IDLE线程在等待队列非空时不应该被调度，而加入到队列中由于FIFO策略总会被调度到的。这可能会浪费CPU时间片，而且不符合IDLE调度时机的设定。**正确的语义应当是 *当且仅当* 等待队列为空时才调度IDLE线程。**

### 思考题 8

因为正常进入内核都要内核锁，但是运行IDLE进程不需要拿内核锁，只需要调度即可进入内核态。那么出现中断异常之后可能会`eret`(比如调度到新入队的进程)，此时放锁可能会导致错误抛出，导致内核被永远阻塞。

## 思路 & Debug

### 旧Bug

`lab4` Debug过程中解决了了`lab 3`残留的Bug。在`lab3 T2`的`debug`过程中提到到弄错了`TYPE_VMSPACE`和`VMSPACE_OBJ_ID`这两个宏，在`lab3`中修复了`create_root_cap_group`，但是没有Fix `sys_create_cap_group`中的相同Bug。似乎`lab3`测试脚本也没有测试这个函数。

### T3

- 需要将`secondary_boot_flag`设置为非0值以取消其他CPU的阻塞
- 在刷新缓存之后，显式的等待(忙等)其他CPU，然后输出Log
- 相应的，新启动的核需要设置`cpu_status`让主核取消阻塞

### T4

- 排号锁只需要在放锁的时候(不需要原子操作)地将owner传给下一个线程即可，拿号使用的是原子指令
- 在需要进入内核前拿锁，出内核前放锁。
  - 出内核调用`eret`被`exception_exit`所包装。
  - 而在`exception_exit`中会restore进入内核时保存的寄存器。
  - 因此，需要在`exception_exit`前unlock，而非eret前。这样能少保存一次寄存器。
- 此外，在标注了`Do not unlock!`的地方不需要放锁。

### T7

- 使用链表来维护Deque, `list_entry`来从字段找对象指针
- 根据测试用例，`sched_dequeue`应当在`choose_thread`中被调用
- 应该注意维护状态，在一个操作被拆分为多个函数的情况下，设置TS_INTER便于Debug
- (面向测试用例)注意函数的鲁棒性

### T9

- 调度并退出内核态即可

### T10-11

- 简单启用Timer即可
- 根据测试用例语义，在预算为0还进入Timer Handler是非法的，在Handler中失去所有预算后应当让出CPU
- (面向测试用例)注意函数的鲁棒性

### T12

- 设置Aff是可能造成竞争的，事实上，本Lab到这题才会导致无锁过不了测试
- 由于Syscall时应当是拿着内核锁的，所以可以放心的根据语义调整亲和性：
  - 如果线程正在某个CPU上跑，调整亲和性，下次调度时再到对应核
  - 如果线程正在等待，需要退出队列，调整亲和性后加入
  - 如果线程没有等待，直接调整亲和性即可

### T13

- 根据头文件定义，stack分配内存应该分配`PMO_ANONYM`
- offset提示了时真实的栈顶，那么初始化需要写0x1000B, 因此`MAIN_THREAD_STACK_SIZE - 0x1000`
- `args.stack `不是Cap而是栈顶位置，这个Bug需要仔细找Page fault异常前的指令做出判断

### T14

- Client buffer: 每个Client占据相等的Buffer, 起始地址由Client id决定，Buffer之间不重叠
- Server Stack/Buffer: 根据`conn_idx`来确定栈顶/Buffer的起始位置

### T15-17

感觉做的比较顺利，按照语义完成信号量，并利用信号量完成生产者-消费者模型，并且完成阻塞互斥锁即可。