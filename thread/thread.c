#include "thread.h"
#include "string.h"
#include "memory.h"
#include "debug.h"
#include "interrupt.h"

task_struct *main_thread;       //主线程的task_struct
list ready_thread_list;         //就绪任务队列
list all_thread_list;           //所有任务队列

typedef enum __node_type {
    GENERAL_LIST_NODE, 
    ALL_LIST_NODE
} node_type;


static void kthread(thread_func func, void *arg) {
    func(arg);
}

//设置main函数为主线程
static void make_main_thread() {
    main_thread = running_thread();
    task_struct_init(main_thread, "main thread", 32);

    ASSERT(list_exist(&all_thread_list, &main_thread->all_list_tag) == false);
    list_push_back(&all_thread_list, &main_thread->all_list_tag);
}

static task_struct *node_to_task(list_node *node, node_type type) {
    task_struct temp;
    uint32_t node_addr = 0;
    if (type == GENERAL_LIST_NODE) {
        node_addr = (uint32_t)&temp.gene_list_tag;
    } else {
        node_addr = (uint32_t)&temp.all_list_tag;
    }
    uint32_t offset = node_addr - (uint32_t)&temp;
    return (task_struct*)((int)node - offset);
}

//初始化task_struct
void task_struct_init(task_struct *thread, char *name, int prio) {
    ASSERT(thread != NULL);
    memset(thread, 0, sizeof(thread->kstack));
    strcpy(thread->name, name);
    thread->status = (thread == main_thread) ? RUNNING : READY;
    thread->priority = prio;
    thread->ava_time = prio;
    thread->elapsed_time = 0;
    thread->page_dir = NULL;
    thread->stack_magic_num = 0x19980312;
    //让kstack指向task_struct指向的页的顶部
    thread->kstack = (uint32_t*)((uint32_t)thread + ((1 << 12)));   
}

//创建线程，对task_struct中的thread_stack进行初始化
void thread_create(task_struct *thread, 
                   thread_func func, 
                   void *func_args) {
    //PCB顶部为中断栈和线程栈
    thread->kstack -= sizeof(intr_stack);    
    thread->kstack -= sizeof(thread_stack);

    //设置线程内核栈
    thread_stack *kthread_stack = (thread_stack*)thread->kstack;
    kthread_stack->ebp = 0;
    kthread_stack->ebx = 0;
    kthread_stack->edi = 0;
    kthread_stack->esi = 0;
    kthread_stack->eip = (uint32_t)kthread; //设置返回地址，ret后会跳转到kthread函数
    kthread_stack->retaddr = 0;
    kthread_stack->func = func;
    kthread_stack->func_arg = func_args;
}

//新建一个线程并开始运行
task_struct *thread_start(char *name, 
                          int prio, 
                          thread_func func, 
                          void *func_arg) {
    //为线程PCB申请一页内存，4KB
    task_struct *thread = (task_struct*)get_kern_pages(1);
    task_struct_init(thread, name, prio);
    thread_create(thread, func, func_arg);

    ASSERT(list_exist(&ready_thread_list, &thread->gene_list_tag) == false);
    ASSERT(list_exist(&all_thread_list, &thread->all_list_tag) == false);

    list_push_back(&ready_thread_list, &thread->gene_list_tag);
    list_push_back(&all_thread_list, &thread->all_list_tag);

    return thread;
}

//获取当前处于RUNNING状态的线程
//当前esp指向的内核栈所在页，就是当前task_struct指向的地方
task_struct *running_thread() {
    uint32_t esp;
    asm ("mov %%esp, %0;" : "=g" (esp));
    return (task_struct*)(esp & 0xfffff000);
}

//任务调度函数
void schedule() {
    ASSERT(intr_get_status() == INTR_OFF);

    task_struct *curr_thread = running_thread();

    //线程时间片耗尽
    if (curr_thread->status == RUNNING) {
        ASSERT(list_exist(&ready_thread_list, &curr_thread->gene_list_tag) == false);
        list_push_back(&ready_thread_list, &curr_thread->gene_list_tag);
        curr_thread->ava_time = curr_thread->priority;
        curr_thread->elapsed_time = 0;
        curr_thread->status = READY;
    } else {
        //todo
    }

    ASSERT(list_empty(&ready_thread_list) == false);
    list_node *temp_node = list_pop_front(&ready_thread_list);
    task_struct *next_thread = node_to_task(temp_node, GENERAL_LIST_NODE);
    next_thread->status = RUNNING;
    switch_to(curr_thread, next_thread);
}

//保存curr的寄存器上下文，并将next的寄存器映像加载到寄存器中
void switch_to(task_struct *curr, task_struct *next) {
    asm volatile (
        "push %esi;"
        "push %edi;"
        "push %ebx;"
        "push %ebp;"
        "movl 20(%esp), %eax;" // curr = [esp+20]
        "movl %esp, (%eax);"      //保存栈指针在当前PCB的开头
        
        "movl 24(%esp), %eax;" // next = [esp+24]
        "movl (%eax), %esp;"      //设置栈指针
        "popl %ebp;"
        "popl %ebx;"
        "popl %edi;"
        "popl %esi;"
        "ret;"
    );
}

//做线程方面的初始化工作，设置主线程
void thread_init() {
    list_init(&ready_thread_list);
    list_init(&all_thread_list);
    make_main_thread();
}