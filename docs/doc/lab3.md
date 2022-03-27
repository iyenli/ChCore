# Report on Lab 3

- 李逸岩 519021911103
- `lyy0627@sjtu.edu.cn`.

## 1

从内核初始化转入第一个用户态进程是在PID=1的`init`进程中完成的。在完成一系列初始化后，0号进程会创建1号进程init. 该进程会设置SP, EL等寄存器进入用户态并且继续运行。由inity进程Fork出来的进程都将在用户态运行。就完成了从内核态转变到用户态的过程，转变后，init进程还在继续运行，执行必要的用户态初始化。然后将控制流交给登录或shell等用户态程序。

## 2

数据结构关系如下：

- object, 是对内核资源的抽象，包括ref-count, type等。opaque字段是0长度数组，好处是可以根据分配Struct的大小动态伸缩，保持8对齐。Object可以是：进程，线程，IPC，PMO, VMSpace, Semaphore等。分配`object`时，指定类型和`opaque`的大小，就会自动分配出`opaque + meta`的大小，并且返回`opaque`.
- object slot, 是object插槽。slot id是下标，还包括权限，有效位，所处进程等. slot table是插槽数组，还包括Bit map和大小。

Thread和Cap_group在实验文档中介绍了。下面是一些工具函数：

- `cap_alloc`: `container_of` macro获得object的头部。分配slot id后组装slot并且插入到cap_group中。
- `cap_copy`: 将某个cap_group的特定槽复制安装到另一个, `cap_move`则为移动
- `sys_cap_copy_from`则是从src移动到*当前* cap_group上, `sys_cap_move`则是从当前移动到dest上

对于`cap_group_init`初始化`thread_list`, `slot_table`. 对`sys_create_cap_group`要分配`object`. 初始化之后作为cap分配给当前进程，cap_group本身作为新进程的第一个资源，初始化VM作为新进程的第二个资源。对于`create_root_cap_group`，无需分配给当前进程。

主要解决的BUG就是搞错宏的名字了`TYPE_VMSPACE` & `VMSPACE_OBJ_ID`，导致运行过程中出现了Page Fault. 解决后，获得了该部分的10分，并顺利运行到下一函数。

## 3

每个section都需要分配一个pmo. 按照代码提示，还需要记录`slot id`以在失败后及时释放资源。此外，要注意页对齐问题，无论起始地址还是终止地址都应该保持页对齐，实际映射的大小为对齐后的首尾地址差。主要需要理解`vmregion`. 其是vmspace的链成员，和pmo一一对应。分配一个pmo就要分配一个vmregion在vmsapce中。一开始疑惑于为什么不直接把pmo映射给实际的vaddr, 而是先kmalloc一段再建立类似虚拟地址-虚拟地址的映射。后来在page_fault_handler中才解决了这个疑惑。

在完成这个部分之后，`[ChCore] create initial thread done on 0`创建了第一个线程。

## 4

按照表格填写。需要使用到`macro exception_entry`. 作用是帮助0x80对齐。然后错误定义位于`irq_entry.h`. 顺序严格按照表填写，`h/t/el0_32/el0_64`在注释和文档中已经标注。然后需要填写处理函数。按照文档调用即可。



## 5





## 6



## 7



## 8





## 9



